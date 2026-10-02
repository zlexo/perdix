#include <vx/render/buffer.hpp>

#include "vk_result.hpp"

#include <string>
#include <utility>

namespace vx::render {
namespace {

VmaAllocationCreateInfo allocation_info(BufferMemory memory) {
    VmaAllocationCreateInfo info{};
    switch (memory) {
    case BufferMemory::upload_target:
        // VMA prefers memory that is device-local and host-visible, and falls
        // back to device-local memory alone. Sequential writes alone would
        // demand host-visible memory, so a discrete GPU would never stage.
        info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;
        info.usage = VMA_MEMORY_USAGE_AUTO;
        break;
    case BufferMemory::device_local:
        info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }
    return info;
}

} // namespace

Result<Buffer> Buffer::create(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
                              BufferMemory memory) {
    if (size == 0) {
        return Error{"a buffer needs a size above zero"};
    }
    if (memory == BufferMemory::upload_target) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    const VmaAllocationCreateInfo memory_info = allocation_info(memory);

    Buffer buffer;
    buffer.allocator_ = device.allocator();
    buffer.size_ = size;
    VmaAllocationInfo allocated{};
    if (const VkResult result = vmaCreateBuffer(buffer.allocator_, &buffer_info, &memory_info,
                                                &buffer.buffer_, &buffer.allocation_, &allocated);
        result != VK_SUCCESS) {
        return Error{"vmaCreateBuffer failed for " + std::to_string(size) +
                     " bytes: " + vk_result_name(result)};
    }
    if (allocated.pMappedData != nullptr) {
        buffer.mapping_ = {static_cast<std::byte*>(allocated.pMappedData), size};
    }
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        const VkBufferDeviceAddressInfo address_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .pNext = nullptr,
            .buffer = buffer.buffer_,
        };
        buffer.address_ = vkGetBufferDeviceAddress(device.handle(), &address_info);
    }
    return buffer;
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)), address_(std::exchange(other.address_, 0)),
      mapping_(std::exchange(other.mapping_, {})) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        address_ = std::exchange(other.address_, 0);
        mapping_ = std::exchange(other.mapping_, {});
    }
    return *this;
}

Buffer::~Buffer() {
    destroy();
}

void Buffer::destroy() noexcept {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

} // namespace vx::render
