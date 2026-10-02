#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace vx::render {

// What a device reports, reduced to what RFC 0001's feature floor asks for.
// Device selection fills it from Vulkan's feature structs; as a plain struct,
// the floor can be tested without a GPU.
struct DeviceFeatures {
    std::uint32_t api_version = 0; // VkPhysicalDeviceProperties::apiVersion
    bool dynamic_rendering = false;
    bool synchronization2 = false;
    bool timeline_semaphore = false;
    bool buffer_device_address = false;
    bool descriptor_indexing = false;
    bool runtime_descriptor_array = false;
    bool descriptor_binding_partially_bound = false;
    bool shader_sampled_image_array_dynamic_indexing = false;
    bool multi_draw_indirect = false;
    bool draw_indirect_first_instance = false;
};

struct FeatureReport {
    bool supported = false;
    std::vector<std::string_view> missing; // Vulkan's names, in the floor's order
};

// Checks every item of the floor and names each one the device lacks.
[[nodiscard]] FeatureReport check_feature_floor(const DeviceFeatures& features);

} // namespace vx::render
