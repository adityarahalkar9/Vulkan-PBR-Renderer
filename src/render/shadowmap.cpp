#include "render/shadowmap.hpp"
#include "render/vulkanutils.hpp"

void ShadowMap::init(VulkanContext& ctx, uint32_t res) {
    ctx_ = &ctx;
    resolution = res;
    create();
}

void ShadowMap::create() {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {resolution, resolution, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &ci, &aci, &image, &allocation_, nullptr));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &vci, nullptr, &view));

    // Comparison sampler: enables hardware PCF via sampler2DShadow.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS;
    VK_CHECK(vkCreateSampler(ctx_->device, &sci, nullptr, &sampler));

    ctx_->name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image), "sun shadow map");
}

void ShadowMap::recreate(uint32_t res) {
    shutdown();
    resolution = res;
    create();
}

void ShadowMap::shutdown() {
    if(!ctx_) return;
    if(sampler) { vkDestroySampler(ctx_->device, sampler, nullptr); sampler = VK_NULL_HANDLE; }
    if(view) { vkDestroyImageView(ctx_->device, view, nullptr); view = VK_NULL_HANDLE; }
    if(image) { vmaDestroyImage(ctx_->allocator, image, allocation_); image = VK_NULL_HANDLE; }
}