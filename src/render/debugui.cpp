#include "render/debugui.hpp"

#include "assets/assetmanager.hpp"
#include "core/jobsystem.hpp"
#include "render/gpuprofiler.hpp"
#include "render/swapchain.hpp"
#include "render/vulkancontext.hpp"
#include "scene/scene.hpp"

#include <backends/imgui_impl_glfw.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

    void checkVk(VkResult err) {
        if(err != VK_SUCCESS)
            throw std::runtime_error(std::format("imgui vulkan error {}", static_cast<int>(err)));
    }

    const char* presentModeName(VkPresentModeKHR m) {
        switch(m) {
            case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
            case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
            default: return "other";
        }
    }

    std::string humanBytes(VkDeviceSize v) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        double f = static_cast<double>(v);
        int u = 0;
        while(f >= 1024.0 && u < 4) { f /= 1024.0; ++u; }
        return std::format("{:.2f} {}", f, units[u]);
    }

    void plotRing(const char* label, const Ring<240>& ring, const char* overlayFmt, float scaleMax) {
        float values[240];
        ring.copyTo(values);
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), overlayFmt, ring.latest());
        ImGui::PlotLines(label, values, static_cast<int>(ring.size()), 0, overlay, 0.0f, scaleMax,
                         ImVec2(0.0f, 42.0f));
    }

    const char* deviceTypeName(VkPhysicalDeviceType t) {
        switch(t) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
            default: return "other";
        }
    }

} // namespace

void DebugUi::init(GLFWwindow* window, VulkanContext& ctx, Swapchain& swapchain, Scene& scene,
                   AssetManager& assets, GpuProfiler& profiler, JobSystem& jobs,
                   Settings& settings, FrameStats& stats) {
    window_ = window;
    ctx_ = &ctx;
    swapchain_ = &swapchain;
    scene_ = &scene;
    assets_ = &assets;
    profiler_ = &profiler;
    jobs_ = &jobs;
    settings_ = &settings;
    stats_ = &stats;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;

    ImGui_ImplGlfw_InitForVulkan(window_, true);
    initBackend();
}

void DebugUi::initBackend() {
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets = 256;
    ci.poolSizeCount = 1;
    ci.pPoolSizes = &poolSize;
    checkVk(vkCreateDescriptorPool(ctx_->device, &ci, nullptr, &imguiPool_));

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_4;
    info.Instance = ctx_->instance;
    info.PhysicalDevice = ctx_->physicalDevice;
    info.Device = ctx_->device;
    info.QueueFamily = ctx_->graphicsFamily;
    info.Queue = ctx_->graphicsQueue;
    info.DescriptorPool = imguiPool_;
    info.MinAllocationSize = 1024 * 1024;
    info.MSAASamples = swapchain_->sampleCount; // must match the render pass attachments
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchain_->format;
    info.CheckVkResultFn = checkVk;
    if(!ImGui_ImplVulkan_Init(&info))
        throw std::runtime_error("ImGui Vulkan backend init failed");

    lastFormat_ = swapchain_->format;
    lastSamples_ = swapchain_->sampleCount;
    lastImageCount_ = swapchain_->imageCount();
}

void DebugUi::syncBackend() {
    if(swapchain_->format == lastFormat_ && swapchain_->sampleCount == lastSamples_ &&
       swapchain_->imageCount() == lastImageCount_)
        return;

    vkDeviceWaitIdle(ctx_->device);
    for(auto& [id, tex] : thumbs_)
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(tex));
    thumbs_.clear();
    ImGui_ImplVulkan_Shutdown();
    vkDestroyDescriptorPool(ctx_->device, imguiPool_, nullptr);
    imguiPool_ = VK_NULL_HANDLE;
    initBackend();
    LOG_INFO("imgui vulkan backend re-initialized (format/samples/image count changed)");
}

