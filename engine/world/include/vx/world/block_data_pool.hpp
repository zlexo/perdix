#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vx {

// One size class of the block data pool, as stats() reports it.
struct BlockDataClass {
    std::size_t block_bytes = 0; // 4 KiB at 1 bit, up to 64 KiB at 16
    std::size_t in_use = 0;      // blocks that packed arrays hold
    std::size_t cached = 0;      // blocks freed and kept for reuse
};

// Memory for chunks' block data (RFC 0002): a size class for each width a
// packed array takes, 4, 8, 16, 32 and 64 KiB, each with a free list, so
// loading and unloading chunks for hours reuses blocks instead of
// fragmenting the heap. Workers allocate block data and the world thread
// frees it (RFC 0005), so each class takes a lock of its own; allocation is
// rare (a generated chunk, a width change, a clone), so the lock costs
// little. Freed blocks stay cached for the life of the process.
namespace block_data {

// Classes by log2 of the width: 0 for 1 bit, up to 4 for 16.
inline constexpr int classes = 5;

// A zeroed block for a packed array 2^log2_bits bits wide: the class's last
// freed block when it has one, else a new one from the heap.
[[nodiscard]] std::uint64_t* allocate(int log2_bits);
// Back to its class, for allocate to hand out again.
void deallocate(std::uint64_t* words, int log2_bits) noexcept;
// Each class's counts, each read under its class's lock.
[[nodiscard]] std::array<BlockDataClass, classes> stats();

} // namespace block_data
} // namespace vx
