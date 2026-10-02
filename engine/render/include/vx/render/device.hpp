#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device_selection.hpp>

#include <vk_mem_alloc.h>
#include <volk.h>

#include <cstdint>

namespace vx::render {

// Reads what selection needs from a physical device. Feature structures newer
// than the device's Vulkan version are left out of the query, as the
// specification requires, so their features read as missing. With a window's
// surface, a queue family that can present to it comes first; headless, pass
// VK_NULL_HANDLE.
[[nodiscard]] DeviceInfo describe_device(VkPhysicalDevice physical_device, VkSurfaceKHR surface);

// Owns the logical device, its one queue and the VMA allocator. volk loads the
// device's functions into globals, straight from the driver, so only one
// Device may exist at a time.
class Device {
public:
    // Enables the feature floor, each optional feature the device offers, and
    // VK_KHR_portability_subset when the device lists it, as the specification
    // requires. `info` describes `physical_device`, which passed selection.
    [[nodiscard]] static Result<Device>
    create(VkInstance instance, VkPhysicalDevice physical_device, const DeviceInfo& info);

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return physical_device_; }
    [[nodiscard]] VkDevice handle() const noexcept { return device_; }
    // Graphics, compute and transfer all share it: RFC 0001 and RFC 0007.
    [[nodiscard]] VkQueue queue() const noexcept { return queue_; }
    [[nodiscard]] std::uint32_t queue_family() const noexcept { return queue_family_; }
    [[nodiscard]] VmaAllocator allocator() const noexcept { return allocator_; }

private:
    Device() = default;
    void destroy() noexcept;

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
};

} // namespace vx::render