ImTextureID DebugUi::thumb(uint32_t textureId) {
    auto it = thumbs_.find(textureId);
    if(it != thumbs_.end()) return it->second;
    if(textureId >= assets_->textures.size()) return nullptr;
    TextureAsset* t = assets_->textures[textureId].get();
    if(t->state != static_cast<int>(AssetState::Ready) || !t->view) return nullptr;
    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(t->sampler, t->view,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImTextureID id = reinterpret_cast<ImTextureID>(set);
    thumbs_.emplace(textureId, id);
    return id;
}

void DebugUi::draw(float dt) {
    drawPerformance(dt);
    drawSettings();
    drawGpuMemory();
    drawAssets();
    drawScene();
}

void DebugUi::drawPerformance(float dt) {
    ImGui::SetNextWindowSize(ImVec2(430, 560), ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("Performance")) { ImGui::End(); return; }

    const FrameStats& s = *stats_;
    ImGui::Text("FPS %.1f   frame %.2f ms   frame #%llu", s.fps, s.frameMs.latest(),
                static_cast<unsigned long long>(s.frameIndex));
    ImGui::Separator();

    if(ImGui::TreeNode("CPU")) {
        plotRing("frame (ms)##cpuframe", s.frameMs, "%.2f ms", 0.0f);
        plotRing("update (ms)##cpuupdate", s.cpuUpdateMs, "%.2f ms", 0.0f);
        plotRing("imgui (ms)##cpuimgui", s.cpuImGuiMs, "%.2f ms", 0.0f);
        plotRing("record+submit (ms)##cpurender", s.cpuRenderMs, "%.2f ms", 0.0f);
        ImGui::TreePop();
    }
    if(ImGui::TreeNode("GPU (timestamp queries)")) {
        plotRing("shadow (ms)##gpushadow", s.gpuShadowMs, "%.2f ms", 0.0f);
        plotRing("geometry (ms)##gpuopaque", s.gpuOpaqueMs, "%.2f ms", 0.0f);
        plotRing("ui (ms)##gpuui", s.gpuUiMs, "%.2f ms", 0.0f);
        plotRing("total (ms)##gputotal", s.gpuTotalMs, "%.2f ms", 0.0f);
        ImGui::Text("timestamp period %.1f ns", ctx_->props.timestampPeriod);
        ImGui::TreePop();
    }
    ImGui::Separator();
    if(ImGui::BeginTable("counters", 2)) {
        auto row = [](const char* k, auto v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k);
            ImGui::TableNextColumn();
            ImGui::Text("%s", std::format("{}", v).c_str());
            };
        row("draw calls", s.drawCalls);
        row("triangles", s.triangles);
        row("entities visible/total", std::format("{}/{}", s.entitiesVisible, s.entitiesTotal));
        row("shadow casters", s.shadowCasters);
        row("job threads", std::format("{} ({} active / {} pending)",
                                       jobs_->threadCount(), jobs_->active(), jobs_->pending()));
        ImGui::EndTable();
    }
    ImGui::Text("frame time budget: %.2f%% of 16.6 ms", s.frameMs.latest() / 16.6f * 100.0f);
    (void)dt;
    ImGui::End();
}

