#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace vx {

// RFC 0011's levels of detail. A level-L chunk is RFC 0002's 32-block cube
// with cells 2^L blocks wide, so it spans 32 x 2^L blocks a side. Its
// position is floor(block / (32 x 2^L)) on each axis: every level's grid is
// fixed in the world, and a level-L chunk holds exactly the 8 level-(L-1)
// chunks inside it. Level 0 is today's chunk.
inline constexpr std::uint8_t lod_levels = 6; // cells of 1 to 32 blocks
inline constexpr std::uint8_t max_lod_level = lod_levels - 1;

struct LodPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint8_t level = 0;

    friend constexpr bool operator==(const LodPos&, const LodPos&) = default;
};

// A cell's width in blocks.
[[nodiscard]] constexpr std::int32_t cell_blocks(std::uint8_t level) noexcept {
    return std::int32_t{1} << level;
}

// The chunk at `level` that holds a block. >> is an arithmetic shift, so
// block -1 is in chunk -1 at every level.
[[nodiscard]] constexpr LodPos lod_of(BlockPos block, std::uint8_t level) noexcept {
    const int shift = chunk_bits + level;
    return {.x = block.x >> shift, .y = block.y >> shift, .z = block.z >> shift, .level = level};
}

// The chunk one level coarser that holds `pos`, which is below the top.
[[nodiscard]] constexpr LodPos parent(LodPos pos) noexcept {
    return {.x = pos.x >> 1,
            .y = pos.y >> 1,
            .z = pos.z >> 1,
            .level = static_cast<std::uint8_t>(pos.level + 1)};
}

// The 8 chunks one level finer inside `pos`, which is above level 0: x
// fastest, then z, then y, as RFC 0002 orders a chunk's blocks.
[[nodiscard]] constexpr std::array<LodPos, 8> children(LodPos pos) noexcept {
    std::array<LodPos, 8> out{};
    const auto level = static_cast<std::uint8_t>(pos.level - 1);
    for (std::int32_t i = 0; i < 8; ++i) {
        // Since C++20, << on a negative number is defined: it wraps.
        out[static_cast<std::size_t>(i)] = {.x = (pos.x << 1) | (i & 1),
                                            .y = (pos.y << 1) | ((i >> 2) & 1),
                                            .z = (pos.z << 1) | ((i >> 1) & 1),
                                            .level = level};
    }
    return out;
}

// The blocks a chunk spans on each axis, from `min` (inclusive) to `max`
// (exclusive). In 64 bits: a chunk at the world's edge ends at 2^31.
struct LodBox {
    std::int64_t min_x = 0;
    std::int64_t min_y = 0;
    std::int64_t min_z = 0;
    std::int64_t max_x = 0;
    std::int64_t max_y = 0;
    std::int64_t max_z = 0;

    friend constexpr bool operator==(const LodBox&, const LodBox&) = default;
};

[[nodiscard]] constexpr LodBox box_of(LodPos pos) noexcept {
    const int shift = chunk_bits + pos.level;
    const std::int64_t size = std::int64_t{1} << shift;
    const std::int64_t x = std::int64_t{pos.x} * size;
    const std::int64_t y = std::int64_t{pos.y} * size;
    const std::int64_t z = std::int64_t{pos.z} * size;
    return {.min_x = x,
            .min_y = y,
            .min_z = z,
            .max_x = x + size,
            .max_y = y + size,
            .max_z = z + size};
}

// Which level is drawn where (RFC 0011): pure functions of the camera and
// these settings, for the streamer and the tests.
struct LodRings {
    double pixels_per_cell = 4.0;             // k, the quality: 2 sharp to 8 fast
    std::uint32_t framebuffer_height = 1'440; // in pixels
    double fov_y = 1.2217304763960306;        // 70 degrees, in radians
    std::int32_t view_distance = 512;         // the terrain disc's radius, in chunks
    std::int32_t near_distance = 8;           // RFC 0006's near cylinder, in chunks,
    std::int32_t near_height = 8;             // always drawn at level 0

