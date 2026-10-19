#include <vx/world/region.hpp>

#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vx {
namespace {

// Whether `column` is within `radius` columns of `center`, on a circle. The
// square test comes after the cheap one, so no square overflows.
bool within(ColumnPos center, ColumnPos column, std::int32_t radius) noexcept {
    if (radius < 0) {
        return false;
    }
    const std::int64_t dx = std::int64_t{column.x} - center.x;
    const std::int64_t dz = std::int64_t{column.z} - center.z;
    if (dx < -radius || dx > radius || dz < -radius || dz > radius) {
        return false;
    }
    return (dx * dx) + (dz * dz) <= std::int64_t{radius} * radius;
}

bool in_world(std::int64_t coordinate, std::uint8_t level) noexcept {
    return coordinate >= lowest_chunk_at(level) && coordinate <= highest_chunk_at(level);
}

bool in_world(ColumnPos column, std::uint8_t level) noexcept {
    return in_world(column.x, level) && in_world(column.z, level);
}

// The chunks `low` to `high` that the world holds at `level`; empty when
// none.
ChunkRange world_range(std::int64_t low, std::int64_t high, std::uint8_t level) noexcept {
    const std::int64_t min_y = std::max<std::int64_t>(low, lowest_chunk_at(level));
    const std::int64_t max_y = std::min<std::int64_t>(high, highest_chunk_at(level));
    if (min_y > max_y) {
        return {};
    }
    return {.min_y = static_cast<std::int32_t>(min_y), .max_y = static_cast<std::int32_t>(max_y)};
}

ChunkRange widened(ChunkRange range, std::int32_t by, std::uint8_t level) noexcept {
    if (range.empty()) {
        return {};
    }
    return world_range(std::int64_t{range.min_y} - by, std::int64_t{range.max_y} + by, level);
}

ColumnPos offset(ColumnPos column, std::int32_t dx, std::int32_t dz) noexcept {
    // Columns past the world's edge wrap here, but in_world turns them away
    // first: offsets are a few columns, and the edge is 2^26 from 2^31.
    return {.x = column.x + dx, .z = column.z + dz};
}

} // namespace

ChunkRange terrain_band(ChunkRange surface, std::span<const ChunkRange, 4> sides) noexcept {
    if (surface.empty()) {
        return {};
    }
    std::int32_t lowest = surface.min_y;
    for (const ChunkRange& side : sides) {
        if (!side.empty()) {
            lowest = std::min(lowest, side.min_y);
        }
    }
    return {.min_y = lowest, .max_y = surface.max_y};
}

void ColumnRuns::add(ChunkRange range) noexcept {
    if (range.empty()) {
        return;
    }
    // Every run the range overlaps or touches joins it; the others stay.
    std::array<ChunkRange, most> kept{};
    std::size_t count = 0;
    ChunkRange merged = range;
    for (const ChunkRange& run : runs()) {
        const bool apart = std::int64_t{run.max_y} + 1 < merged.min_y ||
                           std::int64_t{merged.max_y} + 1 < run.min_y;
        if (apart) {
            kept[count++] = run;
        } else {
            merged = {.min_y = std::min(run.min_y, merged.min_y),
                      .max_y = std::max(run.max_y, merged.max_y)};
        }
    }
    assert(count < most && "more runs in a column than a region makes");
    std::size_t at = count;
    while (at > 0 && kept[at - 1].min_y > merged.min_y) {
        kept[at] = kept[at - 1];
        --at;
    }
    kept[at] = merged;
    runs_ = kept;
    count_ = count + 1;
}

bool ColumnRuns::contains(std::int32_t y) const noexcept {
    return std::ranges::any_of(runs(), [y](const ChunkRange& run) { return run.contains(y); });
}

std::int64_t ColumnRuns::chunks() const noexcept {
    std::int64_t total = 0;
    for (const ChunkRange& run : runs()) {
        total += std::int64_t{run.max_y} - run.min_y + 1;
    }
    return total;
}

