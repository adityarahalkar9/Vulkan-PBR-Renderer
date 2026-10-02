#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

// Allocates the per-frame scene descriptor sets (set 0). Material sets are
// push descriptors (Vulkan 1.4 core) and never come from a pool.
class DescriptorAllocator {
public:
    void init(VkDevice device, uint32_t maxSets);
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void shutdown();

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};