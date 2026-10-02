#include "core/application.hpp"

#include "assets/assetmanager.hpp"
#include "core/jobsystem.hpp"
#include "core/log.hpp"
#include "render/debugui.hpp"
#include "render/descriptorallocator.hpp"
#include "render/gpuprofiler.hpp"
#include "render/renderer.hpp"
#include "render/shadowmap.hpp"
#include "render/swapchain.hpp"
#include "render/uploadqueue.hpp"
#include "render/vulkancontext.hpp"
#include "scene/camera.hpp"
#include "scene/scene.hpp"

#include <imgui.h>

#include <chrono>
#include <cstdlib>
#include <exception>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using Clock = std::chrono::high_resolution_clock;

namespace {
    std::mutex g_dropMutex;
    std::vector<std::string> g_dropped;

    void glfwError(int code, const char* desc) { LOG_ERROR("glfw error {}: {}", code, desc); }
    void dropCallback(GLFWwindow*, int count, const char** paths) {
        std::scoped_lock lock(g_dropMutex);
        for(int i = 0; i < count; ++i) g_dropped.emplace_back(paths[i]);
    }
} // namespace

int Application::run() {
    try {
        initSystems();
        lastTime_ = glfwGetTime();
        while(!glfwWindowShouldClose(window_)) {
            glfwPollEvents();
            pollDroppedFiles();

            double now = glfwGetTime();
            float dt = static_cast<float>(std::min(now - lastTime_, 0.1));
            lastTime_ = now;

            auto tFrame = Clock::now();
            frame(dt);
            stats_.frameMs.push(std::chrono::duration<float, std::milli>(Clock::now() - tFrame).count());
            stats_.fps = dt > 0.0f ? 1.0f / dt : 0.0f;
            stats_.frameIndex = frameCounter_;
            ++frameCounter_;
        }
        shutdownSystems();
        return 0;
    }
    catch(const std::exception& e) {
        LOG_ERROR("fatal: {}", e.what());
#ifdef _WIN32
        MessageBoxA(nullptr, e.what(), "PbrRenderer - fatal error", MB_ICONERROR | MB_OK);
#endif
        shutdownSystems();
        return 1;
    }
}

void Application::initSystems() {
    logr::init("renderer.log");
    LOG_INFO("PbrRenderer (Vulkan 1.4) starting");

    glfwSetErrorCallback(glfwError);
    if(!glfwInit()) throw std::runtime_error("failed to initialize GLFW");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(1700, 900, "Vulkan 1.4 PBR Renderer", nullptr, nullptr);
    if(!window_) throw std::runtime_error("failed to create window");
    glfwSetDropCallback(window_, dropCallback);

    jobs_ = std::make_unique<JobSystem>();

    bool validation = true;
#ifdef PBR_VALIDATION
    if(const char* v = std::getenv("PBR_VALIDATION")) validation = std::strcmp(v, "0") != 0;
#else
    validation = false;
#endif

    context_ = std::make_unique<VulkanContext>();
    context_->init(window_, validation);

    swapchain_ = std::make_unique<Swapchain>();
    swapchain_->init(*context_, window_);
    settings_.presentMode = swapchain_->presentModeIndex();
    settings_.msaaRequest = swapchain_->sampleCount;
    lastPresentMode_ = settings_.presentMode;
    lastMsaa_ = settings_.msaaRequest;

    shadowMap_ = std::make_unique<ShadowMap>();
    shadowMap_->init(*context_, settings_.shadowResolution);

    descriptors_ = std::make_unique<DescriptorAllocator>();
    descriptors_->init(context_->device, 2);

    profiler_ = std::make_unique<GpuProfiler>();
    profiler_->init(*context_, 2);

    uploads_ = std::make_unique<UploadQueue>();
    uploads_->init(*context_);

    assets_ = std::make_unique<AssetManager>();
    assets_->init(&*context_, &*jobs_, &*uploads_);
    assets_->createDefaultTextures(); // tiny synchronous uploads, uploader thread not started yet

    renderer_ = std::make_unique<Renderer>();
    renderer_->init(*context_, *swapchain_, *shadowMap_, *descriptors_, *profiler_);

    scene_ = std::make_unique<Scene>();
    assets_->setScene(scene_.get());

    camera_ = std::make_unique<CameraController>();
    ui_ = std::make_unique<DebugUi>();
    ui_->init(window_, *context_, *swapchain_, *scene_, *assets_, *profiler_, *jobs_, settings_, stats_);

    uploads_->start();                 // asynchronous uploads from here on
    assets_->createDefaultScene();     // metallic/roughness sphere study
    LOG_INFO("all subsystems initialized");
}

