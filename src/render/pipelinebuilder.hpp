#pragma once
#include <vulkan/vulkan.h>

#include <vector>

// Compact graphics pipeline factory for the dynamic-rendering era:
// no render passes, no framebuffers, formats supplied per pipeline.
struct PipelineBuilder {
    VkDevice device = VK_NULL_HANDLE;
    const VkPipelineRenderingCreateInfo* rendering = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkVertexInputAttributeDescription> attributes;
    uint32_t vertexStride = 0;

    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp depthOp = VK_COMPARE_OP_LESS;
    bool depthBias = false;

    bool blend = false;

    bool hasColorAttachment = true;

    VkPipeline build();
};