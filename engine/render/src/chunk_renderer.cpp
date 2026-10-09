#include <vx/render/chunk_renderer.hpp>

#include "vk_result.hpp"

#include <vx/core/frustum.hpp>
#include <vx/core/result.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/device.hpp>
#include <vx/render/draw_lists.hpp>
#include <vx/render/upload.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vk_mem_alloc.h>
#include <volk.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

// Each frame slot's buffer starts with room for the origins and commands of
// about sixty chunks, and doubles when a frame needs more: a world's first
// frames grow it a few times, and so do the scene tests, which exercise it.
constexpr VkDeviceSize first_frame_bytes = VkDeviceSize{4} << 10U;

constexpr VkBufferUsageFlags frame_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

Result<Buffer> make_frame_buffer(const Device& device, VkDeviceSize size, std::uint32_t slot) {
    return Buffer::create(device, size, frame_usage, BufferMemory::per_frame,
                          "frame slot " + std::to_string(slot) + " chunk draws");
}

// A write past the mapping lands in whatever VMA placed beside it: no
// sanitizer sees it, and the frame then hangs the GPU.
template <class T>
void copy_into(std::span<std::byte> to, VkDeviceSize offset, const std::vector<T>& from) {
    const std::size_t bytes = from.size() * sizeof(T);
    assert(offset <= to.size() && bytes <= to.size() - offset);
    if (bytes > 0) {
        std::memcpy(to.subspan(offset).data(), from.data(), bytes);
    }
}

} // namespace

Result<ChunkRenderer> ChunkRenderer::create(const Device& device, std::uint32_t capacity_quads,
                                            std::uint32_t slots, BufferMemory arena_memory,
                                            std::uint32_t draw_call_limit) {
    if (capacity_quads > max_arena_quads) {
        return Error{"a chunk arena holds at most " + std::to_string(max_arena_quads) +
                     " quads, not " + std::to_string(capacity_quads)};
    }
    Result<ChunkArena> arena = ChunkArena::create(device, capacity_quads, slots, arena_memory);
    if (!arena.has_value()) {
        return arena.error();
    }
    std::vector<UploadContext> staging;
    std::vector<Buffer> frames;
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        staging.emplace_back(device);
        Result<Buffer> frame = make_frame_buffer(device, first_frame_bytes, slot);
        if (!frame.has_value()) {
            return frame.error();
        }
        frames.push_back(std::move(frame).value());
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    const std::uint32_t device_limit = properties.limits.maxDrawIndirectCount;
    return ChunkRenderer(std::make_unique<ChunkArena>(std::move(arena).value()), std::move(staging),
                         std::move(frames),
                         draw_call_limit == 0 ? device_limit
                                              : std::min(draw_call_limit, device_limit));
}

ChunkRenderer::ChunkRenderer(std::unique_ptr<ChunkArena> arena, std::vector<UploadContext> staging,
                             std::vector<Buffer> frames, std::uint32_t draw_limit) noexcept
    : arena_(std::move(arena)), uploads_(arena_->space()), staging_(std::move(staging)),
      frames_(std::move(frames)), draw_limit_(draw_limit) {}

void ChunkRenderer::begin_frame(const Device& device, std::uint32_t slot) {
    assert(slot < frames_.size());
    slot_ = slot;
    uploads_.begin_frame(slot, ChunkUploads::Clock::now());
    // The GPU is done with the slot's copies: their staging buffers go. Made
    // afresh, so a renderer moved since holds no stale device.
    staging_[slot] = UploadContext(device);
}

Result<void> ChunkRenderer::apply(ChunkChanges& changes, VkCommandBuffer commands) {
    UploadContext& staging = staging_[slot_];
    ArenaWriter writer(staging, commands, *arena_);
    ChunkUploader uploader(uploads_, writer);
    const Result<void> applied = changes.apply(uploader);
    staging.finish(commands);
    // After an error the frame is never submitted: copies staged before it
    // never run, though the records already name their ranges.
    return applied;
}

Result<ChunkFrame> ChunkRenderer::prepare(const Device& device, const glm::i64vec3& camera_block,
                                          const glm::vec3& camera_fraction, const Frustum& view) {
    build_draw_lists(uploads_.records(), camera_block, camera_fraction, view, switches_, lists_);
    constexpr VkDeviceSize command_bytes = sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize origins_bytes = lists_.origins.size() * sizeof(glm::vec4);
    const VkDeviceSize opaque_offset = origins_bytes;
    const VkDeviceSize cutout_offset = opaque_offset + (lists_.opaque.size() * command_bytes);
    const VkDeviceSize needed = cutout_offset + (lists_.cutout.size() * command_bytes);

    // The slot's last frame is done, so its buffer may be replaced.
    Buffer& frame = frames_[slot_];
    if (frame.size() < needed) {
        Result<Buffer> grown = make_frame_buffer(device, std::max(needed, 2 * frame.size()), slot_);
        if (!grown.has_value()) {
            return grown.error();
        }
        frame = std::move(grown).value();
    }
    const std::span<std::byte> mapping = frame.mapping();
    copy_into(mapping, 0, lists_.origins);
    copy_into(mapping, opaque_offset, lists_.opaque);
    copy_into(mapping, cutout_offset, lists_.cutout);
    if (needed > 0) {
        if (const VkResult result =
                vmaFlushAllocation(device.allocator(), frame.allocation(), 0, needed);
            result != VK_SUCCESS) {
            return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
        }
    }
    return ChunkFrame{.buffer = frame.handle(),
                      .origins = frame.address(),
                      .opaque_offset = opaque_offset,
                      .opaque = static_cast<std::uint32_t>(lists_.opaque.size()),
                      .cutout_offset = cutout_offset,
                      .cutout = static_cast<std::uint32_t>(lists_.cutout.size())};
}

} // namespace vx::render
