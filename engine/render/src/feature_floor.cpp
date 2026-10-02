#include <vx/render/feature_floor.hpp>

#include <vulkan/vulkan_core.h>

namespace vx::render {

FeatureReport check_feature_floor(const DeviceFeatures& features) {
    FeatureReport report;
    const auto require = [&report](bool present, std::string_view name) {
        if (!present) {
            report.missing.push_back(name);
        }
    };

    require(features.api_version >= VK_API_VERSION_1_3, "Vulkan 1.3");
    require(features.dynamic_rendering, "dynamicRendering");
    require(features.synchronization2, "synchronization2");
    require(features.timeline_semaphore, "timelineSemaphore");
    require(features.buffer_device_address, "bufferDeviceAddress");
    require(features.descriptor_indexing, "descriptorIndexing");
    require(features.runtime_descriptor_array, "runtimeDescriptorArray");
    require(features.descriptor_binding_partially_bound, "descriptorBindingPartiallyBound");
    require(features.multi_draw_indirect, "multiDrawIndirect");
    require(features.draw_indirect_first_instance, "drawIndirectFirstInstance");

    report.supported = report.missing.empty();
    return report;
}

} // namespace vx::render
