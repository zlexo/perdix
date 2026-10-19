#include <vx/render/chunk_renderer.hpp>

#include "vk_result.hpp"

#include <vx/core/frustum.hpp>
#include <vx/core/result.hpp>
#include <vx/render/arena_upkeep.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/camera_relative.hpp>
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
#include <array>
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

// A quarter of the device's memory budget, in quads (RFC 0007): the largest
// budget VMA reports for a device-local heap, which is VK_EXT_memory_budget's
// where the device offers it and enabled, and 80% of the heap otherwise.
std::uint32_t quarter_of_budget(const Device& device) {
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(device.allocator(), &memory);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(device.allocator(), budgets.data());
    VkDeviceSize largest = 0;
    for (std::uint32_t heap = 0; heap < memory->memoryHeapCount; ++heap) {
        if ((memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            largest = std::max(largest, budgets.at(heap).budget);
        }
    }
    return static_cast<std::uint32_t>(std::min<VkDeviceSize>(largest / 4 / ChunkArena::quad_bytes,
                                                             ChunkRenderer::max_arena_quads));
}

UpkeepSettings upkeep_settings(std::uint32_t arena_limit, std::uint32_t slots) {
    UpkeepSettings settings;
    settings.max_capacity = arena_limit;
    settings.slots = slots;
    return settings;
}

// After copies into the arena, whatever reads or writes it next, vertex
// pulling or another copy, sees them.
void record_copy_barrier(VkCommandBuffer commands) {
    const VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
}

} // namespace

Result<ChunkRenderer> ChunkRenderer::create(const Device& device, std::uint32_t capacity_quads,
                                            std::uint32_t slots, BufferMemory arena_memory,
                                            std::uint32_t draw_call_limit,
                                            std::uint32_t arena_limit) {
    if (capacity_quads > max_arena_quads) {
        return Error{"a chunk arena holds at most " + std::to_string(max_arena_quads) +
                     " quads, not " + std::to_string(capacity_quads)};
    }
    const std::uint32_t limit =
        arena_limit == 0 ? quarter_of_budget(device) : std::min(arena_limit, max_arena_quads);
    Result<ChunkArena> arena =
        ChunkArena::create(device, std::min(capacity_quads, limit), slots, arena_memory);
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
    return ChunkRenderer(std::make_unique<ChunkArena>(std::move(arena).value()), arena_memory,
                         limit, slots, std::move(staging), std::move(frames),
                         draw_call_limit == 0 ? device_limit
                                              : std::min(draw_call_limit, device_limit));
}

ChunkRenderer::ChunkRenderer(std::unique_ptr<ChunkArena> arena, BufferMemory arena_memory,
                             std::uint32_t arena_limit, std::uint32_t slots,
                             std::vector<UploadContext> staging, std::vector<Buffer> frames,
                             std::uint32_t draw_limit) noexcept
    : arena_(std::move(arena)), arena_memory_(arena_memory), uploads_(arena_->space()),
      upkeep_(upkeep_settings(arena_limit, slots)), replaced_(slots), staging_(std::move(staging)),
      frames_(std::move(frames)), draw_limit_(draw_limit) {}

void ChunkRenderer::begin_frame(const Device& device, std::uint32_t slot) {
    assert(slot < frames_.size());
    slot_ = slot;
    wanted_ = uploads_.stats().wanted;
    uploads_.begin_frame(slot, ChunkUploads::Clock::now());
    replaced_[slot].clear();
    // The GPU is done with the slot's copies: their staging buffers go. Made
    // afresh, so a renderer moved since holds no stale device.
    staging_[slot] = UploadContext(device);
}

Result<void> ChunkRenderer::keep_arena(const Device& device, VkCommandBuffer commands) {
    moves_.clear();
    switch (upkeep_.step(arena_->space(), wanted_)) {
    case UpkeepStep::none:
        return {};
    case UpkeepStep::compact: {
        const UpkeepSettings& settings = upkeep_.settings();
        upkeep_.compacted(uploads_.compact(settings.moves, settings.move_quads, moves_));
        record_moves(commands, arena_->buffer(), arena_->buffer());
        return {};
    }
    case UpkeepStep::repack:
        return repack(device, commands, upkeep_.repack_capacity());
    }
    return {};
}

