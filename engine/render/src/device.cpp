#include <vx/render/device.hpp>

#include "debug_name.hpp"
#include "vk_result.hpp"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

// Declared in vulkan_beta.h, which needs VK_ENABLE_BETA_EXTENSIONS; the name is
// all the engine needs.
constexpr const char* portability_subset_extension = "VK_KHR_portability_subset";

VkBool32 vk_bool(bool value) {
    return value ? VK_TRUE : VK_FALSE;
}

bool lists_extension(VkPhysicalDevice physical_device, std::string_view name) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
        return name == extension.extensionName;
    });
}

struct QueueChoice {
    std::optional<std::uint32_t> family;
    bool presents = true;
};

// The first queue family with graphics and compute that can present to the
// surface. Without a surface, the first with graphics and compute; when none
// can present, that one, marked as not presenting.
QueueChoice choose_queue_family(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families.data());
    const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    std::optional<std::uint32_t> first;
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & wanted) != wanted) {
            continue;
        }
        if (surface == VK_NULL_HANDLE) {
            return {.family = i, .presents = true};
        }
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &supported);
        if (supported == VK_TRUE) {
            return {.family = i, .presents = true};
        }
        if (!first.has_value()) {
            first = i;
        }
    }
    return {.family = first, .presents = surface == VK_NULL_HANDLE};
}

} // namespace

DeviceInfo describe_device(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    const bool vulkan12 = properties.apiVersion >= VK_API_VERSION_1_2;
    const bool vulkan13 = properties.apiVersion >= VK_API_VERSION_1_3;

    DeviceInfo info;
    info.name = properties.deviceName;
    info.type = properties.deviceType;
    info.features.api_version = properties.apiVersion;

    if (vulkan12) {
        VkPhysicalDeviceDriverProperties driver = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
            .pNext = nullptr,
            .driverID = VK_DRIVER_ID_MAX_ENUM, // no zero value; the query overwrites it
            .driverName = {},
            .driverInfo = {},
            .conformanceVersion = {},
        };
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical_device, &properties2);
        info.driver_name = driver.driverName;
        info.driver_info = driver.driverInfo;
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = vulkan13 ? &features13 : nullptr;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = vulkan12 ? &features12 : nullptr;
    vkGetPhysicalDeviceFeatures2(physical_device, &features);

    info.features.dynamic_rendering = features13.dynamicRendering == VK_TRUE;
    info.features.synchronization2 = features13.synchronization2 == VK_TRUE;
    info.features.timeline_semaphore = features12.timelineSemaphore == VK_TRUE;
    info.features.buffer_device_address = features12.bufferDeviceAddress == VK_TRUE;
    info.features.descriptor_indexing = features12.descriptorIndexing == VK_TRUE;
    info.features.runtime_descriptor_array = features12.runtimeDescriptorArray == VK_TRUE;
    info.features.descriptor_binding_partially_bound =
        features12.descriptorBindingPartiallyBound == VK_TRUE;
    info.features.shader_sampled_image_array_dynamic_indexing =
        features.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
    info.features.multi_draw_indirect = features.features.multiDrawIndirect == VK_TRUE;
    info.features.draw_indirect_first_instance =
        features.features.drawIndirectFirstInstance == VK_TRUE;
    info.optional.sampler_anisotropy = features.features.samplerAnisotropy == VK_TRUE;
    info.optional.fill_mode_non_solid = features.features.fillModeNonSolid == VK_TRUE;

    info.portability_subset = lists_extension(physical_device, portability_subset_extension);
    const QueueChoice queue = choose_queue_family(physical_device, surface);
    info.queue_family = queue.family;
    // A window needs swapchains too, which a device that can present supports
    // in practice, but the extension is optional.
    info.presents =
        queue.presents && (surface == VK_NULL_HANDLE ||
                           lists_extension(physical_device, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
    return info;
}

Result<Device> Device::create(VkInstance instance, VkPhysicalDevice physical_device,
                              const DeviceInfo& info, std::span<const char* const> extensions) {
    ZoneScopedN("Create device");
    assert(info.queue_family.has_value() && "the device must have passed selection");
    const std::uint32_t family = *info.queue_family;

    // The feature floor, then the optional features the device offers.
    VkPhysicalDeviceVulkan13Features vulkan13{};
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13.synchronization2 = VK_TRUE;
    vulkan13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceVulkan12Features vulkan12{};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.pNext = &vulkan13;
    vulkan12.descriptorIndexing = VK_TRUE;
    vulkan12.descriptorBindingPartiallyBound = VK_TRUE;
    vulkan12.runtimeDescriptorArray = VK_TRUE;
    vulkan12.timelineSemaphore = VK_TRUE;
    vulkan12.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &vulkan12;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features.features.multiDrawIndirect = VK_TRUE;
    features.features.drawIndirectFirstInstance = VK_TRUE;
    features.features.samplerAnisotropy = vk_bool(info.optional.sampler_anisotropy);
    features.features.fillModeNonSolid = vk_bool(info.optional.fill_mode_non_solid);

    // VUID-VkDeviceCreateInfo-pProperties-04451: a device that lists
    // VK_KHR_portability_subset must enable it.
    std::vector<const char*> enabled(extensions.begin(), extensions.end());
    if (info.portability_subset) {
        enabled.push_back(portability_subset_extension);
    }

    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()),
        .ppEnabledExtensionNames = enabled.data(),
        .pEnabledFeatures = nullptr, // in features, through pNext
    };

    Device device;
    device.physical_device_ = physical_device;
    device.queue_family_ = family;
    if (const VkResult result =
            vkCreateDevice(physical_device, &create_info, nullptr, &device.device_);
        result != VK_SUCCESS) {
        return Error{"vkCreateDevice failed: " + vk_result_name(result)};
    }
    volkLoadDevice(device.device_);
    vkGetDeviceQueue(device.device_, family, 0, &device.queue_);
    set_debug_name(device.device_, device.device_, "device");
    set_debug_name(device.device_, device.queue_, "graphics queue");

    // VMA loads the rest of its functions through these two.
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocator_info.physicalDevice = physical_device;
    allocator_info.device = device.device_;
    allocator_info.pVulkanFunctions = &functions;
    allocator_info.instance = instance;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    if (const VkResult result = vmaCreateAllocator(&allocator_info, &device.allocator_);
        result != VK_SUCCESS) {
        return Error{"vmaCreateAllocator failed: " + vk_result_name(result)};
    }
    return device;
}

Device::Device(Device&& other) noexcept
    : physical_device_(std::exchange(other.physical_device_, VK_NULL_HANDLE)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      queue_(std::exchange(other.queue_, VK_NULL_HANDLE)), queue_family_(other.queue_family_),
      allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        destroy();
        physical_device_ = std::exchange(other.physical_device_, VK_NULL_HANDLE);
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        queue_ = std::exchange(other.queue_, VK_NULL_HANDLE);
        queue_family_ = other.queue_family_;
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
    }
    return *this;
}

Device::~Device() {
    destroy();
}

void Device::destroy() noexcept {
    if (device_ == VK_NULL_HANDLE) {
        return; // moved from, or never created: the allocator comes after the device
    }
    vkDeviceWaitIdle(device_);
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

} // namespace vx::render
