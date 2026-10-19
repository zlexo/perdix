#include <vx/world/lod.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace vx {
namespace {

// The camera's chunk, at level 0.
struct EyeChunk {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};

EyeChunk eye_chunk(const Vec3d& eye) noexcept {
    const auto chunk = [](double v) {
        return static_cast<std::int64_t>(std::floor(v)) >> chunk_bits;
    };
    return {.x = chunk(eye.x), .y = chunk(eye.y), .z = chunk(eye.z)};
}

// The level-0 chunks a chunk of any level spans on one axis, inclusive.
struct Span {
    std::int64_t first = 0;
    std::int64_t last = 0;
};

Span span(std::int32_t coordinate, std::uint8_t level) noexcept {
    const std::int64_t first = std::int64_t{coordinate} * (std::int64_t{1} << level);
    return {.first = first, .last = first + (std::int64_t{1} << level) - 1};
}

// Whether a chunk's columns come within `radius` chunks of the camera's
// column, as RFC 0006's region counts it.
bool reaches(const EyeChunk& eye, LodPos chunk, std::int32_t radius) noexcept {
    if (radius < 0) {
        return false;
    }
    const Span xs = span(chunk.x, chunk.level);
    const Span zs = span(chunk.z, chunk.level);
    const std::int64_t dx = std::clamp(eye.x, xs.first, xs.last) - eye.x;
    const std::int64_t dz = std::clamp(eye.z, zs.first, zs.last) - eye.z;
    return (dx * dx) + (dz * dz) <= std::int64_t{radius} * radius;
}

// The distance from the eye to the nearest point of a box, in blocks.
double nearest_distance(const Vec3d& eye, const LodBox& box) noexcept {
    const auto gap = [](double v, std::int64_t min, std::int64_t max) {
        return std::max({static_cast<double>(min) - v, 0.0, v - static_cast<double>(max)});
    };
    const double x = gap(eye.x, box.min_x, box.max_x);
    const double y = gap(eye.y, box.min_y, box.max_y);
    const double z = gap(eye.z, box.min_z, box.max_z);
    return std::sqrt((x * x) + (y * y) + (z * z));
}

// The floor under unit(), above which chunks drawn side by side differ by
// at most one level (RFC 0011's balance). Say a chunk A of level a is drawn
// beside a chunk B of level b <= a - 2. A is drawn, so its nearest point is
// at least radius(a) >= 2^(b+2) units away. B is drawn, so its parent P was
// refined: P's nearest point is under radius(b+1) = 2^(b+1) units away, or
// P reaches into the near cylinder, so its nearest point is at most the
// cylinder's reach, 32 sqrt(near_distance^2 + near_height^2) blocks, away.
// A touches B inside P, so A's nearest point is at most P's diagonal,
// sqrt(3) x 32 x 2^(b+1) blocks, farther than P's. The first case needs a
// unit under sqrt(3) x 32 blocks; the second, worst at b = 0, under (the
// reach + sqrt(3) x 64) / 4. A block above both, neither can happen.
// Whether any of a chunk's blocks are in the 32-bit world.
bool in_world(const LodBox& box) noexcept {
    constexpr std::int64_t edge = std::int64_t{1} << 31;
    return box.max_x > -edge && box.min_x < edge && box.max_y > -edge && box.min_y < edge &&
           box.max_z > -edge && box.min_z < edge;
}

double balance_floor(std::int32_t near_distance, std::int32_t near_height) noexcept {
    constexpr double root3 = std::numbers::sqrt3;
    const auto squares = static_cast<double>((std::int64_t{near_distance} * near_distance) +
                                             (std::int64_t{near_height} * near_height));
    const double reach = chunk_size * std::sqrt(squares);
    return std::max(root3 * chunk_size, (reach + (root3 * 2 * chunk_size)) / 4.0) + 1.0;
}

} // namespace

double LodRings::unit() const noexcept {
    const double from_screen =
        static_cast<double>(framebuffer_height) / (2.0 * pixels_per_cell * std::tan(fov_y / 2.0));
    return std::max(from_screen, balance_floor(near_distance, near_height));
}

double LodRings::radius(std::uint8_t level) const noexcept {
    return level == 0 ? 0.0 : unit() * static_cast<double>(std::int64_t{1} << level);
}

std::uint8_t LodRings::level_at(const Vec3d& eye, LodPos chunk) const noexcept {
    const EyeChunk at = eye_chunk(eye);
    const Span rows = span(chunk.y, chunk.level);
    if (rows.first <= at.y + near_height && rows.last >= at.y - near_height &&
        reaches(at, chunk, near_distance)) {
        return 0;
    }
    const double distance = nearest_distance(eye, box_of(chunk));
    const double base = unit();
    std::uint8_t level = 0;
    while (level < max_lod_level &&
           distance >= base * static_cast<double>(std::int64_t{1} << (level + 1))) {
        ++level;
    }
    return level;
}

bool LodRings::in_view(const Vec3d& eye, LodPos chunk) const noexcept {
    return reaches(eye_chunk(eye), chunk, view_distance);
}

bool LodRings::drawn(const Vec3d& eye, LodPos chunk) const noexcept {
    if (!in_view(eye, chunk) || level_at(eye, chunk) < chunk.level) {
        return false;
    }
    return chunk.level == max_lod_level || level_at(eye, parent(chunk)) <= chunk.level;
}

std::vector<LodPos> lod_cover(const LodRings& rings, const Vec3d& eye, std::int32_t min_y,
                              std::int32_t max_y) {
    std::vector<LodPos> drawn;
    if (min_y > max_y) {
        return drawn;
    }
    // The top level's chunks over the disc's square and the rows, then down
    // through every chunk whose place calls for a finer level.
    const EyeChunk at = eye_chunk(eye);
    const std::int64_t reach = std::max(rings.view_distance, 0);
    const auto top = [](std::int64_t chunk) {
        return static_cast<std::int32_t>(chunk >> max_lod_level);
    };
    std::vector<LodPos> pending;
    for (std::int32_t y = top(min_y); y <= top(max_y); ++y) {
        for (std::int32_t z = top(at.z - reach); z <= top(at.z + reach); ++z) {
            for (std::int32_t x = top(at.x - reach); x <= top(at.x + reach); ++x) {
                pending.push_back({.x = x, .y = y, .z = z, .level = max_lod_level});
            }
        }
    }
    while (!pending.empty()) {
        const LodPos chunk = pending.back();
        pending.pop_back();
        const Span rows = span(chunk.y, chunk.level);
        if (rows.last < min_y || rows.first > max_y || !in_world(box_of(chunk)) ||
            !rings.in_view(eye, chunk)) {
            continue;
        }
        if (chunk.level == 0 || rings.level_at(eye, chunk) >= chunk.level) {
            drawn.push_back(chunk);
            continue;
        }
        for (const LodPos child : children(chunk)) {
            pending.push_back(child);
        }
    }
    return drawn;
}

} // namespace vx
