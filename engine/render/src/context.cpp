#include <vx/render/context.hpp>

#include <vx/render/device.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

// A device chosen on an instance, before its logical device exists.
struct Selection {
    Instance instance;
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
    Result<Instance> instance = Instance::create(
        {.extensions = desc.extensions, .validation = desc.validation, .portability = portability});
    if (!instance.has_value()) {
        return instance.error();
    }

    const std::vector<VkPhysicalDevice> devices = physical_devices(instance.value().handle());
    std::vector<DeviceInfo> infos;
    infos.reserve(devices.size());
    for (VkPhysicalDevice device : devices) {
        infos.push_back(describe_device(device));
    }
    const Result<std::size_t> chosen = choose_device(infos);
    if (!chosen.has_value()) {
        return chosen.error();
    }
    return Selection{
        .instance = std::move(instance).value(),
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
    Result<Device> device =
        Device::create(chosen.instance.handle(), chosen.physical_device, chosen.info);
    if (!device.has_value()) {
        return device.error();
    }
    return Context(std::move(chosen.instance), std::move(device).value(), std::move(chosen.info));
}

Context::Context(Instance instance, Device device, DeviceInfo device_info)
    : instance_(std::move(instance)), device_(std::move(device)),
      device_info_(std::move(device_info)) {}

} // namespace vx::render
