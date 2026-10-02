#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device_selection.hpp>
#include <vx/render/instance.hpp>

#include <volk.h>

#include <span>

namespace vx::render {

struct ContextDesc {
    // The window library's surface extensions; empty when running headless.
    std::span<const char* const> extensions;
    // VK_LAYER_KHRONOS_validation with synchronization validation: debug builds.
    bool validation = false;
};

// The instance and the physical device the engine renders with, picked by RFC
// 0001's device selection: conformant drivers first, and portability drivers
// such as MoltenVK only when no conformant driver offers a device that passes
// the floor.
class Context {
public:
    // Fails naming every device and what it lacks when none qualifies.
    [[nodiscard]] static Result<Context> create(const ContextDesc& desc);

    [[nodiscard]] const Instance& instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return physical_device_; }
    [[nodiscard]] const DeviceInfo& device_info() const noexcept { return device_info_; }
    // The instance opts in to portability drivers for the fallback alone.
    [[nodiscard]] bool portability_fallback() const noexcept {
        return instance_.lists_portability_drivers();
    }

private:
    Context(Instance instance, VkPhysicalDevice physical_device, DeviceInfo device_info);

    Instance instance_;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    DeviceInfo device_info_;
};

} // namespace vx::render
