#include <vx/world/streamed_boxes.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/block_physics.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace vx {
namespace {

// The chunks along one axis whose blocks [low, high] overlaps or touches.
struct Chunks {
    std::int32_t first = 0;
    std::int32_t last = -1;
};

Chunks touching(double low, double high) noexcept {
    // A block spans [b, b + 1]: it touches when b <= high and b + 1 >= low.
    const double first_block = std::ceil(low - 1.0);
    const double last_block = std::floor(high);
    return {.first = static_cast<std::int32_t>(std::floor(first_block / chunk_size)),
            .last = static_cast<std::int32_t>(std::floor(last_block / chunk_size))};
}

} // namespace

void StreamedBoxes::collect(const Box& region, std::vector<Box>& out) const {
    for (const double coordinate :
         {region.min.x, region.min.y, region.min.z, region.max.x, region.max.y, region.max.z}) {
        // Far past the world, too: a move there finds nothing.
        if (!std::isfinite(coordinate) || std::abs(coordinate) > 1.0e9) {
            return;
        }
    }
    const Chunks xs = touching(region.min.x, region.max.x);
    const Chunks ys = touching(region.min.y, region.max.y);
    const Chunks zs = touching(region.min.z, region.max.z);
    for (std::int32_t y = ys.first; y <= ys.last; ++y) {
        for (std::int32_t z = zs.first; z <= zs.last; ++z) {
            for (std::int32_t x = xs.first; x <= xs.last; ++x) {
                const ChunkPos pos{.x = x, .y = y, .z = z};
                const ChunkRecord* record = streamer_->find(pos);
                if (record == nullptr || !counts_as_ready(record->state)) {
                    const BlockPos low = block_at(pos, {});
                    const Vec3d corner{.x = static_cast<double>(low.x),
                                       .y = static_cast<double>(low.y),
                                       .z = static_cast<double>(low.z)};
                    constexpr Vec3d side{.x = chunk_size, .y = chunk_size, .z = chunk_size};
                    out.push_back({.min = corner, .max = corner + side});
                    continue;
                }
                if (record->blocks != nullptr) {
                    ChunkBoxes(record->blocks->value(), pos, physics_).collect(region, out);
                } else {
                    const ChunkStorage uniform = ChunkStorage::uniform(record->uniform);
                    ChunkBoxes(uniform, pos, physics_).collect(region, out);
                }
            }
        }
    }
}

double StreamedBoxes::slipperiness(BlockPos block) const {
    const ChunkPos pos = chunk_of(block);
    const ChunkRecord* record = streamer_->find(pos);
    if (record == nullptr || !counts_as_ready(record->state)) {
        return BlockPhysics{}.slipperiness;
    }
    if (record->blocks != nullptr) {
        return ChunkBoxes(record->blocks->value(), pos, physics_).slipperiness(block);
    }
    const ChunkStorage uniform = ChunkStorage::uniform(record->uniform);
    return ChunkBoxes(uniform, pos, physics_).slipperiness(block);
}

} // namespace vx
