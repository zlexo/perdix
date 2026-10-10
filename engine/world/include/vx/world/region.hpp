#pragma once

#include <vx/world/coordinates.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace vx {

// A column of chunks: a chunk's position without its y.
struct ColumnPos {
    std::int32_t x = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const ColumnPos&, const ColumnPos&) = default;
};

// Chunks min_y to max_y of one column, both included; empty when min_y is
// above max_y.
struct ChunkRange {
    std::int32_t min_y = 0;
    std::int32_t max_y = -1;

    [[nodiscard]] constexpr bool empty() const noexcept { return min_y > max_y; }
    [[nodiscard]] constexpr bool contains(std::int32_t y) const noexcept {
        return min_y <= y && y <= max_y;
    }
    friend constexpr bool operator==(const ChunkRange&, const ChunkRange&) = default;
};

// The chunk coordinates a world of 32-bit block coordinates holds (RFC 0002).
inline constexpr std::int32_t lowest_chunk = std::numeric_limits<std::int32_t>::min() >> chunk_bits;
inline constexpr std::int32_t highest_chunk =
    std::numeric_limits<std::int32_t>::max() >> chunk_bits;

// RFC 0006's region, in whole chunks from the camera's chunk.
struct RegionSettings {
    std::int32_t view_distance = 32; // the terrain disc's radius
    std::int32_t near_distance = 8;  // the near cylinder's radius
    std::int32_t near_height = 8;    // its chunks below and above the camera's
    std::int32_t hysteresis = 2;     // how much larger the region a loaded chunk must leave
};

// The settings RFC 0006's memory budget lowers and raises while streaming:
// the terrain disc's radius and the hysteresis.
struct RegionLimits {
    std::int32_t view_distance = 32;
    std::int32_t hysteresis = 2;

    friend constexpr bool operator==(const RegionLimits&, const RegionLimits&) = default;
};

// The terrain band of a column (RFC 0006): its surface range, the chunks
// where anything solid can meet the sky, reaching down to the lowest surface
// of its four side neighbors, so that a cliff along a column's border is
// drawn. Empty when its surface range is. (M5's edits will stretch it to
// every edited chunk.)
[[nodiscard]] ChunkRange terrain_band(ChunkRange surface,
                                      std::span<const ChunkRange, 4> sides) noexcept;

// Sorted, disjoint runs of chunks in one column: as many as a planned column
// can need, one for each of the 25 bands it reads and one for the near
// cylinder, whatever the bands.
class ColumnRuns {
public:
    static constexpr std::size_t most = 26;

    // Adds a range, merging it with the runs it overlaps or touches. An
    // empty range adds nothing.
    void add(ChunkRange range) noexcept;
    [[nodiscard]] bool contains(std::int32_t y) const noexcept;
    [[nodiscard]] std::span<const ChunkRange> runs() const& noexcept {
        return std::span(runs_).first(count_);
    }
    // A temporary's runs would dangle once its full expression ends, as in
    // `for (run : region.drawn(...).runs())`: C++20 does not extend its
    // life. Name the runs first.
    [[nodiscard]] std::span<const ChunkRange> runs() const&& = delete;
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // Chunks in all the runs.
    [[nodiscard]] std::int64_t chunks() const noexcept;

private:
    std::array<ChunkRange, most> runs_{};
    std::size_t count_ = 0;
};

// RFC 0006's region around a camera, as pure functions of the camera's chunk,
// the settings and each column's terrain band. Drawn: the terrain disc, every
// column within the view distance with its band, and the near cylinder,
// every chunk within the near distance and height. Loaded: within one chunk
// of a drawn chunk, diagonals included, since meshing reads all 26
// neighbors. Planned: within one chunk of a loaded chunk, for RFC 0008's
// plans. Nothing outside the 32-bit world belongs to any of them.
class Region {
public:
    Region(ChunkPos camera, const RegionSettings& settings) noexcept
        : camera_(camera), settings_(settings) {}

    // The same region with every distance `by` chunks larger. Streaming keeps
    // a drawn chunk drawn until it leaves the drawn region grown by the
    // hysteresis, and loads and plans around the drawn chunks, so a loaded
    // chunk unloads only once it leaves the loaded region grown as much, and
    // a drawn chunk never loses a neighbor.
    [[nodiscard]] Region grown(std::int32_t by) const noexcept;

    // Whether a column holds drawn chunks: within the view distance or the
    // near distance.
    [[nodiscard]] bool column_drawn(ColumnPos column) const noexcept;
    // Within one column of a drawn one, diagonals included.
    [[nodiscard]] bool column_loaded(ColumnPos column) const noexcept;
    // Within one column of a loaded one.
    [[nodiscard]] bool column_planned(ColumnPos column) const noexcept;

    // The drawn chunks of `column`, whose terrain band is `band`.
    [[nodiscard]] ColumnRuns drawn(ColumnPos column, ChunkRange band) const noexcept;
    // The loaded chunks of `column`. `bands` holds the terrain bands of the
    // 3 by 3 columns centered on it, x fastest, then z: bands[4] is its own.
    [[nodiscard]] ColumnRuns loaded(ColumnPos column,
                                    std::span<const ChunkRange, 9> bands) const noexcept;
    // The planned chunks of `column`, from the bands of the 5 by 5 columns
    // centered on it, x fastest: bands[12] is its own.
    [[nodiscard]] ColumnRuns planned(ColumnPos column,
                                     std::span<const ChunkRange, 25> bands) const noexcept;

    [[nodiscard]] ChunkPos camera() const noexcept { return camera_; }
    [[nodiscard]] const RegionSettings& settings() const noexcept { return settings_; }

private:
    ChunkPos camera_;
    RegionSettings settings_;
};

} // namespace vx
