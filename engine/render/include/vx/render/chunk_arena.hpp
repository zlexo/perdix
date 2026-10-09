#pragma once

#include <vx/core/result.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/device.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace vx::render {

// A chunk mesh's place in the arena (RFC 0007), in RFC 0003's 8-byte quads.
struct ArenaRange {
    std::uint32_t first_quad = 0;
    std::uint32_t quads = 0;
    VmaVirtualAllocation allocation = VK_NULL_HANDLE;
};

// The arena's state, for the overlay and for deciding when to compact.
struct ArenaStats {
    std::uint32_t capacity = 0; // quads
    std::uint32_t live = 0;     // quads in ranges handed out and not retired
    std::uint32_t retired = 0;  // quads retired, waiting for their frame
    std::uint32_t free = 0;     // quads the next allocation may take
    std::uint32_t largest_free = 0;
    std::uint32_t ranges = 0; // live ranges
    // 1 less the largest free range over all free space: 0 when the free
    // space is one range, or when there is none.
    double fragmentation = 0.0;
};

// The arena's space (RFC 0007), without its buffer: a VMA virtual block
// (TLSF) counting quads, and for each frame slot the ranges freed while it
// was recorded, which go back to the block only when the slot comes round
// again, so no frame in flight reads a range after it is reused. Main thread
// only, as the arena is.
class ArenaSpace {
public:
    // Room for `capacity_quads` quads, for `slots` frame slots.
    [[nodiscard]] static Result<ArenaSpace> create(std::uint32_t capacity_quads,
                                                   std::uint32_t slots);
    ArenaSpace(ArenaSpace&& other) noexcept;
    ArenaSpace& operator=(ArenaSpace&& other) noexcept;
    ArenaSpace(const ArenaSpace&) = delete;
    ArenaSpace& operator=(const ArenaSpace&) = delete;
    ~ArenaSpace();

    // A range of `quads` quads, at least one; none when no free range fits,
    // which RFC 0007 answers by compacting or growing.
    [[nodiscard]] std::optional<ArenaRange> allocate(std::uint32_t quads) noexcept;
    // `range` is no longer drawn from: it goes back when frame slot `slot`,
    // the one being recorded, comes round again.
    void retire(const ArenaRange& range, std::uint32_t slot);
    // Frame slot `slot` comes round: the GPU is done with its frame, and the
    // ranges retired in it are free.
    void reclaim(std::uint32_t slot) noexcept;
    // Walks the block for its free ranges: a few times a second, not every
    // frame.
    [[nodiscard]] ArenaStats stats() const noexcept;

private:
    ArenaSpace() = default;
    void destroy() noexcept;

    VmaVirtualBlock block_ = VK_NULL_HANDLE;
    std::uint32_t capacity_ = 0;
    std::uint32_t live_ = 0;
    std::uint32_t ranges_ = 0;
    std::uint32_t retired_quads_ = 0;
    std::vector<std::vector<ArenaRange>> retired_; // by frame slot
};

// RFC 0007's arena: one buffer with a device address holding every chunk's
// mesh, and its space. The buffer is device-local and host-visible where the
// device has such memory, as unified memory does, and written through its
// mapping; elsewhere uploads stage into it (RFC 0001's upload).
class ChunkArena {
public:
    static constexpr VkDeviceSize quad_bytes = 8;

    // `memory` stays an upload target but in tests, which force the staged
    // path with BufferMemory::device_local.
    [[nodiscard]] static Result<ChunkArena>
    create(const Device& device, std::uint32_t capacity_quads, std::uint32_t slots,
           BufferMemory memory = BufferMemory::upload_target);

    [[nodiscard]] ArenaSpace& space() noexcept { return space_; }
    [[nodiscard]] const ArenaSpace& space() const noexcept { return space_; }
    [[nodiscard]] const Buffer& buffer() const noexcept { return buffer_; }

private:
    ChunkArena(Buffer buffer, ArenaSpace space) noexcept
        : buffer_(std::move(buffer)), space_(std::move(space)) {}

    Buffer buffer_;
    ArenaSpace space_;
};

} // namespace vx::render