Region Region::grown(std::int32_t by) const noexcept {
    RegionSettings settings = settings_;
    settings.view_distance += by;
    // No cylinder stays none: a ring's -1 grows to nothing.
    settings.near_distance = settings.near_distance < 0 ? -1 : settings.near_distance + by;
    settings.near_height += by;
    // A ring's hole shrinks as the ring grows; past nothing it is gone.
    settings.inner_distance = settings.inner_distance < 0 ? -1 : settings.inner_distance - by;
    return {camera_, settings};
}

bool Region::in_terrain(ColumnPos column) const noexcept {
    const ColumnPos center{.x = camera_.x, .z = camera_.z};
    return within(center, column, settings_.view_distance) &&
           !within(center, column, settings_.inner_distance);
}

bool Region::column_drawn(ColumnPos column) const noexcept {
    const ColumnPos center{.x = camera_.x, .z = camera_.z};
    return in_world(column, settings_.level) &&
           (in_terrain(column) || within(center, column, settings_.near_distance));
}

bool Region::column_loaded(ColumnPos column) const noexcept {
    if (!in_world(column, settings_.level)) {
        return false;
    }
    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const ColumnPos near = offset(column, dx, dz);
            if (in_world(near, settings_.level) && column_drawn(near)) {
                return true;
            }
        }
    }
    return false;
}

bool Region::column_planned(ColumnPos column) const noexcept {
    if (!in_world(column, settings_.level)) {
        return false;
    }
    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const ColumnPos near = offset(column, dx, dz);
            if (in_world(near, settings_.level) && column_loaded(near)) {
                return true;
            }
        }
    }
    return false;
}

ColumnRuns Region::drawn(ColumnPos column, ChunkRange band) const noexcept {
    ColumnRuns runs;
    if (!in_world(column, settings_.level)) {
        return runs;
    }
    const ColumnPos center{.x = camera_.x, .z = camera_.z};
    if (in_terrain(column)) {
        runs.add(world_range(band.min_y, band.max_y, settings_.level));
    }
    if (within(center, column, settings_.near_distance)) {
        runs.add(world_range(std::int64_t{camera_.y} - settings_.near_height,
                             std::int64_t{camera_.y} + settings_.near_height, settings_.level));
    }
    return runs;
}

ColumnRuns Region::loaded(ColumnPos column, std::span<const ChunkRange, 9> bands) const noexcept {
    ColumnRuns runs;
    if (!in_world(column, settings_.level)) {
        return runs;
    }
    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const ColumnPos near = offset(column, dx, dz);
            if (!in_world(near, settings_.level)) {
                continue;
            }
            const std::size_t at =
                (static_cast<std::size_t>(dz + 1) * 3) + static_cast<std::size_t>(dx + 1);
            const ColumnRuns near_runs = drawn(near, bands[at]);
            for (const ChunkRange& run : near_runs.runs()) {
                runs.add(widened(run, 1, settings_.level));
            }
        }
    }
    return runs;
}

ColumnRuns Region::planned(ColumnPos column, std::span<const ChunkRange, 25> bands) const noexcept {
    ColumnRuns runs;
    if (!in_world(column, settings_.level)) {
        return runs;
    }
    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const ColumnPos near = offset(column, dx, dz);
            if (!in_world(near, settings_.level)) {
                continue;
            }
            // The 3 by 3 bands around the neighbor, out of the 5 by 5.
            std::array<ChunkRange, 9> around{};
            for (std::int32_t bz = -1; bz <= 1; ++bz) {
                for (std::int32_t bx = -1; bx <= 1; ++bx) {
                    const std::size_t to =
                        (static_cast<std::size_t>(bz + 1) * 3) + static_cast<std::size_t>(bx + 1);
                    const std::size_t from = (static_cast<std::size_t>(dz + bz + 2) * 5) +
                                             static_cast<std::size_t>(dx + bx + 2);
                    around[to] = bands[from];
                }
            }
            const ColumnRuns near_runs = loaded(near, around);
            for (const ChunkRange& run : near_runs.runs()) {
                runs.add(widened(run, 1, settings_.level));
            }
        }
    }
    return runs;
}

} // namespace vx