Result<void> ChunkRenderer::repack(const Device& device, VkCommandBuffer commands,
                                   std::uint32_t capacity) {
    const std::uint32_t current = arena_->space().capacity();
    Result<ChunkArena> fresh = ChunkArena::create(
        device, capacity, static_cast<std::uint32_t>(frames_.size()), arena_memory_);
    // The device has no memory for it: the arena stays, and is full.
    if (!fresh.has_value()) {
        repack_error_ = fresh.error().message;
        upkeep_.repacked(false, current);
        return {};
    }
    if (!uploads_.repack(fresh.value().space(), moves_)) {
        repack_error_ =
            "the meshes did not fit a repack's arena of " + std::to_string(capacity) + " quads";
        upkeep_.repacked(false, current);
        return {};
    }
    // The uploads point at the arena's space, which now holds the fresh one.
    ChunkArena old = std::exchange(*arena_, std::move(fresh).value());
    record_moves(commands, old.buffer(), arena_->buffer());
    // Frames in flight draw from the old buffer, and this frame copies out of
    // it: it goes when this frame's slot comes round.
    replaced_[slot_].push_back(std::move(old));
    upkeep_.repacked(true, current);
    return {};
}

void ChunkRenderer::record_moves(VkCommandBuffer commands, const Buffer& from, const Buffer& to) {
    if (moves_.empty()) {
        return;
    }
    // Neighbors that stay neighbors copy as one region: a repack moves runs
    // of meshes that lay side by side.
    regions_.clear();
    for (const ArenaMove& move : moves_) {
        const VkDeviceSize source = VkDeviceSize{move.from} * ChunkArena::quad_bytes;
        const VkDeviceSize target = VkDeviceSize{move.to} * ChunkArena::quad_bytes;
        const VkDeviceSize bytes = VkDeviceSize{move.quads} * ChunkArena::quad_bytes;
        if (!regions_.empty() && regions_.back().srcOffset + regions_.back().size == source &&
            regions_.back().dstOffset + regions_.back().size == target) {
            regions_.back().size += bytes;
            continue;
        }
        regions_.push_back({.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                            .pNext = nullptr,
                            .srcOffset = source,
                            .dstOffset = target,
                            .size = bytes});
    }
    const VkCopyBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .pNext = nullptr,
        .srcBuffer = from.handle(),
        .dstBuffer = to.handle(),
        .regionCount = static_cast<std::uint32_t>(regions_.size()),
        .pRegions = regions_.data(),
    };
    vkCmdCopyBuffer2(commands, &copy);
    record_copy_barrier(commands);
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
                                          const glm::vec3& camera_fraction, const Frustum& view,
                                          float fade_seconds) {
    const ChunkFade fade{.now = uploads_.frame_start(),
                         .time = std::chrono::duration<float>(fade_seconds)};
    // RFC 0011's open border sets: a chunk is drawn while its record is shown.
    const DrawnAt drawn_at = [this](ChunkPos pos, std::uint8_t level) {
        const ChunkMeshRecord* record = uploads_.find(pos, level);
        return record != nullptr && record->shown;
    };
    if (frozen_ && !frozen_view_.has_value()) {
        frozen_view_ = CullingView{
            .camera_block = camera_block, .camera_fraction = camera_fraction, .frustum = view};
    }
    if (frozen_view_.has_value()) {
        // Culled as the frozen camera saw it, drawn from where the camera
        // is: each origin moves from the frozen camera's block to this one's,
        // exactly while the camera has moved under 2^24 blocks since.
        const CullingView& culling = *frozen_view_;
        build_draw_lists(uploads_.records(), culling.camera_block, culling.camera_fraction,
                         culling.frustum, switches_, lists_, drawn_at, fade);
        const glm::vec4 shift(relative_origin(culling.camera_block, camera_block), 0.0F);
        for (glm::vec4& origin : lists_.origins) {
            origin += shift;
        }
    } else {
        build_draw_lists(uploads_.records(), camera_block, camera_fraction, view, switches_, lists_,
                         drawn_at, fade);
    }
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
    return ChunkFrame{.quads = arena_->buffer().address(),
                      .buffer = frame.handle(),
                      .origins = frame.address(),
                      .opaque_offset = opaque_offset,
                      .opaque = static_cast<std::uint32_t>(lists_.opaque.size()),
                      .cutout_offset = cutout_offset,
                      .cutout = static_cast<std::uint32_t>(lists_.cutout.size())};
}

} // namespace vx::render
