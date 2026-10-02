#include <vx/render/device_selection.hpp>

#include <string_view>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

// Lower ranks better.
int type_rank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 3;
    default:
        return 4;
    }
}

// Lower ranks better: a conformant driver first, then the device type.
std::pair<bool, int> rank(const DeviceInfo& device) {
    return {device.portability_subset, type_rank(device.type)};
}

} // namespace

Result<std::size_t> choose_device(std::span<const DeviceInfo> devices) {
    if (devices.empty()) {
        return Error{"no Vulkan device found"};
    }

    std::optional<std::size_t> best;
    std::string rejected;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const DeviceInfo& device = devices[i];
        std::vector<std::string_view> missing = check_feature_floor(device.features).missing;
        if (!device.queue_family.has_value()) {
            missing.emplace_back("a queue with graphics and compute");
        }
        if (!missing.empty()) {
            rejected += "\n  " + device.name + " (" + device.driver_name + ") lacks ";
            std::string_view separator;
            for (const std::string_view name : missing) {
                rejected += separator;
                rejected += name;
                separator = ", ";
            }
            continue;
        }
        if (!best.has_value() || rank(device) < rank(devices[*best])) {
            best = i;
        }
    }

    if (!best.has_value()) {
        return Error{"no Vulkan device meets the feature floor:" + rejected};
    }
    return *best;
}

} // namespace vx::render
