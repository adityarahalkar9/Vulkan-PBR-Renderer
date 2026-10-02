#pragma once
// Owns every subsystem and drives the main loop.

#include "core/types.hpp"

#include <GLFW/glfw3.h>

#include <memory>
#include <string>
#include <vector>

class VulkanContext;
class Swapchain;
class UploadQueue;
class DescriptorAllocator;
class ShadowMap;
class GpuProfiler;
class Renderer;
class AssetManager;
class Scene;
class CameraController;
class DebugUi;
class JobSystem;

class Application {
public:
    int run();

private:
    void initSystems();
    void shutdownSystems();
    void frame(float dt);
    void pollDroppedFiles();
    void checkSwapchainSettings();

    GLFWwindow* window_ = nullptr;
    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<VulkanContext> context_;
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<UploadQueue> uploads_;
    std::unique_ptr<DescriptorAllocator> descriptors_;
    std::unique_ptr<ShadowMap> shadowMap_;
    std::unique_ptr<GpuProfiler> profiler_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<AssetManager> assets_;
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<CameraController> camera_;
    std::unique_ptr<DebugUi> ui_;

    Settings settings_;
    FrameStats stats_;
    double lastTime_ = 0.0;
    uint64_t frameCounter_ = 0;
    int lastPresentMode_ = -1;
    uint32_t lastMsaa_ = 0;
    bool hadSceneEntities_ = false;
};