#include "render/vulkancontext.hpp"

#include "core/log.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <format>
#include <set>

namespace {

    VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
        if(severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            LOG_ERROR("validation: {}", data->pMessage);
        else if(severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            LOG_WARN("validation: {}", data->pMessage);
        return VK_FALSE;
    }

    bool hasLayer(const char* name) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        return std::any_of(layers.begin(), layers.end(),
                           [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
    }

    bool hasDeviceExtension(VkPhysicalDevice dev, const char* name) {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
        return std::any_of(exts.begin(), exts.end(),
                           [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    }

} // namespace

void VulkanContext::init(GLFWwindow* window, bool validation) {
    // ---- instance ---------------------------------------------------------
    uint32_t instanceVersion = 0;
    vkEnumerateInstanceVersion(&instanceVersion);
    LOG_INFO("loader/driver reports Vulkan {}.{}.{}",
             VK_API_VERSION_MAJOR(instanceVersion), VK_API_VERSION_MINOR(instanceVersion),
             VK_API_VERSION_PATCH(instanceVersion));
    if(instanceVersion < VK_API_VERSION_1_4)
        throw std::runtime_error(std::format(
            "this engine requires Vulkan 1.4; the installed driver reports {}.{}.{}. "
            "Update your GPU driver (NVIDIA 560+, AMD 24.x+, or a current Intel driver).",
            VK_API_VERSION_MAJOR(instanceVersion), VK_API_VERSION_MINOR(instanceVersion),
            VK_API_VERSION_PATCH(instanceVersion)));

    uint32_t glfwCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwCount);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> layers;
    if(validation && hasLayer("VK_LAYER_KHRONOS_VALIDATION")) {
        layers.push_back("VK_LAYER_KHRONOS_VALIDATION");
        validationEnabled = true;
        LOG_INFO("validation layers enabled");
    }
    else if(validation) {
        LOG_WARN("validation requested but VK_LAYER_KHRONOS_VALIDATION is not installed");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PbrRenderer";
    app.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
    app.pEngineName = "PbrRenderer";
    app.engineVersion = VK_MAKE_VERSION(2, 0, 0);
    app.apiVersion = VK_API_VERSION_1_4;

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.pNext = validationEnabled ? &dbg : nullptr;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));

    if(validationEnabled) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if(create) create(instance, &dbg, nullptr, &messenger_);
    }
    vkSetDebugUtilsObjectNameEXT_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT"));

    VK_CHECK(glfwCreateWindowSurface(instance, window, nullptr, &surface));

    // ---- physical device -----------------------------------------------------
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, deviceCount, devices.data());

    int bestScore = -1;
    for(VkPhysicalDevice dev : devices) {
        vkGetPhysicalDeviceProperties(dev, &props);
        if(props.apiVersion < VK_API_VERSION_1_4) continue;

        queueFamilies.clear();
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &familyCount, families.data());

        int graphicsWithPresent = -1;
        for(uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
            queueFamilies.push_back({families[i].queueFlags, families[i].queueCount, present == VK_TRUE});
            if((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && graphicsWithPresent < 0)
                graphicsWithPresent = static_cast<int>(i);
        }
        if(graphicsWithPresent < 0) continue;

        int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 :
                     props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500 : 100);
        score += static_cast<int>(props.limits.maxImageDimension2D / 1024);
        if(score > bestScore) {
            bestScore = score;
            physicalDevice = dev;
            graphicsFamily = static_cast<uint32_t>(graphicsWithPresent);
        }
    }
    if(physicalDevice == VK_NULL_HANDLE)
        throw std::runtime_error("no Vulkan 1.4 capable GPU with a graphics+present queue family found");
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    LOG_INFO("device: '{}' ({}, api {}.{}.{}, driver {})", props.deviceName,
             props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" :
             props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : "other",
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);

    // ---- verify the features we rely on -----------------------------------------
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    f2.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &f14;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &f2);

    if(!f13.dynamicRendering || !f13.synchronization2)
        throw std::runtime_error("dynamic rendering / synchronization2 unavailable (required)");
    if(!f12.hostQueryReset)
        throw std::runtime_error("hostQueryReset unavailable (required for GPU profiling)");
    if(!f2.features.samplerAnisotropy || !f2.features.fillModeNonSolid)
        throw std::runtime_error("samplerAnisotropy/fillModeNonSolid unavailable");
    if(!f14.pushDescriptor)
        throw std::runtime_error("pushDescriptor (optional Vulkan 1.4 feature) unavailable on this driver");
    if(props.limits.maxPushDescriptors < 5)
        throw std::runtime_error(std::format("maxPushDescriptors = {} < 5 (material set needs 5)",
                                             props.limits.maxPushDescriptors));
    supportsDynamicRenderingLocalRead = f14.dynamicRenderingLocalRead != VK_FALSE;
    supportedSamples = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;

    LOG_INFO("push descriptors OK (maxPushDescriptors={}), timestamps guaranteed (period {} ns)",
             props.limits.maxPushDescriptors, props.timestampPeriod);

    // ---- device --------------------------------------------------------------------
    std::vector<const char*> deviceExts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if(hasDeviceExtension(physicalDevice, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        deviceExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        supportsMemoryBudget = true;
        LOG_INFO("VK_EXT_memory_budget available - heap budgets will be live");
    }

    // Enable the verified subset.
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.fillModeNonSolid = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.dynamicRendering = VK_TRUE;
    f14.pushDescriptor = VK_TRUE;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCi.queueFamilyIndex = graphicsFamily;
    queueCi.queueCount = 1;
    queueCi.pQueuePriorities = &priority;

    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &queueCi;
    di.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
    di.ppEnabledExtensionNames = deviceExts.data();
    VK_CHECK(vkCreateDevice(physicalDevice, &di, nullptr, &device));

    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);

    // GPU architecture survey (also shown in the UI).
    for(size_t i = 0; i < queueFamilies.size(); ++i) {
        const auto& qf = queueFamilies[i];
        LOG_INFO("queue family {}: {} queues, flags=0x{:x}, present={}", i, qf.count,
                 static_cast<uint32_t>(qf.flags), qf.present ? "yes" : "no");
    }
    for(uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        const auto& heap = memProps.memoryHeaps[i];
        LOG_INFO("memory heap {}: {:.2f} GB, {}", i, heap.size / (1024.0 * 1024.0 * 1024.0),
                 heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device local" : "host");
    }

    // ---- VMA ----------------------------------------------------------------------
    VmaAllocatorCreateInfo vaci{};
    vaci.physicalDevice = physicalDevice;
    vaci.device = device;
    vaci.instance = instance;
    vaci.vulkanApiVersion = VK_API_VERSION_1_4;
    VK_CHECK(vmaCreateAllocator(&vaci, &allocator));

    name(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<uint64_t>(device), "PbrRenderer device");
}

void VulkanContext::name(VkObjectType type, uint64_t handle, const char* label) {
    if(!vkSetDebugUtilsObjectNameEXT_) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = label;
    vkSetDebugUtilsObjectNameEXT_(device, &info);
}

void VulkanContext::shutdown() {
    if(allocator) { vmaDestroyAllocator(allocator); allocator = VK_NULL_HANDLE; }
    if(device) { vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE; }
    if(surface) { vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
    if(messenger_) {
        if(auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")))
            destroy(instance, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if(instance) { vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
}