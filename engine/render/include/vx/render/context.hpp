#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>
#include <vx/render/device_selection.hpp>
#include <vx/render/instance.hpp>

#include <span>

namespace vx::render {

struct ContextDesc {
    // The window library's surface extensions; empty when running headless.
    std::span<const char* const> extensions;
    // VK_LAYER_KHRONOS_validation with synchronization validation: debug builds.
    bool validation = false;
};

// The instance and the device the engine renders with, picked by RFC 0001's
// device selection: conformant drivers first, and portability drivers such as
// MoltenVK only when no conformant driver offers a device that passes the floor.
class Context {
public:
    // Fails naming every device and what it lacks when none qualifies.
    [[nodiscard]] static Result<Context> create(const ContextDesc& desc);

    Context(Context&& other) noexcept = default;
    // Members are assigned in order, which would destroy the old instance
    // before the old device.
    Context& operator=(Context&& other) = delete;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context() = default;

    [[nodiscard]] const Instance& instance() const noexcept { return instance_; }
    [[nodiscard]] const Device& device() const noexcept { return device_; }
    [[nodiscard]] const DeviceInfo& device_info() const noexcept { return device_info_; }
    // The instance opts in to portability drivers for the fallback alone.
    [[nodiscard]] bool portability_fallback() const noexcept {
        return instance_.lists_portability_drivers();
    }

private:
    Context(Instance instance, Device device, DeviceInfo device_info);

    Instance instance_; // declared first, so it is destroyed last
    Device device_;
    DeviceInfo device_info_;
};

} // namespace vx::render
