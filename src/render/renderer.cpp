#include "render/renderer.hpp"
#include "assets/assetmanager.hpp"
#include "core/log.hpp"
#include "render/descriptorallocator.hpp"
#include "render/gpuprofiler.hpp"
#include "render/pipelinebuilder.hpp"
#include "render/shadowmap.hpp"
#include "render/swapchain.hpp"
#include "scene/camera.hpp"
#include "scene/frustum.hpp"
#include "scene/scene.hpp"

#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

using Stage = GpuProfiler::Stage;

namespace {

    glm::vec3 sunDirection(const Settings& s) {
        float ce = std::cos(s.sunElevation);
        return glm::vec3(ce * std::cos(s.sunAzimuth), std::sin(s.sunElevation), ce * std::sin(s.sunAzimuth));
    }

    const MaterialAsset* materialFor(const AssetManager& assets, const Entity& e, const Primitive& p) {
        if(e.materialOverride >= 0 &&
           e.materialOverride < static_cast<int32_t>(assets.materials.size()))
            return assets.materials[static_cast<size_t>(e.materialOverride)].get();
        if(p.material >= 0 && p.material < static_cast<int32_t>(assets.materials.size()))
            return assets.materials[static_cast<size_t>(p.material)].get();
        return &assets.fallbackMaterial;
    }

} // namespace

void Renderer::init(VulkanContext& ctx, Swapchain& swapchain, ShadowMap& shadow,
                    DescriptorAllocator& descriptors, GpuProfiler& profiler) {
    ctx_ = &ctx;
    swapchain_ = &swapchain;
    shadow_ = &shadow;
    descriptors_ = &descriptors;
    profiler_ = &profiler;

    createLayouts();
    createFrameSlots();
    rebuildPipelines(false);
}

void Renderer::shutdown() {
    if(!ctx_) return;
    vkDeviceWaitIdle(ctx_->device);
    for(auto& f : frames_) {
        if(f.cb) vkFreeCommandBuffers(ctx_->device, f.pool, 1, &f.cb);
        if(f.pool) vkDestroyCommandPool(ctx_->device, f.pool, nullptr);
        if(f.fence) vkDestroyFence(ctx_->device, f.fence, nullptr);
        if(f.acquire) vkDestroySemaphore(ctx_->device, f.acquire, nullptr);
        if(f.present) vkDestroySemaphore(ctx_->device, f.present, nullptr);
        if(f.ubo) vmaDestroyBuffer(ctx_->allocator, f.ubo, f.uboAllocation);
    }
    for(VkPipeline p : {pipeOpaque_, pipeDoubleSided_, pipeBlend_, pipeShadow_, pipeSky_})
        if(p) vkDestroyPipeline(ctx_->device, p, nullptr);
    vkDestroyPipelineLayout(ctx_->device, pbrLayout_, nullptr);
    vkDestroyPipelineLayout(ctx_->device, shadowLayout_, nullptr);
    vkDestroyPipelineLayout(ctx_->device, skyLayout_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_->device, sceneLayout_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_->device, materialLayout_, nullptr);
    vkDestroyDescriptorSetLayout(ctx_->device, shadowMaterialLayout_, nullptr);
}

VkShaderModule Renderer::loadShader(const char* file) {
    std::string path = std::string(RENDERER_SHADER_DIR) + "/" + file;
    std::ifstream in(path, std::ios::binary);
    if(!in) throw std::runtime_error("missing shader: " + path +
                                     " (did the PbrRenderer_shaders target build?)");
    std::vector<char> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_->device, &ci, nullptr, &module));
    return module;
}

void Renderer::createLayouts() {
    // Set 0: scene data + shadow sampler (bound normally, once per pass).
    {
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 2;
        ci.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &ci, nullptr, &sceneLayout_));
    }
    // Set 1 for PBR: five material samplers, PUSH DESCRIPTOR (Vulkan 1.4 core).
    {
        const VkDescriptorSetLayoutBinding bindings[5] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        ci.bindingCount = 5;
        ci.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &ci, nullptr, &materialLayout_));
    }
    // Set 1 for the shadow pass: base color only (alpha cutouts).
    {
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &ci, nullptr, &shadowMaterialLayout_));
    }

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushBlock)};

    {
        const VkDescriptorSetLayout sets[] = {sceneLayout_, materialLayout_};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = sets;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &push;
        VK_CHECK(vkCreatePipelineLayout(ctx_->device, &ci, nullptr, &pbrLayout_));
    }
    {
        const VkDescriptorSetLayout sets[] = {sceneLayout_, shadowMaterialLayout_};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = sets;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &push;
        VK_CHECK(vkCreatePipelineLayout(ctx_->device, &ci, nullptr, &shadowLayout_));
    }
    {
        const VkDescriptorSetLayout sets[] = {sceneLayout_};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = sets;
        VK_CHECK(vkCreatePipelineLayout(ctx_->device, &ci, nullptr, &skyLayout_));
    }
}