void DebugUi::drawSettings() {
    if(!ImGui::Begin("Render Settings")) { ImGui::End(); return; }
    Settings& s = *settings_;

    ImGui::Checkbox("shadows", &s.shadows);
    ImGui::Checkbox("sky", &s.sky);
    ImGui::Checkbox("wireframe", &s.wireframe);
    ImGui::Checkbox("frustum culling", &s.frustumCulling);
    ImGui::Checkbox("animate lights", &s.animateLights);
    ImGui::Separator();

    static const int shadowResolutions[] = {1024, 2048, 4096, 8192};
    int resIndex = 1;
    for(int i = 0; i < 4; ++i)
        if(static_cast<int>(s.shadowResolution) == shadowResolutions[i]) resIndex = i;
    if(ImGui::Combo("shadow resolution", &resIndex, "1024\02048\04096\08192\0")) {
        s.shadowResolution = static_cast<uint32_t>(shadowResolutions[resIndex]);
        s.shadows = true;
    }
    ImGui::SliderFloat("bias constant", &s.shadowBiasConstant, 0.0f, 8.0f);
    ImGui::SliderFloat("bias slope", &s.shadowBiasSlope, 0.0f, 8.0f);
    ImGui::SliderFloat("normal offset", &s.shadowNormalOffset, 0.0f, 4.0f);
    ImGui::Separator();

    ImGui::SliderFloat("exposure", &s.exposure, 0.1f, 4.0f, "%.2f");
    ImGui::SliderFloat("ambient", &s.ambientIntensity, 0.0f, 2.0f);
    ImGui::Separator();

    ImGui::SliderAngle("sun azimuth", &s.sunAzimuth, 0.0f, 360.0f);
    ImGui::SliderFloat("sun elevation", &s.sunElevation, 0.02f, 1.5f);
    ImGui::SliderFloat("sun intensity", &s.sunIntensity, 0.0f, 10.0f);
    ImGui::ColorEdit3("sun color", &s.sunColor.x);
    ImGui::Separator();

    const auto& modes = swapchain_->availablePresentModes();
    if(ImGui::BeginCombo("present mode", presentModeName(modes[static_cast<size_t>(
        std::clamp<int>(s.presentMode, 0, static_cast<int>(modes.size()) - 1))]))) {
        for(int i = 0; i < static_cast<int>(modes.size()); ++i)
            if(ImGui::Selectable(presentModeName(modes[static_cast<size_t>(i)]), i == s.presentMode))
                s.presentMode = i;
        ImGui::EndCombo();
    }

    static const uint32_t sampleOptions[] = {1, 2, 4, 8};
    static const char* sampleNames[] = {"off (1x)", "2x MSAA", "4x MSAA", "8x MSAA"};
    int msaaIndex = 0;
    for(int i = 0; i < 4; ++i)
        if(sampleOptions[i] == s.msaaRequest) msaaIndex = i;
    if(ImGui::Combo("MSAA", &msaaIndex, sampleNames, 4)) {
        uint32_t want = sampleOptions[msaaIndex];
        if(want > 1 && !(ctx_->supportedSamples & want))
            LOG_WARN("{}x MSAA not supported by this device; clamping", want);
        s.msaaRequest = want;
    }
    ImGui::Text("device sample bits: 0x%x", static_cast<unsigned>(ctx_->supportedSamples));
    ImGui::End();
}

