#pragma once
#include "render/vulkancontext.hpp"

#include <vulkan/vulkan.h>

#include <vector>

struct GLFWwindow;

class Swapchain {
public:
    void init(VulkanContext& ctx, GLFWwindow* window);
    void shutdown();
    void recreate();                       // full rebuild (waits for idle)

    void setPresentMode(int index) { presentModeIndex_ = index; }
    void setSamples(uint32_t requested);   // clamps to device support

    VkImageView view(uint32_t i) const { return views_[i]; }
    VkImage image(uint32_t i) const { return images_[i]; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    bool isSrgb() const;

    const std::vector<VkPresentModeKHR>& availablePresentModes() const { return availableModes_; }
    int presentModeIndex() const { return presentModeIndex_; }

    VkFormat format = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D extent{};
    VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
    VkImageView depthView = VK_NULL_HANDLE;     // multisampled when MSAA is on
    VkImageView msaaColorView = VK_NULL_HANDLE; // null when sampleCount == 1

private:
    void destroyAttachments();
    void createAttachments();

    VulkanContext* ctx_ = nullptr;
    GLFWwindow* window_ = nullptr;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkPresentModeKHR> availableModes_;
    int presentModeIndex_ = 0;
    uint32_t requestedSamples_ = 4;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthAlloc_ = VK_NULL_HANDLE;
    VkImage msaaImage_ = VK_NULL_HANDLE;
    VmaAllocation msaaAlloc_ = VK_NULL_HANDLE;
};