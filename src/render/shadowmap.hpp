#pragma once
#include "render/vulkancontext.hpp"

class ShadowMap {
public:
    void init(VulkanContext& ctx, uint32_t resolution);
    void recreate(uint32_t resolution); // caller must guarantee the device is idle
    void shutdown();

    VkImageView view = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t resolution = 2048;
    VkFormat format = VK_FORMAT_D32_SFLOAT;

private:
    void create();

    VulkanContext* ctx_ = nullptr;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
};