#include <vx/world/backlog.hpp>

#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace vx {
namespace {

// Squares that cannot overflow: a distance past 2^15 chunks keys the same.
std::uint32_t squared(std::int64_t d) noexcept {
    const std::int64_t clamped = std::min<std::int64_t>(std::abs(d), 1 << 15);
    return static_cast<std::uint32_t>(clamped * clamped);
}

std::uint32_t saturated(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

} // namespace

std::uint32_t chunk_score(ChunkPos chunk, ChunkPos camera, Vec3d eye, const Frustum& view,
                          std::uint8_t level) noexcept {
    const std::int64_t dx = std::int64_t{chunk.x} - camera.x;
    const std::int64_t dy = std::int64_t{chunk.y} - camera.y;
    const std::int64_t dz = std::int64_t{chunk.z} - camera.z;
    if (std::abs(dx) <= 1 && std::abs(dy) <= 1 && std::abs(dz) <= 1) {
        return 0;
    }
    const std::uint64_t distance =
        std::uint64_t{squared(dx)} + std::uint64_t{squared(dy)} + std::uint64_t{squared(dz)};
    // The chunk's box grown by a chunk of its level, relative to the eye, in
    // doubles: a coarse chunk's corner can be 2^31 blocks out.
    const double size = std::ldexp(static_cast<double>(chunk_size), level);
    const Vec3d min{.x = ((static_cast<double>(chunk.x) - 1.0) * size) - eye.x,
                    .y = ((static_cast<double>(chunk.y) - 1.0) * size) - eye.y,
                    .z = ((static_cast<double>(chunk.z) - 1.0) * size) - eye.z};
    const double side = 3.0 * size;
    const Vec3d max{.x = min.x + side, .y = min.y + side, .z = min.z + side};
    // Doubled outside the view: four times the square.
    return saturated(view.may_contain(min, max) ? distance : 4 * distance);
}

std::uint32_t column_score(std::int32_t x, std::int32_t z, ChunkPos camera) noexcept {
    return saturated(std::uint64_t{squared(std::int64_t{x} - camera.x)} +
                     std::uint64_t{squared(std::int64_t{z} - camera.z)});
}

} // namespace vx
