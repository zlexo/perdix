#include <vx/render/device_report.hpp>

#include <cstdint>

namespace vx::render {
namespace {

const char* type_name(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "CPU";
    default:
        return "other device";
    }
}

std::string version_string(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + '.' +
           std::to_string(VK_API_VERSION_MINOR(version)) + '.' +
           std::to_string(VK_API_VERSION_PATCH(version));
}

} // namespace

std::string format_device_report(const DeviceInfo& device, bool portability_fallback) {
    std::string report = "Device: " + device.name + " (" + type_name(device.type) + ")\n";

    report += "Driver: " + device.driver_name;
    if (!device.driver_info.empty()) {
        report += ' ' + device.driver_info;
    }
    report += ", Vulkan " + version_string(device.features.api_version) + '\n';

    report += "Portability fallback: ";
    report += portability_fallback ? "taken, no device on a conformant driver passed the floor\n"
                                   : "not needed\n";

    if (!device.optional.sampler_anisotropy) {
        report += "Not offered: samplerAnisotropy, so filtering stays trilinear\n";
    }
    if (!device.optional.fill_mode_non_solid) {
        report += "Not offered: fillModeNonSolid, so there is no wireframe toggle\n";
    }
    return report;
}

} // namespace vx::render
