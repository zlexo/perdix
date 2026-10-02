#pragma once

#include <vx/core/result.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/device.hpp>

#include <volk.h>

#include <cstddef>
#include <span>
#include <vector>

namespace vx::render {

// The staging side of uploads (RFC 0001): the staging buffers of copies
// recorded but not yet executed. Each frame slot gets its own, reset once the
// GPU is done with the slot's commands.
class UploadContext {
public:
    explicit UploadContext(const Device& device) noexcept : device_(&device) {}

    // Records one barrier that makes the copies staged since the last finish
    // visible to every later command; records nothing when nothing was staged.
    void finish(VkCommandBuffer commands);

    // Frees the staging buffers: the GPU must be done with every copy
    // recorded through this context.
    void reset() noexcept;

    // The bytes that went through staging since the last reset.
    [[nodiscard]] VkDeviceSize staged_bytes() const noexcept { return staged_bytes_; }

private:
    friend Result<void> upload(UploadContext& context, VkCommandBuffer commands, const Buffer& dst,
                               VkDeviceSize dst_offset, std::span<const std::byte> bytes);

    const Device* device_;
    std::vector<Buffer> staging_;
    VkDeviceSize staged_bytes_ = 0;
    bool unfinished_ = false; // copies recorded since the last finish
};

// Copies `bytes` into `dst` at `dst_offset`: through dst's mapping when it has
// one, flushed where its memory is not coherent; otherwise into a staging
// buffer, recording the copy into `commands`, and context.finish() makes it
// visible. Running past the end of `dst` is an error.
[[nodiscard]] Result<void> upload(UploadContext& context, VkCommandBuffer commands,
                                  const Buffer& dst, VkDeviceSize dst_offset,
                                  std::span<const std::byte> bytes);

} // namespace vx::render
