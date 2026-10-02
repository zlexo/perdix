#include <vx/render/context.hpp>

#include <vx/render/device.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

// A device chosen on an instance, before its logical device exists.
struct Selection {
    Instance instance;
    Surface surface;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    DeviceInfo info;
};

std::vector<VkPhysicalDevice> physical_devices(VkInstance instance) {
    std::uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) < VK_SUCCESS) {
        return {};
    }
    devices.resize(count); // VK_INCOMPLETE: a device went away in between
    return devices;
}

// RFC 0001's selection on one instance, which lists portability drivers or not.
Result<Selection> select(const ContextDesc& desc, bool portability) {
    const std::span<const char* const> extensions = desc.surface.has_value()
                                                        ? desc.surface->instance_extensions
                                                        : std::span<const char* const>{};
    Result<Instance> instance = Instance::create(
        {.extensions = extensions, .validation = desc.validation, .portability = portability});
    if (!instance.has_value()) {
        return instance.error();
    }

    Surface surface;
    if (desc.surface.has_value()) {
        const Result<VkSurfaceKHR> made = desc.surface->create(instance.value().handle());
        if (!made.has_value()) {
            return made.error();
        }
        surface = Surface(instance.value().handle(), made.value());
    }

    const std::vector<VkPhysicalDevice> devices = physical_devices(instance.value().handle());
    std::vector<DeviceInfo> infos;
    infos.reserve(devices.size());
    for (VkPhysicalDevice device : devices) {
        infos.push_back(describe_device(device, surface.handle()));
    }
    const Result<std::size_t> chosen = choose_device(infos);
    if (!chosen.has_value()) {
        return chosen.error();
    }
    return Selection{
        .instance = std::move(instance).value(),
        .surface = std::move(surface),
        .physical_device = devices[chosen.value()],
        .info = std::move(infos[chosen.value()]),
    };
}

} // namespace

Result<Context> Context::create(const ContextDesc& desc) {
    // An instance that does not opt in lists no portability driver, so MoltenVK
    // cannot win quietly. Only when that finds nothing, because no conformant
    // driver is installed or none of its devices passes the floor, opt in. That
    // instance lists every driver, so its error names every device.
    Result<Selection> selection = select(desc, false);
    if (!selection.has_value()) {
        selection = select(desc, true);
        if (!selection.has_value()) {
            return selection.error();
        }
    }

    Selection& chosen = selection.value();
    std::vector<const char*> extensions;
    if (chosen.surface.handle() != VK_NULL_HANDLE) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    Result<Device> device =
        Device::create(chosen.instance.handle(), chosen.physical_device, chosen.info, extensions);
    if (!device.has_value()) {
        return device.error();
    }
    return Context(std::move(chosen.instance), std::move(chosen.surface), std::move(device).value(),
                   std::move(chosen.info));
}

Context::Context(Instance instance, Surface surface, Device device, DeviceInfo device_info)
    : instance_(std::move(instance)), surface_(std::move(surface)), device_(std::move(device)),
      device_info_(std::move(device_info)) {}

} // namespace vx::render
