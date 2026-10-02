#pragma once
// Vulkan 1.4 device context: instance/device creation with the 1.2/1.3/1.4
// feature chains, queue family survey, VMA allocator and debug naming.

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct GLFWwindow;

struct QueueFamilyInfo {
    VkQueueFlags flags;
    uint32_t count;
    bool present;
};

class VulkanContext {
public:
    void init(GLFWwindow* window, bool validation);
    void shutdown();

    void name(VkObjectType type, uint64_t handle, const char* label);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0;

    VmaAllocator allocator = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    std::vector<QueueFamilyInfo> queueFamilies;
    VkSampleCountFlags supportedSamples = VK_SAMPLE_COUNT_1_BIT;
    bool anisotropy = false;
    bool supportsMemoryBudget = false;
    bool supportsDynamicRenderingLocalRead = false;
    bool validationEnabled = false;

    std::mutex submitMutex; // graphics queue shared with the uploader thread

private:
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT_ = nullptr;
};