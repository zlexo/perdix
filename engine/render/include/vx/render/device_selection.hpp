#pragma once

#include <vx/core/result.hpp>
#include <vx/render/feature_floor.hpp>

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vx::render {

// Features the engine uses when a device offers them, and works without.
struct OptionalFeatures {
    bool sampler_anisotropy = false;  // RFC 0004's block texture filtering
    bool fill_mode_non_solid = false; // RFC 0003's wireframe toggle
};

// What selection knows about one physical device. Device creation fills it from
// Vulkan; as a plain struct, selection and the startup report are tested
// without a GPU.
struct DeviceInfo {
    std::string name;        // VkPhysicalDeviceProperties::deviceName
    std::string driver_name; // VkPhysicalDeviceDriverProperties::driverName
    std::string driver_info; // VkPhysicalDeviceDriverProperties::driverInfo, often a version
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    DeviceFeatures features;
    OptionalFeatures optional;
    // Lists VK_KHR_portability_subset: a portability driver, such as MoltenVK.
    bool portability_subset = false;
    // The first queue family with graphics and compute. The engine uses one queue.
    std::optional<std::uint32_t> queue_family;
};

// Picks the device to run on, among those that pass the feature floor and have
// a queue family with graphics and compute. A device without
// VK_KHR_portability_subset beats one that lists it, then a discrete GPU beats
// an integrated one; ties keep the loader's order. Fails naming every device
// and what it lacks.
[[nodiscard]] Result<std::size_t> choose_device(std::span<const DeviceInfo> devices);

} // namespace vx::render
