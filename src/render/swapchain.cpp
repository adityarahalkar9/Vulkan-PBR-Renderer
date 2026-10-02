#include "render/swapchain.hpp"

#include "core/log.hpp"
#include "render/vulkanutils.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>

namespace {
    bool formatIsSrgb(VkFormat f) {
        return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_R8G8B8_SRGB;
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
} // namespace

void Swapchain::init(VulkanContext& ctx, GLFWwindow* window) {
    ctx_ = &ctx;
    window_ = window;

    uint32_t modeCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &modeCount, nullptr));
    availableModes_.resize(modeCount);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &modeCount,
                                                       availableModes_.data()));
    // FIFO is guaranteed; keep it as the default selection.
    for(size_t i = 0; i < availableModes_.size(); ++i)
        if(availableModes_[i] == VK_PRESENT_MODE_FIFO_KHR) {
            presentModeIndex_ = static_cast<int>(i);
            break;
        }
    LOG_INFO("present modes available:");
    for(size_t i = 0; i < availableModes_.size(); ++i)
        LOG_INFO("  [{}] {}", i, presentModeName(availableModes_[i]));

    recreate();
}

void Swapchain::setSamples(uint32_t requested) {
    requestedSamples_ = requested;
    while(requestedSamples_ > 1 && !(ctx_->supportedSamples & requestedSamples_))
        requestedSamples_ >>= 1;
}

bool Swapchain::isSrgb() const { return formatIsSrgb(format); }

void Swapchain::recreate() {
    vkDeviceWaitIdle(ctx_->device);
    destroyAttachments();

    // ---- surface capabilities / format / extent --------------------------
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_->physicalDevice, ctx_->surface, &caps));

    uint32_t formatCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->physicalDevice, ctx_->surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->physicalDevice, ctx_->surface, &formatCount,
                                                  formats.data()));
    VkSurfaceFormatKHR chosen = formats.front();
    for(auto& f : formats) {
        if(f.format == VK_FORMAT_B8G8R8A8_SRGB &&
           f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
        if(formatIsSrgb(f.format)) chosen = f;
    }
    format = chosen.format;

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    extent.width = std::clamp<uint32_t>(static_cast<uint32_t>(fbw), caps.minImageExtent.width,
                                        caps.maxImageExtent.width);
    extent.height = std::clamp<uint32_t>(static_cast<uint32_t>(fbh), caps.minImageExtent.height,
                                         caps.maxImageExtent.height);
    if(extent.width == 0 || extent.height == 0)
        extent = {1, 1}; // minimized; the application skips frames

    uint32_t imageCount = caps.minImageCount + 1;
    if(caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkPresentModeKHR presentMode = availableModes_[static_cast<size_t>(
        std::clamp<int>(presentModeIndex_, 0, static_cast<int>(availableModes_.size()) - 1))];

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = ctx_->surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(ctx_->device, &ci, nullptr, &swapchain_));

    uint32_t count = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(ctx_->device, swapchain_, &count, nullptr));
    images_.resize(count);
    VK_CHECK(vkGetSwapchainImagesKHR(ctx_->device, swapchain_, &count, images_.data()));
    views_.resize(count);
    for(uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_->device, &vci, nullptr, &views_[i]));
    }

    // ---- depth + optional MSAA color target -------------------------------
    sampleCount = VK_SAMPLE_COUNT_1_BIT;
    for(uint32_t s = requestedSamples_; s > 1; s >>= 1)
        if(ctx_->supportedSamples & s) {
            sampleCount = static_cast<VkSampleCountFlagBits>(s);
            break;
        }
    createAttachments();

    LOG_INFO("swapchain: {}x{}, {} images, srgb={}, samples={}, present={}",
             extent.width, extent.height, count, isSrgb() ? "yes" : "no", sampleCount,
             presentModeName(presentMode));
}

void Swapchain::createAttachments() {
    VkImageCreateInfo dci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    dci.imageType = VK_IMAGE_TYPE_2D;
    dci.format = VK_FORMAT_D32_SFLOAT;
    dci.extent = {extent.width, extent.height, 1};
    dci.mipLevels = 1;
    dci.arrayLayers = 1;
    dci.samples = sampleCount;
    dci.tiling = VK_IMAGE_TILING_OPTIMAL;
    dci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &dci, &aci, &depthImage_, &depthAlloc_, nullptr));

    VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvci.image = depthImage_;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvci.format = dci.format;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &dvci, nullptr, &depthView));
    ctx_->name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(depthImage_), "swapchain depth");

    if(sampleCount != VK_SAMPLE_COUNT_1_BIT) {
        VkImageCreateInfo mci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        mci.imageType = VK_IMAGE_TYPE_2D;
        mci.format = format;
        mci.extent = {extent.width, extent.height, 1};
        mci.mipLevels = 1;
        mci.arrayLayers = 1;
        mci.samples = sampleCount;
        mci.tiling = VK_IMAGE_TILING_OPTIMAL;
        mci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        mci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vmaCreateImage(ctx_->allocator, &mci, &aci, &msaaImage_, &msaaAlloc_, nullptr));

        VkImageViewCreateInfo mvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        mvci.image = msaaImage_;
        mvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        mvci.format = format;
        mvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_->device, &mvci, nullptr, &msaaColorView));
        ctx_->name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(msaaImage_), "swapchain msaa");
    }
}

void Swapchain::destroyAttachments() {
    if(depthView) { vkDestroyImageView(ctx_->device, depthView, nullptr); depthView = VK_NULL_HANDLE; }
    if(depthImage_) { vmaDestroyImage(ctx_->allocator, depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; }
    if(msaaColorView) { vkDestroyImageView(ctx_->device, msaaColorView, nullptr); msaaColorView = VK_NULL_HANDLE; }
    if(msaaImage_) { vmaDestroyImage(ctx_->allocator, msaaImage_, msaaAlloc_); msaaImage_ = VK_NULL_HANDLE; }
    for(auto v : views_) vkDestroyImageView(ctx_->device, v, nullptr);
    views_.clear();
    images_.clear();
    if(swapchain_) { vkDestroySwapchainKHR(ctx_->device, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
}

void Swapchain::shutdown() {
    if(!ctx_) return;
    destroyAttachments();
    ctx_ = nullptr;
}