#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace vx::test {

// A block's box, its lowest corner at (x, y, z).
constexpr Box block_box(double x, double y, double z) {
    return {.min = {.x = x, .y = y, .z = z}, .max = {.x = x + 1.0, .y = y + 1.0, .z = z + 1.0}};
}

// The standing player's box, 0.6 wide and 1.8 tall, its bottom center at
// `feet` (RFC 0010).
constexpr Box player_box(Vec3d feet) {
    return {.min = {.x = feet.x - 0.3, .y = feet.y, .z = feet.z - 0.3},
            .max = {.x = feet.x + 0.3, .y = feet.y + 1.8, .z = feet.z + 0.3}};
}

// A floor whose top is y = 0, from x and z = -2 to 4.
inline std::vector<Box> floor_boxes() {
    std::vector<Box> boxes;
    for (int x = -2; x < 4; ++x) {
        for (int z = -2; z < 4; ++z) {
            boxes.push_back(block_box(x, -1.0, z));
        }
    }
    return boxes;
}

// A box from the floor up to `height` across the block at (x, 0, z): a slab
// is 0.5.
constexpr Box low_box(double x, double z, double height) {
    return {.min = {.x = x, .y = 0.0, .z = z}, .max = {.x = x + 1.0, .y = height, .z = z + 1.0}};
}

// `boxes`, and `more`.
inline std::vector<Box> with(std::vector<Box> boxes, std::initializer_list<Box> more) {
    boxes.insert(boxes.end(), more);
    return boxes;
}

// A world of loose boxes for movement's tests, with no chunks. It remembers
// every region it was asked about.
class BoxWorld final : public BlockBoxes {
public:
    std::vector<Box> boxes;
    mutable std::vector<Box> regions;

    [[nodiscard]] double slipperiness(BlockPos /*block*/) const override { return 0.6; }

    void collect(const Box& region, std::vector<Box>& out) const override {
        regions.push_back(region);
        for (const Box& box : boxes) {
            if (box.max.x >= region.min.x && box.min.x <= region.max.x &&
                box.max.y >= region.min.y && box.min.y <= region.max.y &&
                box.max.z >= region.min.z && box.min.z <= region.max.z) {
                out.push_back(box);
            }
        }
    }
};

// A player at rest on the floor at the origin, as RFC 0010's vectors start:
// on the ground, with gravity's step carried.
inline Player at_rest() {
    Player player;
    player.velocity.y = -0.0784;
    player.on_ground = true;
    return player;
}

// A floor whose top is y = 0, a million blocks out each way, with loose
// boxes on it, for the tick's tests: the floor's blocks are made as a
// region asks for them.
class FlatWorld final : public BlockBoxes {
public:
    // How far out the floor goes, in blocks.
    static constexpr double reach = 1e6;

    std::vector<Box> boxes;
    double floor_slipperiness = 0.6; // the floor's blocks; the loose boxes are 0.6

    [[nodiscard]] double slipperiness(BlockPos block) const override {
        return block.y < 0 ? floor_slipperiness : 0.6;
    }

    void collect(const Box& region, std::vector<Box>& out) const override {
        // The blocks under y = 0 that the region overlaps or touches, as far
        // as the floor goes: clamped as doubles, so no cast overflows.
        for (const double coordinate :
             {region.min.x, region.min.y, region.min.z, region.max.x, region.max.y, region.max.z}) {
            if (!std::isfinite(coordinate)) {
                return;
            }
        }
        const auto first = [](double low) {
            return static_cast<int>(std::clamp(std::ceil(low - 1.0), -reach, reach));
        };
        const auto last = [](double high) {
            return static_cast<int>(std::clamp(std::floor(high), -reach, reach));
        };
        for (int y = first(region.min.y); y <= std::min(last(region.max.y), -1); ++y) {
            for (int z = first(region.min.z); z <= last(region.max.z); ++z) {
                for (int x = first(region.min.x); x <= last(region.max.x); ++x) {
                    out.push_back(block_box(x, y, z));
                }
            }
        }
        for (const Box& box : boxes) {
            if (box.max.x >= region.min.x && box.min.x <= region.max.x &&
                box.max.y >= region.min.y && box.min.y <= region.max.y &&
                box.max.z >= region.min.z && box.min.z <= region.max.z) {
                out.push_back(box);
            }
        }
    }
};

} // namespace vx::test