void Renderer::createFrameSlots() {
    for(uint32_t i = 0; i < kFrames; ++i) {
        FrameSlot& f = frames_[i];

        VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolCi.queueFamilyIndex = ctx_->graphicsFamily;
        VK_CHECK(vkCreateCommandPool(ctx_->device, &poolCi, nullptr, &f.pool));

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx_->device, &ai, &f.cb));

        VkFenceCreateInfo fenceCi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first wait must pass immediately
        VK_CHECK(vkCreateFence(ctx_->device, &fenceCi, nullptr, &f.fence));

        VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(ctx_->device, &semCi, nullptr, &f.acquire));
        VK_CHECK(vkCreateSemaphore(ctx_->device, &semCi, nullptr, &f.present));

        // Persistently mapped scene UBO.
        VkBufferCreateInfo bufCi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufCi.size = sizeof(SceneUbo);
        bufCi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationInfo info{};
        VK_CHECK(vmaCreateBuffer(ctx_->allocator, &bufCi, &aci, &f.ubo, &f.uboAllocation, &info));
        f.uboMapped = info.pMappedData;

        f.sceneSet = descriptors_->allocate(sceneLayout_);
        VkDescriptorBufferInfo bi{f.ubo, 0, sizeof(SceneUbo)};
        VkDescriptorImageInfo ii{shadow_->sampler, shadow_->view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet writes[] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, f.sceneSet, 0, 0,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, &bi, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, f.sceneSet, 1, 0,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &ii, nullptr, nullptr},
        };
        vkUpdateDescriptorSets(ctx_->device, 2, writes, 0, nullptr);
    }
}

void Renderer::rebuildPipelines(bool wireframe) {
    vkDeviceWaitIdle(ctx_->device);
    for(VkPipeline p : {pipeOpaque_, pipeDoubleSided_, pipeBlend_, pipeShadow_, pipeSky_})
        if(p) vkDestroyPipeline(ctx_->device, p, nullptr);

    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    };
    const VkFormat colorFormat = swapchain_->format;
    const VkFormat depthFormat = shadow_->format;

    VkPipelineRenderingCreateInfo mainRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    mainRendering.colorAttachmentCount = 1;
    mainRendering.pColorAttachmentFormats = &colorFormat;
    mainRendering.depthAttachmentFormat = depthFormat;

    VkPipelineRenderingCreateInfo shadowRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    shadowRendering.colorAttachmentCount = 0;
    shadowRendering.depthAttachmentFormat = depthFormat;

    VkShaderModule vert = loadShader("pbr.vert.spv");
    VkShaderModule frag = loadShader("pbr.frag.spv");
    VkShaderModule shadowVert = loadShader("shadow.vert.spv");
    VkShaderModule shadowFrag = loadShader("shadow.frag.spv");
    VkShaderModule skyVert = loadShader("sky.vert.spv");
    VkShaderModule skyFrag = loadShader("sky.frag.spv");

    PipelineBuilder b;
    b.device = ctx_->device;
    b.vertexStride = sizeof(Vertex);
    b.attributes.assign(attributes, attributes + 3);
    b.samples = swapchain_->sampleCount;
    b.rendering = &mainRendering;
    b.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // GLTF convention; proj Y is flipped
    b.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;

    b.layout = pbrLayout_;
    b.stages = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};

    b.cullMode = VK_CULL_MODE_BACK_BIT;
    pipeOpaque_ = b.build();
    b.cullMode = VK_CULL_MODE_NONE_BIT;
    pipeDoubleSided_ = b.build();
    b.blend = true;
    b.depthWrite = false;
    pipeBlend_ = b.build();

    // Sky: fullscreen triangle, depth test LEQUAL at the far plane, no writes.
    b.blend = false;
    b.cullMode = VK_CULL_MODE_NONE_BIT;
    b.depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    b.depthWrite = false;
    b.layout = skyLayout_;
    b.stages = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 VK_SHADER_STAGE_VERTEX_BIT, skyVert, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 VK_SHADER_STAGE_FRAGMENT_BIT, skyFrag, "main", nullptr}};
    pipeSky_ = b.build();

    // Shadow: depth-only, cull none (thin/double-sided geometry), dynamic bias.
    b.rendering = &shadowRendering;
    b.samples = VK_SAMPLE_COUNT_1_BIT;
    b.layout = shadowLayout_;
    b.stages = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 VK_SHADER_STAGE_VERTEX_BIT, shadowVert, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                 VK_SHADER_STAGE_FRAGMENT_BIT, shadowFrag, "main", nullptr}};
    b.depthOp = VK_COMPARE_OP_LESS;
    b.depthWrite = true;
    b.depthBias = true;
    b.polygonMode = VK_POLYGON_MODE_FILL;
    pipeShadow_ = b.build();

    for(VkShaderModule m : {vert, frag, shadowVert, shadowFrag, skyVert, skyFrag})
        vkDestroyShaderModule(ctx_->device, m, nullptr);

    lastWireframe_ = wireframe;
    ctx_->name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeOpaque_), "pbr opaque");
    ctx_->name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeShadow_), "sun shadow");
}

