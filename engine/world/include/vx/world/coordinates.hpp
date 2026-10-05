#pragma once

#include <cstdint>

namespace vx {

// Chunks are cubes of 32 blocks a side (RFC 0002), with no fixed world
// height: they exist wherever generation puts them, up and down.
inline constexpr int chunk_bits = 5;
inline constexpr int chunk_size = 1 << chunk_bits;                        // 32
inline constexpr int chunk_area = chunk_size * chunk_size;                // 1,024 on a face
inline constexpr int chunk_volume = chunk_size * chunk_size * chunk_size; // 32,768

// World block coordinates: about 2.1 billion blocks either way on each axis.
struct BlockPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const BlockPos&, const BlockPos&) = default;
};

// World chunk coordinates: a block's coordinates shifted right by 5.
struct ChunkPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const ChunkPos&, const ChunkPos&) = default;
};

// A block's place inside its chunk: each coordinate 0 to 31.
struct LocalPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const LocalPos&, const LocalPos&) = default;
};

// The chunk a block is in. Since C++20, >> on a negative number is an
// arithmetic shift, so block -1 is in chunk -1.
constexpr ChunkPos chunk_of(BlockPos block) noexcept {
    return {.x = block.x >> chunk_bits, .y = block.y >> chunk_bits, .z = block.z >> chunk_bits};
}

// Where a block is inside its chunk: block -1 is at 31 in chunk -1.
constexpr LocalPos local_of(BlockPos block) noexcept {
    constexpr std::int32_t mask = chunk_size - 1;
    return {.x = block.x & mask, .y = block.y & mask, .z = block.z & mask};
}

// The block at `local` in `chunk`: chunk_of and local_of undone.
constexpr BlockPos block_at(ChunkPos chunk, LocalPos local) noexcept {
    // Since C++20, << on a negative number is defined too: it wraps.
    return {.x = (chunk.x << chunk_bits) | local.x,
            .y = (chunk.y << chunk_bits) | local.y,
            .z = (chunk.z << chunk_bits) | local.z};
}

// A block's index in its chunk's arrays. x varies fastest, then z, then y:
// one horizontal layer is 1,024 consecutive entries, since terrain is
// layered horizontally. Minecraft orders its chunks the same way.
constexpr std::uint16_t local_index(int x, int y, int z) noexcept {
    return static_cast<std::uint16_t>(x | (z << chunk_bits) | (y << (2 * chunk_bits)));
}

// The six faces of a block or a chunk, in RFC 0003's order.
enum class Face : std::uint8_t { pos_x, neg_x, pos_y, neg_y, pos_z, neg_z };

} // namespace vx
