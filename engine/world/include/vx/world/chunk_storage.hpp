#pragma once

#include <vx/world/block_state.hpp>
#include <vx/world/packed_array.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace vx {

// A chunk's 32,768 blocks (RFC 0002), stored in one of three modes:
//
// - uniform: one state fills the chunk, which owns no memory;
// - paletted: 2 to 256 distinct states, each block an index of 1, 2, 4 or
//   8 bits into a palette;
// - direct: more than 256, each block its 16-bit state.
//
// A move-only value with no locks inside: which thread may touch a chunk is
// RFC 0005's design, and the const methods write nothing.
class ChunkStorage {
public:
    // Uniform air, with no allocation.
    ChunkStorage() noexcept = default;
    // Uniform `state`, with no allocation.
    [[nodiscard]] static ChunkStorage uniform(BlockState state) noexcept;

    // A move leaves `other` uniform air.
    ChunkStorage(ChunkStorage&& other) noexcept;
    ChunkStorage& operator=(ChunkStorage&& other) noexcept;
    // Copies are explicit, through clone(): an edit to a chunk that jobs are
    // reading writes to a copy (RFC 0005), never by accident.
    ChunkStorage(const ChunkStorage&) = delete;
    ChunkStorage& operator=(const ChunkStorage&) = delete;
    ~ChunkStorage() = default;

    [[nodiscard]] ChunkStorage clone() const;

    // The block at `index` (local_index): a branch on the mode, then a
    // shift, a mask and, when paletted, a palette lookup.
    [[nodiscard]] BlockState get(std::uint16_t index) const noexcept {
        if (palette_ != nullptr) {
            return palette_[indices_.get(index)];
        }
        if (indices_.empty()) {
            return uniform_;
        }
        return static_cast<BlockState>(indices_.get(index));
    }

    // Writes in place, once a scan of the palette finds the state's entry. A
    // state the palette has no room for takes an entry no block uses any
    // more, or else the width doubles: 1, 2, 4, then 8 bits, then direct.
    void set(std::uint16_t index, BlockState state);

    // Drops palette entries no block uses any more, when that lets the chunk
    // shrink: to a narrower width, to uniform once one state is left, or from
    // direct back to a palette once 256 or fewer are. A write, which the
    // world thread runs as a pass of its own, on chunks no job is reading
    // (RFC 0002).
    void compact();

    [[nodiscard]] bool is_uniform() const noexcept { return indices_.empty(); }
    // 0 when uniform, 1, 2, 4 or 8 when paletted, 16 when direct.
    [[nodiscard]] int bits_per_block() const noexcept { return indices_.bits(); }
    // The block data and the palette: the memory the chunk owns beside its
    // header, which is sizeof(ChunkStorage).
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

private:
    // The entry to write at `index` for `state`, which the palette does not
    // hold: a free entry, one no block but `index` uses, or one made by
    // growing. Once direct, the state itself.
    [[nodiscard]] std::uint32_t add_to_palette(std::uint16_t index, BlockState state);
    // Doubles the width, or goes direct from 8 bits.
    void grow();
    // compact() for a direct chunk.
    void compact_direct();
    // How many blocks hold each palette entry.
    [[nodiscard]] std::array<std::uint16_t, 256> count_uses() const noexcept;
    [[nodiscard]] std::uint32_t palette_capacity() const noexcept;

    // 32 bytes in all, RFC 0002's budget for a uniform chunk's header.
    PackedArray indices_; // empty when uniform
    // 2^bits entries when paletted, else none.
    std::unique_ptr<BlockState[]> palette_; // NOLINT(modernize-avoid-c-arrays)
    std::uint16_t palette_size_ = 0;        // entries in use, live or not
    BlockState uniform_ = BlockState::air;  // the state of a uniform chunk
};

} // namespace vx
