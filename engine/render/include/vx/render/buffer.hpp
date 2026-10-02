#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <vk_mem_alloc.h>
#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace vx::render {

// What a buffer's memory is for, which picks the memory (RFC 0001).
enum class BufferMemory : std::uint8_t {
    // Filled by the CPU, read by the GPU: device-local and host-visible where
    // the device has such memory, as unified memory does, and mapped there;
    // otherwise device-local, filled through a staging buffer.
    upload_target,
    // Written and read by the GPU only.
    device_local,
    // Written by the GPU, read by the CPU: host-visible, cached where the
    // device offers it, and always mapped.
    readback,
    // The CPU's side of a copy into GPU memory: host-visible and always mapped.
    staging,
};

// A buffer and its memory, from VMA.
class Buffer {
public:
    // `usage` says what the GPU does with the buffer. An upload target also
    // becomes a copy destination, for when its memory is not host-visible.
    [[nodiscard]] static Result<Buffer> create(const Device& device, VkDeviceSize size,
                                               VkBufferUsageFlags usage, BufferMemory memory);

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();

    [[nodiscard]] VkBuffer handle() const noexcept { return buffer_; }
    [[nodiscard]] VmaAllocation allocation() const noexcept { return allocation_; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
    // What shaders reach the buffer through; zero unless it was made with
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    [[nodiscard]] VkDeviceAddress address() const noexcept { return address_; }
    // The whole buffer, mapped for its lifetime; empty when its memory is not
    // host-visible, or not meant for the CPU.
    [[nodiscard]] std::span<std::byte> mapping() const noexcept { return mapping_; }

private:
    Buffer() = default;
    void destroy() noexcept;

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    VkDeviceAddress address_ = 0;
    std::span<std::byte> mapping_;
};

} // namespace vx::render
