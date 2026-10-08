#include <vx/world/chunk_boxes.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace vx {
namespace {

// The chunk's blocks along one axis that [low, high] overlaps or touches, as
// local coordinates from 0 to 31: a block spans [b, b + 1], so it touches
// when b <= high and b + 1 >= low. None when first > last. Clamped as
// doubles, so a region far from the chunk never overflows an int.
struct Span {
    int first = 0;
    int last = -1;
};

Span touching(double low, double high, std::int32_t origin) noexcept {
    const auto start = static_cast<double>(origin);
    constexpr double top = chunk_size - 1;
    const double first = std::clamp(std::ceil(low - 1.0 - start), 0.0, top + 1.0);
    const double last = std::clamp(std::floor(high - start), -1.0, top);
    return {.first = static_cast<int>(first), .last = static_cast<int>(last)};
}

} // namespace

ChunkBoxes::ChunkBoxes(const ChunkStorage& chunk, ChunkPos position) noexcept
    : chunk_(&chunk), origin_(block_at(position, {})) {}

void ChunkBoxes::collect(const Box& region, std::vector<Box>& out) const {
    for (const double coordinate :
         {region.min.x, region.min.y, region.min.z, region.max.x, region.max.y, region.max.z}) {
        if (!std::isfinite(coordinate)) {
            return;
        }
    }
    const Span xs = touching(region.min.x, region.max.x, origin_.x);
    const Span ys = touching(region.min.y, region.max.y, origin_.y);
    const Span zs = touching(region.min.z, region.max.z, origin_.z);
    for (int y = ys.first; y <= ys.last; ++y) {
        for (int z = zs.first; z <= zs.last; ++z) {
            for (int x = xs.first; x <= xs.last; ++x) {
                if (chunk_->get(local_index(x, y, z)) == BlockState::air) {
                    continue;
                }
                const Vec3d low{.x = static_cast<double>(origin_.x + x),
                                .y = static_cast<double>(origin_.y + y),
                                .z = static_cast<double>(origin_.z + z)};
                out.push_back({.min = low, .max = low + Vec3d{.x = 1.0, .y = 1.0, .z = 1.0}});
            }
        }
    }
}

} // namespace vx
