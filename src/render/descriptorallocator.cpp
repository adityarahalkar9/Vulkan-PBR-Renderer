#include "render/descriptorallocator.hpp"
#include "render/vulkanutils.hpp"

void DescriptorAllocator::init(VkDevice device, uint32_t maxSets) {
    device_ = device;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSets * 4}, // headroom for shadow rewrites
    };
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = maxSets;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &ci, nullptr, &pool_));
}

VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &set));
    return set;
}

void DescriptorAllocator::shutdown() {
    if(pool_) { vkDestroyDescriptorPool(device_, pool_, nullptr); pool_ = VK_NULL_HANDLE; }
}