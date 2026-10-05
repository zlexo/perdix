#pragma once

#include <vx/core/result.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/device.hpp>
#include <vx/render/image.hpp>

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
    friend Result<void> upload_image(UploadContext& context, VkCommandBuffer commands,
                                     const Image& image, std::span<const std::byte> pixels);
    // A staging buffer holding `bytes`, kept until reset(); its handle.
    [[nodiscard]] Result<VkBuffer> stage(std::span<const std::byte> bytes);

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

// Copies `pixels` into all of `image`, a color image of four bytes a texel:
// level by level, every layer of a level before the next level, each layer
// rows of texels with nothing between them, as build_block_textures lays out
// RFC 0004's array. One copy, with a region per level. Images always go
// through a staging buffer: their memory is laid out for the GPU, so the CPU
// cannot write it through a mapping. Records the image's move from undefined
// to a copy destination, which waits for every earlier command and discards
// what the image held, then the copy, then its move to the layout shaders
// read, each over every level and layer.
[[nodiscard]] Result<void> upload_image(UploadContext& context, VkCommandBuffer commands,
                                        const Image& image, std::span<const std::byte> pixels);

} // namespace vx::render
