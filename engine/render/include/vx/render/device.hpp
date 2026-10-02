#pragma once

#include <vx/render/device_selection.hpp>

#include <volk.h>

namespace vx::render {

// Reads what selection needs from a physical device. Feature structures newer
// than the device's Vulkan version are left out of the query, as the
// specification requires, so their features read as missing.
[[nodiscard]] DeviceInfo describe_device(VkPhysicalDevice physical_device);

} // namespace vx::render