void Renderer::recreateSwapchain() {
    vkDeviceWaitIdle(ctx_->device);
    swapchain_->recreate();
    for(auto& f : frames_) {
        vkDestroySemaphore(ctx_->device, f.acquire, nullptr);
        vkDestroySemaphore(ctx_->device, f.present, nullptr);
        VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(ctx_->device, &semCi, nullptr, &f.acquire));
        VK_CHECK(vkCreateSemaphore(ctx_->device, &semCi, nullptr, &f.present));
    }
    rebuildPipelines(lastWireframe_);
    rebindShadowSampler();
    needsRecreate_ = false;
}

void Renderer::rebindShadowSampler() {
    for(auto& f : frames_) {
        VkDescriptorImageInfo ii{shadow_->sampler, shadow_->view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = f.sceneSet;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &ii;
        vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);
    }
}

bool Renderer::beginFrame() {
    if(needsRecreate_) {
        recreateSwapchain();
        return false; // skip this frame
    }

    FrameSlot& f = frames_[frameIndex_];
    VK_CHECK(vkWaitForFences(ctx_->device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    profiler_->beginFrame(frameIndex_); // results are guaranteed valid once the fence is signaled

    VkResult result = vkAcquireNextImageKHR(ctx_->device, swapchain_->swapchainKHR(), UINT64_MAX,
                                            f.acquire, VK_NULL_HANDLE, &imageIndex_);
    if(result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
    if(result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        VK_CHECK(result);
    if(result == VK_SUBOPTIMAL_KHR)
        needsRecreate_ = true; // draw this frame, rebuild afterwards

    VK_CHECK(vkResetCommandPool(ctx_->device, f.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cb, &begin));
    return true;
}

glm::mat4 Renderer::computeLightMatrix(const Scene& scene, const Settings& s, float& spanOut) {
    AABB b = scene.bounds.valid() ? scene.bounds : AABB{glm::vec3(-5.0f), glm::vec3(5.0f)};
    glm::vec3 sun = sunDirection(s);
    glm::vec3 c = b.center();
    float r = std::max(b.radius(), 0.5f);

    glm::vec3 eye = c - sun * (r * 4.0f);
    glm::vec3 up = std::abs(sun.y) > 0.98f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    glm::mat4 lightView = glm::lookAt(eye, c, up);

    AABB ls;
    for(int i = 0; i < 8; ++i) {
        glm::vec3 corner(i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z);
        ls.expand(glm::vec3(lightView * glm::vec4(corner, 1.0f)));
    }
    float pad = r * 0.05f + 0.1f;
    spanOut = ls.max.x - ls.min.x;
    float nearZ = std::max(0.01f, -ls.max.z - pad);
    float farZ = -ls.min.z + pad;
    glm::mat4 lightProj = glm::ortho(ls.min.x - pad, ls.max.x + pad, ls.min.y - pad, ls.max.y + pad,
                                     nearZ, farZ);
    return lightProj * lightView;
}

void Renderer::updateSceneUbo(Scene& scene, CameraController& camera, const Settings& s,
                              const glm::mat4& lightVP, float shadowTexelWorldSize) {
    glm::mat4 view = camera.view();
    glm::mat4 proj = camera.proj();

    uboData_ = {};
    uboData_.view = view;
    uboData_.proj = proj;
    uboData_.lightViewProj = lightVP;
    uboData_.invViewProj = glm::inverse(proj * view);
    uboData_.camPos = glm::vec4(camera.position(), 1.0f);
    uboData_.sunDir = glm::vec4(sunDirection(s), 0.0f);
    uboData_.sunColorIntensity = glm::vec4(s.sunColor, s.sunIntensity);
    uboData_.ambientColor = glm::vec4(1.0f, 1.0f, 1.0f, s.ambientIntensity);
    uboData_.screenParams = glm::vec4(static_cast<float>(swapchain_->extent.width),
                                      static_cast<float>(swapchain_->extent.height),
                                      s.exposure, swapchain_->isSrgb() ? 0.0f : 1.0f);
    uboData_.skyZenith = glm::vec4(0.16f, 0.28f, 0.58f, 0.0f);
    uboData_.skyHorizon = glm::vec4(0.62f, 0.74f, 0.90f, 0.0f);
    uboData_.miscParams = glm::vec4(
        static_cast<float>(std::min<size_t>(scene.lights.size(), 8)),
        s.shadows ? 1.0f : 0.0f,
        shadowTexelWorldSize * s.shadowNormalOffset,
        static_cast<float>(shadow_->resolution));
    for(size_t i = 0; i < std::min<size_t>(scene.lights.size(), 8); ++i) {
        const auto& l = scene.lights[i];
        uboData_.pointPosRadius[i] = glm::vec4(l.pos, l.radius);
        uboData_.pointColorIntensity[i] = glm::vec4(l.color, l.intensity);
    }
    std::memcpy(frames_[frameIndex_].uboMapped, &uboData_, sizeof(SceneUbo));
}

void Renderer::pushMaterial(VkCommandBuffer cb, VkPipelineLayout layout,
                            const MaterialAsset& material, AssetManager& assets, bool shadowVariant) {
    const int32_t ids[5] = {material.texBaseColor, material.texMetallicRoughness,
                            material.texNormal, material.texOcclusion, material.texEmissive};
    const uint32_t count = shadowVariant ? 1 : 5;

    VkDescriptorImageInfo infos[5];
    VkWriteDescriptorSet writes[5];
    for(uint32_t i = 0; i < count; ++i) {
        TextureAsset* t = nullptr;
        if(ids[i] >= 0 && ids[i] < static_cast<int32_t>(assets.textures.size()))
            t = assets.textures[static_cast<size_t>(ids[i])].get();
        if(!t || t->state != static_cast<int>(AssetState::Ready) || !t->view)
            t = assets.defaultTexture(i); // 1x1 stand-ins

        infos[i] = {t->sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = VK_NULL_HANDLE; // ignored for push descriptors
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    // Vulkan 1.4 core entry point (promoted from VK_KHR_push_descriptor).
    vkCmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, count, writes);
}

void Renderer::render(Scene& scene, CameraController& camera, AssetManager& assets,
                      const Settings& settings, FrameStats& stats, ImDrawData* drawData) {
    // ---- structural changes requested by the UI --------------------------------
    if(settings.shadowResolution != shadow_->resolution) {
        vkDeviceWaitIdle(ctx_->device);
        shadow_->recreate(settings.shadowResolution);
        rebindShadowSampler();
        settings.shadowResolution = shadow_->resolution;
    }
    if(settings.wireframe != lastWireframe_)
        rebuildPipelines(settings.wireframe);

    if(!beginFrame()) return;
    FrameSlot& f = frames_[frameIndex_];
    VkCommandBuffer cb = f.cb;

    float lightSpan = 1.0f;
    glm::mat4 lightVP = computeLightMatrix(scene, settings, lightSpan);
    float shadowTexelWorldSize = lightSpan / static_cast<float>(shadow_->resolution);
    updateSceneUbo(scene, camera, settings, lightVP, shadowTexelWorldSize);

    const glm::mat4 viewProj = camera.proj() * camera.view();

    // ---- culling ------------------------------------------------------------------
    Frustum cameraFrustum = Frustum::fromMatrix(viewProj);
    Frustum lightFrustum = Frustum::fromMatrix(lightVP);
    visible_.clear();
    casters_.clear();
    for(const auto& e : scene.entities) {
        if(!e.visible || e.model >= assets.models.size()) continue;
        if(assets.models[e.model]->state != AssetState::Ready) continue;
        bool inView = !settings.frustumCulling || cameraFrustum.testAABB(e.worldAABB);
        if(inView) visible_.push_back(&e);
        if(settings.shadows && (!settings.frustumCulling || lightFrustum.testAABB(e.worldAABB)))
            casters_.push_back(&e);
    }
    stats.entitiesTotal = static_cast<uint32_t>(scene.entities.size());
    stats.entitiesVisible = static_cast<uint32_t>(visible_.size());
    stats.shadowCasters = static_cast<uint32_t>(casters_.size());
    stats.drawCalls = 0;
    stats.triangles = 0;

    // ============================ SHADOW PASS =====================================
    if(settings.shadows) {
        profiler_->mark(cb, Stage::Shadow, false);

        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = shadow_->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {shadow_->resolution, shadow_->resolution}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cb, &ri);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(shadow_->resolution),
                     static_cast<float>(shadow_->resolution), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {shadow_->resolution, shadow_->resolution}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdSetDepthBias(cb, settings.shadowBiasConstant, 0.0f, settings.shadowBiasSlope);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeShadow_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout_, 0, 1, &f.sceneSet, 0, nullptr);

        for(const Entity* e : casters_) {
            const ModelAsset& m = *assets.models[e->model];
            VkBuffer vb = m.vertexBuffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(cb, m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            for(const Primitive& prim : m.primitives) {
                const MaterialAsset* mat = materialFor(assets, *e, prim);
                PushBlock push;
                push.model = e->modelMatrix;
                push.baseColorFactor = mat->baseColorFactor;
                push.emissiveFactor = mat->emissiveFactor;
                push.params0 = {mat->metallic, mat->roughness, mat->alphaCutoff,
                                uintToFloatBits(mat->flags)};
                push.params1 = {mat->normalScale, mat->occlusionStrength, 0.0f, 0.0f};
                vkCmdPushConstants(cb, shadowLayout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(PushBlock), &push);
                pushMaterial(cb, shadowLayout_, *mat, assets, true);
                vkCmdDrawIndexed(cb, prim.indexCount, 1, prim.firstIndex, 0, 0);
                ++stats.drawCalls;
                stats.triangles += prim.indexCount / 3;
            }
        }
        vkCmdEndRendering(cb);

        // Make the shadow map sampleable by the main pass.
        depthBarrier(cb, shadow_->image,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        profiler_->mark(cb, Stage::Shadow, true);
    }

    // ============================ MAIN PASS =======================================
    // One dynamic render pass: opaque -> sky -> transparent -> ImGui.
    // Depth is never stored afterwards -> VK_ATTACHMENT_STORE_OP_NONE (Vulkan 1.4).
    profiler_->mark(cb, Stage::Geometry, false);

    const bool msaa = swapchain_->sampleCount != VK_SAMPLE_COUNT_1_BIT;
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = msaa ? swapchain_->msaaColorView : swapchain_->view(imageIndex_);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.62f, 0.74f, 0.90f, 1.0f}};
    if(msaa) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = swapchain_->view(imageIndex_);
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = swapchain_->depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE; // promoted to core in Vulkan 1.4
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingAttachmentInfo* attachments[] = {&color};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, swapchain_->extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = attachments;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cb, &ri);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(swapchain_->extent.width),
                 static_cast<float>(swapchain_->extent.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, swapchain_->extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pbrLayout_, 0, 1, &f.sceneSet, 0, nullptr);

    // ---- opaque ---------------------------------------------------------------
    blendSorted_.clear();
    for(const Entity* e : visible_) {
        const ModelAsset& m = *assets.models[e->model];
        VkBuffer vb = m.vertexBuffer;
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cb, m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        bool hasBlendPrims = false;
        for(const Primitive& prim : m.primitives) {
            const MaterialAsset* mat = materialFor(assets, *e, prim);
            if(mat->flags & matflags::AlphaBlend) {
                hasBlendPrims = true;
                continue;
            }
            VkPipeline pipe = (mat->flags & matflags::DoubleSided) ? pipeDoubleSided_ : pipeOpaque_;
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

            PushBlock push;
            push.model = e->modelMatrix;
            push.baseColorFactor = mat->baseColorFactor;
            push.emissiveFactor = mat->emissiveFactor;
            push.params0 = {mat->metallic, mat->roughness, mat->alphaCutoff,
                            uintToFloatBits(mat->flags)};
            push.params1 = {mat->normalScale, mat->occlusionStrength, 0.0f, 0.0f};
            vkCmdPushConstants(cb, pbrLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushBlock), &push);
            pushMaterial(cb, pbrLayout_, *mat, assets, false);
            vkCmdDrawIndexed(cb, prim.indexCount, 1, prim.firstIndex, 0, 0);
            ++stats.drawCalls;
            stats.triangles += prim.indexCount / 3;
        }
        if(hasBlendPrims) {
            float dist = glm::length(camera.position() - e->worldAABB.center());
            blendSorted_.emplace_back(dist, e);
        }
    }

    // ---- sky (after opaque: LEQUAL at the far plane, no writes) -----------------
    if(settings.sky) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeSky_);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }

    // ---- transparent (back to front) --------------------------------------------
    if(!blendSorted_.empty()) {
        std::sort(blendSorted_.begin(), blendSorted_.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeBlend_);
        for(auto& [dist, e] : blendSorted_) {
            (void)dist;
            const ModelAsset& m = *assets.models[e->model];
            VkBuffer vb = m.vertexBuffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(cb, m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            for(const Primitive& prim : m.primitives) {
                const MaterialAsset* mat = materialFor(assets, *e, prim);
                if(!(mat->flags & matflags::AlphaBlend)) continue;

                PushBlock push;
                push.model = e->modelMatrix;
                push.baseColorFactor = mat->baseColorFactor;
                push.emissiveFactor = mat->emissiveFactor;
                push.params0 = {mat->metallic, mat->roughness, mat->alphaCutoff,
                                uintToFloatBits(mat->flags)};
                push.params1 = {mat->normalScale, mat->occlusionStrength, 0.0f, 0.0f};
                vkCmdPushConstants(cb, pbrLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(PushBlock), &push);
                pushMaterial(cb, pbrLayout_, *mat, assets, false);
                vkCmdDrawIndexed(cb, prim.indexCount, 1, prim.firstIndex, 0, 0);
                ++stats.drawCalls;
                stats.triangles += prim.indexCount / 3;
            }
        }
    }
    profiler_->mark(cb, Stage::Geometry, true);

    // ---- UI -----------------------------------------------------------------------
    profiler_->mark(cb, Stage::Ui, false);
    if(drawData && drawData->CmdListsCount > 0)
        ImGui_ImplVulkan_RenderDrawData(drawData, cb);
    profiler_->mark(cb, Stage::Ui, true);

    vkCmdEndRendering(cb);

    // ---- present barrier ------------------------------------------------------------
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.image = swapchain_->image(imageIndex_);
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
    }
    VK_CHECK(vkEndCommandBuffer(cb));

    // ---- submit (synchronization2) + present -----------------------------------------
    {
        VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbInfo.commandBuffer = cb;
        VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitInfo.semaphore = f.acquire;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalInfo.semaphore = f.present;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &waitInfo;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferSubmitInfos = &cbInfo;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signalInfo;

        std::scoped_lock lock(ctx_->submitMutex);
        VK_CHECK(vkResetFences(ctx_->device, 1, &f.fence));
        VK_CHECK(vkQueueSubmit2(ctx_->graphicsQueue, 1, &si, f.fence));
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &f.present;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_->swapchainKHR();
    pi.pImageIndices = &imageIndex_;
    VkResult present = vkQueuePresentKHR(ctx_->graphicsQueue, &pi);
    if(present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR)
        needsRecreate_ = true;
    else if(present != VK_SUCCESS)
        LOG_WARN("present returned {}", static_cast<int>(present));

    // GPU timings read at the next beginFrame of this slot; publish the latest values.
    stats.gpuShadowMs.push(profiler_->stageMs(static_cast<int>(Stage::Shadow)));
    stats.gpuOpaqueMs.push(profiler_->stageMs(static_cast<int>(Stage::Geometry)));
    stats.gpuSkyMs.push(0.0f); // drawn inside the geometry pass
    stats.gpuUiMs.push(profiler_->stageMs(static_cast<int>(Stage::Ui)));
    stats.gpuTotalMs.push(profiler_->totalMs());

    frameIndex_ = (frameIndex_ + 1) % kFrames;
}