    // The distance at which a block spans k pixels on screen: level L
    // starts at 2^L times it. Never below the floor at which chunks drawn
    // side by side could be two levels apart (lod.cpp says why).
    [[nodiscard]] double unit() const noexcept;
    // Where a level starts, in blocks: level 0 at 0.
    [[nodiscard]] double radius(std::uint8_t level) const noexcept;
    // The level a chunk's place calls for, the same for every chunk of its
    // column: 0 for a column reaching into the near cylinder, else the
    // coarsest level whose ring starts at or before the column's nearest
    // point, across. By column, not by the box's distance in three
    // dimensions as RFC 0011 first had it: a coarse level's surface can lie
    // a row below the finer one's, so a column drawn at two levels could
    // draw its ground at neither.
    [[nodiscard]] std::uint8_t level_at(const Vec3d& eye, LodPos chunk) const noexcept;
    // Whether a chunk reaches into the terrain disc: one of its columns is
    // within the view distance of the camera's, as RFC 0006 counts it.
    [[nodiscard]] bool in_view(const Vec3d& eye, LodPos chunk) const noexcept;
    // Whether a chunk's column reaches into the near cylinder, whose
    // columns are drawn at level 0 at every height, whatever the distance
    // says.
    [[nodiscard]] bool near(const Vec3d& eye, LodPos chunk) const noexcept;
    // Whether a chunk is drawn: in view, its place calls for its level or a
    // coarser one, and its parent's for a finer one (the top level has no
    // parent to refine).
    [[nodiscard]] bool drawn(const Vec3d& eye, LodPos chunk) const noexcept;
    // How far a level streams: the radius of the disc of its columns, in its
    // own chunks around the camera's chunk at the level, as RFC 0006's
    // region counts it, that holds every chunk of the level lod_cover can
    // draw, wherever the eye is in the camera's chunk and however high. A
    // full disc, since a high camera draws coarse chunks straight below it;
    // -1, no columns, for a negative view distance.
    [[nodiscard]] std::int32_t stream_distance(std::uint8_t level) const noexcept;
};

// Every chunk drawn (RFC 0011) between level-0 chunk rows `min_y` and
// `max_y`, within the 32-bit world: a partition of those rows of the
// terrain disc's columns, each chunk within a level of its neighbors.
[[nodiscard]] std::vector<LodPos> lod_cover(const LodRings& rings, const Vec3d& eye,
                                            std::int32_t min_y, std::int32_t max_y);

// What a chunk offers RFC 0011's swaps, as streaming knows it.
enum class LodReady : std::uint8_t {
    waiting, // its mesh is not ready yet
    mesh,    // its mesh is ready, and has faces to draw
    empty,   // its mesh is ready, and draws nothing
    outside, // outside its level's terrain: nothing in it draws, at any level
};

// RFC 0011's swaps: the chunks to draw now for lod_cover's chunks between
// the same rows, among those `ready` says can be, given the chunks drawn
// before, `drawn`. Top down from the top level: a chunk whose place calls
// for a finer level gives way to its 8 children once all of them can be
// drawn, and until then stays, when ready; a chunk at its level is drawn
// once ready. Where neither can be, what was drawn there before stays, if
// all of it still can be: so a coarse chunk goes only with all of its
// children that have faces, and children go only once their parent can be
// drawn, however far the camera moved. A chunk outside its level's terrain
// ends the descent. The chunks with faces are listed; with every chunk
// ready they are lod_cover's.
[[nodiscard]] std::vector<LodPos> lod_select(const LodRings& rings, const Vec3d& eye,
                                             std::int32_t min_y, std::int32_t max_y,
                                             const std::function<LodReady(LodPos)>& ready,
                                             std::span<const LodPos> drawn = {});

} // namespace vx
