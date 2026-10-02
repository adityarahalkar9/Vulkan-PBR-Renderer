#pragma once
// ImGui debug front end: performance graphs, render settings, GPU/memory
// architecture inspection, scene inspector and the asset download manager.

#include "core/types.hpp"

#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <cstdint>
#include <map>

class VulkanContext;
class Swapchain;
class Scene;
class AssetManager;
class GpuProfiler;
class JobSystem;
struct GLFWwindow;

class DebugUi {
public:
    void init(GLFWwindow* window, VulkanContext& ctx, Swapchain& swapchain, Scene& scene,
              AssetManager& assets, GpuProfiler& profiler, JobSystem& jobs,
              Settings& settings, FrameStats& stats);
    void syncBackend(); // re-init the ImGui Vulkan backend if the swapchain changed
    void draw(float dt);
    void shutdown();

private:
    void initBackend();
    ImTextureID thumb(uint32_t textureId);
    void drawPerformance(float dt);
    void drawSettings();
    void drawGpuMemory();
    void drawAssets();
    void drawScene();

    GLFWwindow* window_ = nullptr;
    VulkanContext* ctx_ = nullptr;
    Swapchain* swapchain_ = nullptr;
    Scene* scene_ = nullptr;
    AssetManager* assets_ = nullptr;
    GpuProfiler* profiler_ = nullptr;
    JobSystem* jobs_ = nullptr;
    Settings* settings_ = nullptr;
    FrameStats* stats_ = nullptr;

    VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;
    VkFormat lastFormat_ = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits lastSamples_ = VK_SAMPLE_COUNT_1_BIT;
    uint32_t lastImageCount_ = 0;
    std::map<uint32_t, ImTextureID> thumbs_;
    char url_[512] = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb";
};