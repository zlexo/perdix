#include <vx/render/chunk_arena.hpp>

#include <vx/core/result.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/device.hpp>

#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vx::render {

Result<ArenaSpace> ArenaSpace::create(std::uint32_t capacity_quads, std::uint32_t slots) {
    if (capacity_quads == 0 || slots == 0) {
        return Error{"an arena needs room for a quad and a frame slot"};
    }
    // The block counts quads, so an allocation's offset is its first quad.
    const VmaVirtualBlockCreateInfo info = {
        .size = capacity_quads,
        .flags = 0,
        .pAllocationCallbacks = nullptr,
    };
    ArenaSpace space;
    if (const VkResult created = vmaCreateVirtualBlock(&info, &space.block_);
        created != VK_SUCCESS) {
        return Error{"the arena's virtual block could not be made: VkResult " +
                     std::to_string(static_cast<int>(created))};
    }
    space.capacity_ = capacity_quads;
    space.retired_.resize(slots);
    return space;
}

ArenaSpace::ArenaSpace(ArenaSpace&& other) noexcept
    : block_(std::exchange(other.block_, VK_NULL_HANDLE)),
      capacity_(std::exchange(other.capacity_, 0U)), live_(std::exchange(other.live_, 0U)),
      ranges_(std::exchange(other.ranges_, 0U)),
      retired_quads_(std::exchange(other.retired_quads_, 0U)),
      retired_(std::exchange(other.retired_, {})) {}

ArenaSpace& ArenaSpace::operator=(ArenaSpace&& other) noexcept {
    if (this != &other) {
        destroy();
        block_ = std::exchange(other.block_, VK_NULL_HANDLE);
        capacity_ = std::exchange(other.capacity_, 0U);
        live_ = std::exchange(other.live_, 0U);
        ranges_ = std::exchange(other.ranges_, 0U);
        retired_quads_ = std::exchange(other.retired_quads_, 0U);
        retired_ = std::exchange(other.retired_, {});
    }
    return *this;
}

ArenaSpace::~ArenaSpace() {
    destroy();
}

void ArenaSpace::destroy() noexcept {
    if (block_ != VK_NULL_HANDLE) {
        // Every range goes with the block: the arena is going.
        vmaClearVirtualBlock(block_);
        vmaDestroyVirtualBlock(block_);
        block_ = VK_NULL_HANDLE;
    }
}

std::optional<ArenaRange> ArenaSpace::allocate(std::uint32_t quads) noexcept {
    assert(quads > 0);
    const VmaVirtualAllocationCreateInfo info = {
        .size = quads,
        .alignment = 0,
        .flags = 0,
        .pUserData = nullptr,
    };
    VmaVirtualAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    if (vmaVirtualAllocate(block_, &info, &allocation, &offset) != VK_SUCCESS) {
        return std::nullopt;
    }
    live_ += quads;
    ++ranges_;
    return ArenaRange{
        .first_quad = static_cast<std::uint32_t>(offset), .quads = quads, .allocation = allocation};
}

void ArenaSpace::retire(const ArenaRange& range, std::uint32_t slot) {
    assert(slot < retired_.size() && range.allocation != VK_NULL_HANDLE);
    retired_[slot].push_back(range);
    live_ -= range.quads;
    --ranges_;
    retired_quads_ += range.quads;
}

void ArenaSpace::reclaim(std::uint32_t slot) noexcept {
    assert(slot < retired_.size());
    for (const ArenaRange& range : retired_[slot]) {
        vmaVirtualFree(block_, range.allocation);
        retired_quads_ -= range.quads;
    }
    retired_[slot].clear();
}

ArenaStats ArenaSpace::stats() const noexcept {
    VmaDetailedStatistics detailed{};
    vmaCalculateVirtualBlockStatistics(block_, &detailed);
    const auto free = static_cast<std::uint32_t>(capacity_ - detailed.statistics.allocationBytes);
    const auto largest = free == 0 ? 0U : static_cast<std::uint32_t>(detailed.unusedRangeSizeMax);
    return {.capacity = capacity_,
            .live = live_,
            .retired = retired_quads_,
            .free = free,
            .largest_free = largest,
            .ranges = ranges_,
            .fragmentation =
                free == 0 ? 0.0 : 1.0 - (static_cast<double>(largest) / static_cast<double>(free))};
}

Result<ChunkArena> ChunkArena::create(const Device& device, std::uint32_t capacity_quads,
                                      std::uint32_t slots) {
    Result<ArenaSpace> space = ArenaSpace::create(capacity_quads, slots);
    if (!space.has_value()) {
        return space.error();
    }
    // Compaction and repack copy within it and out of it (RFC 0007).
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Result<Buffer> buffer = Buffer::create(device, quad_bytes * capacity_quads, usage,
                                           BufferMemory::upload_target, "chunk arena");
    if (!buffer.has_value()) {
        return buffer.error();
    }
    return ChunkArena(std::move(buffer.value()), std::move(space.value()));
}

} // namespace vx::render
