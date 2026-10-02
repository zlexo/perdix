#pragma once

#include <vx/render/feature_floor.hpp>

#include <vulkan/vulkan_core.h>

#include <cstdint>

namespace vx::test {

// A device that offers exactly the feature floor, at the given Vulkan version.
inline render::DeviceFeatures full_floor(std::uint32_t api_version = VK_API_VERSION_1_3) {
    return {
        .api_version = api_version,
        .dynamic_rendering = true,
        .synchronization2 = true,
        .timeline_semaphore = true,
        .buffer_device_address = true,
        .descriptor_indexing = true,
        .runtime_descriptor_array = true,
        .descriptor_binding_partially_bound = true,
        .multi_draw_indirect = true,
        .draw_indirect_first_instance = true,
    };
}

} // namespace vx::test