void DebugUi::drawGpuMemory() {
    if(!ImGui::Begin("GPU & Memory")) { ImGui::End(); return; }
    const auto& p = ctx_->props;

    ImGui::Text("device: %s (%s)", p.deviceName, deviceTypeName(p.deviceType));
    ImGui::Text("api %u.%u.%u   driver %u   vendor 0x%x",
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID);
    ImGui::Text("Vulkan 1.4 features: pushDescriptor=yes (%u max), dynamicRenderingLocalRead=%s",
                p.limits.maxPushDescriptors, ctx_->supportsDynamicRenderingLocalRead ? "yes" : "no");
    ImGui::Separator();

    if(ImGui::TreeNode("queue families")) {
        if(ImGui::BeginTable("queues", 4, ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("index");
            ImGui::TableSetupColumn("count");
            ImGui::TableSetupColumn("flags");
            ImGui::TableSetupColumn("present");
            ImGui::TableHeadersRow();
            for(size_t i = 0; i < ctx_->queueFamilies.size(); ++i) {
                const auto& q = ctx_->queueFamilies[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                ImGui::TableNextColumn(); ImGui::Text("%u", q.count);
                ImGui::TableNextColumn(); ImGui::Text("0x%x", static_cast<unsigned>(q.flags));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(q.present ? "yes" : "no");
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }

    if(ImGui::TreeNode("memory heaps (live budgets)")) {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(ctx_->allocator, budgets);
        for(uint32_t i = 0; i < ctx_->memProps.memoryHeapCount; ++i) {
            const auto& heap = ctx_->memProps.memoryHeaps[i];
            bool deviceLocal = heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
            VmaBudget& b = budgets[i];
            ImGui::Text("heap %u (%s): %s", i, deviceLocal ? "DEVICE_LOCAL" : "host",
                        humanBytes(heap.size).c_str());
            float usedFraction = b.budget > 0 ? static_cast<float>(b.usage) / static_cast<float>(b.budget)
                : 0.0f;
            ImGui::ProgressBar(usedFraction, ImVec2(-1, 0),
                               std::format("usage {} / budget {}", humanBytes(b.usage),
                                           humanBytes(b.budget)).c_str());
            ImGui::Text("  VMA allocations: {} totaling {}", b.statistics.allocationCount,
                        humanBytes(b.statistics.allocationBytes).c_str());
        }
        if(!ctx_->supportsMemoryBudget)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                               "VK_EXT_memory_budget unavailable: budget values are approximations");
        ImGui::TreePop();
    }

    if(ImGui::TreeNode("memory types")) {
        if(ImGui::BeginTable("types", 3, ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("index");
            ImGui::TableSetupColumn("heap");
            ImGui::TableSetupColumn("property flags");
            ImGui::TableHeadersRow();
            for(uint32_t i = 0; i < ctx_->memProps.memoryTypeCount; ++i) {
                const auto& t = ctx_->memProps.memoryTypes[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", i);
                ImGui::TableNextColumn(); ImGui::Text("%u", t.heapIndex);
                ImGui::TableNextColumn();
                std::string flags;
                if(t.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) flags += "DEVICE_LOCAL ";
                if(t.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) flags += "HOST_VISIBLE ";
                if(t.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) flags += "HOST_COHERENT ";
                if(t.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) flags += "HOST_CACHED ";
                if(t.propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) flags += "LAZY ";
                ImGui::TextUnformatted(flags.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }

    if(ImGui::TreeNode("selected limits")) {
        ImGui::Text("maxPushDescriptors: %u", p.limits.maxPushDescriptors);
        ImGui::Text("timestampPeriod: %.1f ns", p.timestampPeriod);
        ImGui::Text("maxSamplerAnisotropy: %.0f", p.limits.maxSamplerAnisotropy);
        ImGui::Text("maxImageDimension2D: %u", p.limits.maxImageDimension2D);
        ImGui::Text("maxMemoryAllocationCount: %u", p.limits.maxMemoryAllocationCount);
        ImGui::Text("bufferImageGranularity: %llu",
                    static_cast<unsigned long long>(p.limits.bufferImageGranularity));
        ImGui::Text("minUniformBufferOffsetAlignment: %llu",
                    static_cast<unsigned long long>(p.limits.minUniformBufferOffsetAlignment));
        ImGui::TreePop();
    }
    ImGui::End();
}

void DebugUi::drawAssets() {
    if(!ImGui::Begin("Assets")) { ImGui::End(); return; }

    if(ImGui::CollapsingHeader("downloads (HTTP -> parallel pipeline)")) {
        static const char* presetNames[] = {"DamagedHelmet", "Avocado", "BoomBox"};
        static const char* presetUrls[] = {
            "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb",
            "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Avocado/glTF-Binary/Avocado.glb",
            "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/BoomBox/glTF-Binary/BoomBox.glb",
        };
        static int preset = 0;
        if(ImGui::Combo("preset", &preset, presetNames, 3))
            std::snprintf(url_, sizeof(url_), "%s", presetUrls[preset]);
        ImGui::InputText("url", url_, sizeof(url_));
        ImGui::SameLine();
        if(ImGui::Button("download") && url_[0])
            assets_->downloadModel(url_);

        for(auto& d : assets_->downloads) {
            ImGui::PushID(d.get());
            int phase = d->phase.load();
            if(phase == 1) {
                uint64_t done = d->done.load(), total = d->total.load();
                float frac = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                ImGui::ProgressBar(frac, ImVec2(-1, 0),
                                   std::format("{} / {}", humanBytes(done), humanBytes(total)).c_str());
            }
            else if(phase == 2) {
                ImGui::Text("parsing GLB...");
            }
            else if(phase == 3) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "done");
            }
            else if(phase == 4) {
                ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1), "failed");
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled("tip: you can also drag & drop .glb/.gltf files onto the window");
    }

    if(ImGui::CollapsingHeader("load requests")) {
        for(auto& r : assets_->requests) {
            ImGui::BulletText("%s: %s", r->name.c_str(), stateName(static_cast<AssetState>(r->state.load())));
            if(r->failed() && !r->errorString().empty())
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r->errorString().c_str());
        }
    }

    if(ImGui::CollapsingHeader("models")) {
        for(auto& m : assets_->models) {
            ImGui::BulletText("%s (%s): %u verts / %u indices, %zu primitives",
                              m->name.c_str(), stateName(m->state), m->vertexCount, m->indexCount,
                              m->primitives.size());
        }
    }

    if(ImGui::CollapsingHeader("materials")) {
        for(auto& m : assets_->materials) {
            ImGui::BulletText("%s  m=%.2f r=%.2f  flags=0x%x", m->name.c_str(),
                              m->metallic, m->roughness, m->flags);
        }
    }

    if(ImGui::CollapsingHeader("textures")) {
        for(uint32_t i = 0; i < assets_->textures.size(); ++i) {
            auto& t = assets_->textures[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            ImTextureID id = thumb(i);
            if(id) ImGui::Image(id, ImVec2(48, 48));
            else ImGui::Dummy(ImVec2(48, 48));
            ImGui::EndGroup();
            ImGui::SameLine();
            int state = t->state.load();
            ImGui::Text("[%u] %s\n%s  %ux%u, %u mips%s", i, t->name.c_str(), stateName(static_cast<AssetState>(state)),
                        t->width, t->height, t->mipLevels, t->srgb ? ", sRGB" : "");
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    if(ImGui::Button("rebuild demo scene"))
        assets_->createDefaultScene();
    ImGui::SameLine();
    if(ImGui::Button("clear scene"))
        scene_->clear();
    ImGui::End();
}

void DebugUi::drawScene() {
    if(!ImGui::Begin("Scene")) { ImGui::End(); return; }
    const Scene& s = *scene_;

    ImGui::Text("entities: %zu   bounds: [%.1f %.1f %.1f] .. [%.1f %.1f %.1f]", s.entities.size(),
                s.bounds.min.x, s.bounds.min.y, s.bounds.min.z,
                s.bounds.max.x, s.bounds.max.y, s.bounds.max.z);

    if(ImGui::CollapsingHeader("entity list")) {
        const size_t shown = std::min<size_t>(s.entities.size(), 400);
        for(size_t i = 0; i < shown; ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox("##vis", &s.entities[i].visible);
            ImGui::SameLine();
            ImGui::Text("%zu: %s", i, s.entities[i].name.c_str());
            ImGui::PopID();
        }
        if(s.entities.size() > shown)
            ImGui::TextDisabled("... %zu more", s.entities.size() - shown);
    }

    if(ImGui::CollapsingHeader("point lights")) {
        for(size_t i = 0; i < s.lights.size(); ++i) {
            const auto& l = s.lights[i];
            ImGui::BulletText("[%zu] (%.1f, %.1f, %.1f)  I=%.1f  r=%.1f", i, l.pos.x, l.pos.y, l.pos.z,
                              l.intensity, l.radius);
        }
    }
    ImGui::End();
}

void DebugUi::shutdown() {
    if(!ImGui::GetCurrentContext()) return;
    for(auto& [id, tex] : thumbs_)
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(tex));
    thumbs_.clear();
    ImGui_ImplVulkan_Shutdown();
    if(imguiPool_) { vkDestroyDescriptorPool(ctx_->device, imguiPool_, nullptr); imguiPool_ = VK_NULL_HANDLE; }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}