#include <vx/render/device.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace vx::render {
namespace {

// Declared in vulkan_beta.h, which needs VK_ENABLE_BETA_EXTENSIONS; the name is
// all the engine needs.
constexpr const char* portability_subset_extension = "VK_KHR_portability_subset";

bool lists_extension(VkPhysicalDevice physical_device, std::string_view name) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
        return name == extension.extensionName;
    });
}

std::optional<std::uint32_t> graphics_and_compute_family(VkPhysicalDevice physical_device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families.data());
    const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

DeviceInfo describe_device(VkPhysicalDevice physical_device) {
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
    info.features.multi_draw_indirect = features.features.multiDrawIndirect == VK_TRUE;
    info.features.draw_indirect_first_instance =
        features.features.drawIndirectFirstInstance == VK_TRUE;
    info.optional.sampler_anisotropy = features.features.samplerAnisotropy == VK_TRUE;
    info.optional.fill_mode_non_solid = features.features.fillModeNonSolid == VK_TRUE;

    info.portability_subset = lists_extension(physical_device, portability_subset_extension);
    info.queue_family = graphics_and_compute_family(physical_device);
    return info;
}

} // namespace vx::render
