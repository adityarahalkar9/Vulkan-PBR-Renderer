#pragma once
// Frame composition:
//   [shadow pass]   dynamic rendering, depth only, PCF source
//   [main pass]     one dynamic render pass: opaque -> sky -> transparent -> UI
//                   depth stored with VK_ATTACHMENT_STORE_OP_NONE (Vulkan 1.4)
// Materials use push descriptors (Vulkan 1.4 core) - set 1 is never bound
// from a pool, it is pushed per draw with up to 5 combined image samplers.

#include "core/types.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <utility>
#include <vector>

class VulkanContext;
class Swapchain;
class ShadowMap;
class DescriptorAllocator;
class AssetManager;
class GpuProfiler;
class Scene;
class CameraController;
struct ImDrawData;

// Mirrors the SceneUbo uniform block in the shaders (std140).
struct SceneUbo {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 lightViewProj;
    glm::mat4 invViewProj;
    glm::vec4 camPos;
    glm::vec4 sunDir;
    glm::vec4 sunColorIntensity;
    glm::vec4 ambientColor;
    glm::vec4 screenParams;
    glm::vec4 skyZenith;
    glm::vec4 skyHorizon;
    glm::vec4 miscParams;
    glm::vec4 pointPosRadius[8];
    glm::vec4 pointColorIntensity[8];
};

// Mirrors the push_constant block (exactly 128 bytes - the minimum guarantee).
struct PushBlock {
    glm::mat4 model;
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveFactor;
    glm::vec4 params0; // x metallic, y roughness, z alpha cutoff, w flags (bit pattern)
    glm::vec4 params1; // x normal scale, y occlusion strength
};
static_assert(sizeof(PushBlock) == 128);

class Renderer {
public:
    void init(VulkanContext& ctx, Swapchain& swapchain, ShadowMap& shadow,
              DescriptorAllocator& descriptors, GpuProfiler& profiler);
    void shutdown();
    void requestSwapchainRecreate() { needsRecreate_ = true; }
    bool beginFrame();
    void render(Scene& scene, CameraController& camera, AssetManager& assets,
                const Settings& settings, FrameStats& stats, ImDrawData* drawData);

private:
    struct FrameSlot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkSemaphore present = VK_NULL_HANDLE;
        VkBuffer ubo = VK_NULL_HANDLE;
        VmaAllocation uboAllocation = VK_NULL_HANDLE;
        void* uboMapped = nullptr;
        VkDescriptorSet sceneSet = VK_NULL_HANDLE;
    };

    void createLayouts();
    void createFrameSlots();
    void rebuildPipelines(bool wireframe);
    void recreateSwapchain();
    void rebindShadowSampler();
    void updateSceneUbo(Scene& scene, CameraController& camera, const Settings& settings,
                        const glm::mat4& lightVP, float shadowTexelWorldSize);
    glm::mat4 computeLightMatrix(const Scene& scene, const Settings& settings, float& spanOut);
    void pushMaterial(VkCommandBuffer cb, VkPipelineLayout layout,
                      const MaterialAsset& material, AssetManager& assets, bool shadowVariant);
    VkShaderModule loadShader(const char* file);

    VulkanContext* ctx_ = nullptr;
    Swapchain* swapchain_ = nullptr;
    ShadowMap* shadow_ = nullptr;
    DescriptorAllocator* descriptors_ = nullptr;
    GpuProfiler* profiler_ = nullptr;

    static constexpr uint32_t kFrames = 2;
    FrameSlot frames_[kFrames];
    uint32_t frameIndex_ = 0;
    uint32_t imageIndex_ = 0;
    bool needsRecreate_ = false;
    bool lastWireframe_ = false;

    VkDescriptorSetLayout sceneLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;      // push descriptor, 5 samplers
    VkDescriptorSetLayout shadowMaterialLayout_ = VK_NULL_HANDLE; // push descriptor, 1 sampler
    VkPipelineLayout pbrLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout skyLayout_ = VK_NULL_HANDLE;

    VkPipeline pipeOpaque_ = VK_NULL_HANDLE;
    VkPipeline pipeDoubleSided_ = VK_NULL_HANDLE;
    VkPipeline pipeBlend_ = VK_NULL_HANDLE;
    VkPipeline pipeShadow_ = VK_NULL_HANDLE;
    VkPipeline pipeSky_ = VK_NULL_HANDLE;

    SceneUbo uboData_{};
    std::vector<const Entity*> visible_;
    std::vector<const Entity*> casters_;
    std::vector<std::pair<float, const Entity*>> blendSorted_;
};