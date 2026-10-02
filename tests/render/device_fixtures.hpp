#pragma once

#include <vx/render/device_selection.hpp>
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

// The reference machine's GPU through each of its two drivers, as Vulkan SDK
// 1.4.363.0 reports them to the engine's Vulkan 1.3 instance. MoltenVK caps its
// version at the instance's: vulkaninfo, at 1.4, reads 1.4.357 instead.
inline render::DeviceInfo m3_on_kosmickrisp() {
    return {
        .name = "Apple M3",
        .driver_name = "KosmicKrisp",
        .driver_info = "vulkan-sdk-1.4.363 (git-269a955c89)",
        .type = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        .features = full_floor(VK_MAKE_API_VERSION(0, 1, 4, 363)),
        .optional = {.sampler_anisotropy = true, .fill_mode_non_solid = false},
        .portability_subset = false,
        .queue_family = 0U,
        .presents = true,
    };
}

inline render::DeviceInfo m3_on_moltenvk() {
    return {
        .name = "Apple M3",
        .driver_name = "MoltenVK",
        .driver_info = "1.4.2",
        .type = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        .features = full_floor(VK_MAKE_API_VERSION(0, 1, 3, 357)),
        .optional = {.sampler_anisotropy = true, .fill_mode_non_solid = true},
        .portability_subset = true,
        .queue_family = 0U,
        .presents = true,
    };
}

} // namespace vx::test