void Application::shutdownSystems() {
    LOG_INFO("shutting down...");
    if(context_) vkDeviceWaitIdle(context_->device);

    if(ui_) ui_->shutdown();
    if(uploads_) uploads_->shutdown();
    jobs_.reset();

    if(context_) vkDeviceWaitIdle(context_->device);
    if(renderer_) renderer_->shutdown();
    if(shadowMap_) shadowMap_->shutdown();
    if(assets_) assets_->shutdown();
    if(descriptors_) descriptors_->shutdown();
    if(profiler_) profiler_->shutdown();
    if(swapchain_) swapchain_->shutdown();
    if(context_) context_->shutdown();

    if(window_) { glfwDestroyWindow(window_); window_ = nullptr; }
    glfwTerminate();
    logr::shutdown();
}

void Application::pollDroppedFiles() {
    std::vector<std::string> files;
    {
        std::scoped_lock lock(g_dropMutex);
        files.swap(g_dropped);
    }
    for(auto& f : files) {
        LOG_INFO("file dropped: {}", f);
        assets_->loadModelFromFile(f);
    }
}

void Application::checkSwapchainSettings() {
    if(settings_.presentMode != lastPresentMode_) {
        lastPresentMode_ = settings_.presentMode;
        swapchain_->setPresentMode(settings_.presentMode);
        renderer_->requestSwapchainRecreate();
    }
    if(settings_.msaaRequest != lastMsaa_) {
        swapchain_->setSamples(settings_.msaaRequest);
        settings_.msaaRequest = swapchain_->sampleCount; // adopt clamped value
        lastMsaa_ = settings_.msaaRequest;
        renderer_->requestSwapchainRecreate();
    }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    if(fbw > 0 && fbh > 0 &&
       (static_cast<uint32_t>(fbw) != swapchain_->extent.width ||
        static_cast<uint32_t>(fbh) != swapchain_->extent.height))
        renderer_->requestSwapchainRecreate();
}

void Application::frame(float dt) {
    auto t0 = Clock::now();
    checkSwapchainSettings();

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    if(fbw == 0 || fbh == 0) return; // minimized

    if(glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    if(glfwGetKey(window_, GLFW_KEY_F1) == GLFW_PRESS && !ImGui::GetIO().WantTextInput)
        settings_.showUi = !settings_.showUi;

    camera_->setAspect(static_cast<float>(fbw) / static_cast<float>(fbh));
    camera_->update(window_, dt);
    assets_->update();
    scene_->update(dt, settings_.animateLights);
    stats_.cpuUpdateMs.push(std::chrono::duration<float, std::milli>(Clock::now() - t0).count());

    if(!renderer_->beginFrame()) return; // swapchain lost / recreated

    ui_->syncBackend(); // re-inits the ImGui Vulkan backend if format/samples changed

    auto t1 = Clock::now();
    ImGui_ImplGlfw_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if(settings_.showUi) ui_->draw(dt);
    ImGui::Render();
    stats_.cpuImGuiMs.push(std::chrono::duration<float, std::milli>(Clock::now() - t1).count());

    auto t2 = Clock::now();
    renderer_->render(*scene_, *camera_, *assets_, settings_, stats_, ImGui::GetDrawData());
    stats_.cpuRenderMs.push(std::chrono::duration<float, std::milli>(Clock::now() - t2).count());

    // Frame the camera once the first entities appear.
    if(!hadSceneEntities_ && !scene_->entities.empty()) {
        hadSceneEntities_ = true;
        camera_->frameScene(scene_->bounds);
    }
}