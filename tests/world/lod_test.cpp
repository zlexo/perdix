// RFC 0011's levels: chunks of any level on grids fixed in the world, each
// holding exactly the 8 chunks of the level below.

#include <vx/world/coordinates.hpp>
#include <vx/world/lod.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace {

using vx::BlockPos;
using vx::LodBox;
using vx::LodPos;

constexpr std::int32_t lowest = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t highest = std::numeric_limits<std::int32_t>::max();

// Blocks across the whole 32-bit world: the edges, around the origin, and
// 2,000 anywhere.
std::vector<BlockPos> sample_blocks() {
    std::vector<BlockPos> blocks{{.x = 0, .y = 0, .z = 0},
                                 {.x = -1, .y = -1, .z = -1},
                                 {.x = 31, .y = 32, .z = 33},
                                 {.x = -32, .y = -33, .z = -1'024},
                                 {.x = lowest, .y = highest, .z = lowest},
                                 {.x = highest, .y = lowest, .z = highest},
                                 {.x = lowest + 1, .y = highest - 1, .z = 1'023}};
    std::mt19937 random(0x10D);
    std::uniform_int_distribution<std::int32_t> anywhere(lowest, highest);
    for (int i = 0; i < 2'000; ++i) {
        blocks.push_back({.x = anywhere(random), .y = anywhere(random), .z = anywhere(random)});
    }
    return blocks;
}

bool holds(const LodBox& box, BlockPos block) {
    return box.min_x <= block.x && block.x < box.max_x && box.min_y <= block.y &&
           block.y < box.max_y && box.min_z <= block.z && block.z < box.max_z;
}

std::int64_t volume(const LodBox& box) {
    return (box.max_x - box.min_x) * (box.max_y - box.min_y) * (box.max_z - box.min_z);
}

bool overlap(const LodBox& a, const LodBox& b) {
    return a.min_x < b.max_x && b.min_x < a.max_x && a.min_y < b.max_y && b.min_y < a.max_y &&
           a.min_z < b.max_z && b.min_z < a.max_z;
}

} // namespace

TEST_CASE("a chunk of each level holds its block and is the parent of the finer one that does") {
    for (const BlockPos block : sample_blocks()) {
        for (std::uint8_t level = 0; level <= vx::max_lod_level; ++level) {
            const LodPos pos = vx::lod_of(block, level);
            CHECK(pos.level == level);
            CHECK(holds(vx::box_of(pos), block));
            if (level < vx::max_lod_level) {
                CHECK(vx::parent(pos) == vx::lod_of(block, static_cast<std::uint8_t>(level + 1)));
            }
        }
    }
    // Level 0 is RFC 0002's chunk; a level-1 chunk spans 64 blocks, floored
    // below zero as above it.
    CHECK(vx::lod_of({.x = -1, .y = 32, .z = 31}, 0) ==
          LodPos{.x = -1, .y = 1, .z = 0, .level = 0});
    CHECK(vx::lod_of({.x = 63, .y = 64, .z = -64}, 1) ==
          LodPos{.x = 0, .y = 1, .z = -1, .level = 1});
    CHECK(vx::lod_of({.x = -65, .y = 0, .z = 0}, 1).x == -2);
    CHECK(vx::cell_blocks(0) == 1);
    CHECK(vx::cell_blocks(vx::max_lod_level) == 32);
    CHECK(vx::box_of({.x = 1, .y = -1, .z = 0, .level = 2}) ==
          LodBox{.min_x = 128, .min_y = -128, .min_z = 0, .max_x = 256, .max_y = 0, .max_z = 128});
}

TEST_CASE("a chunk's 8 children tile it exactly, x fastest, then z, then y") {
    for (const BlockPos block : sample_blocks()) {
        for (std::uint8_t level = 1; level <= vx::max_lod_level; ++level) {
            const LodPos pos = vx::lod_of(block, level);
            const LodBox box = vx::box_of(pos);
            const std::array<LodPos, 8> kids = vx::children(pos);
            std::int64_t covered = 0;
            for (std::size_t i = 0; i < kids.size(); ++i) {
                const LodBox kid = vx::box_of(kids[i]);
                CHECK(kids[i].level == level - 1);
                CHECK(vx::parent(kids[i]) == pos);
                CHECK(kid.min_x >= box.min_x);
                CHECK(kid.max_x <= box.max_x);
                CHECK(kid.min_y >= box.min_y);
                CHECK(kid.max_y <= box.max_y);
                CHECK(kid.min_z >= box.min_z);
                CHECK(kid.max_z <= box.max_z);
                for (std::size_t j = 0; j < i; ++j) {
                    CHECK_FALSE(overlap(kid, vx::box_of(kids[j])));
                }
                covered += volume(kid);
            }
            CHECK(covered == volume(box));
            // The order: x fastest, then z, then y.
            CHECK(kids[1].x == kids[0].x + 1);
            CHECK(kids[2].z == kids[0].z + 1);
            CHECK(kids[4].y == kids[0].y + 1);
            CHECK(kids[0].x == static_cast<std::int32_t>(static_cast<std::uint32_t>(pos.x) << 1U));
        }
    }
}

TEST_CASE("chunks at the edges of the 32-bit world end at 2^31, in 64 bits") {
    constexpr std::int64_t edge = std::int64_t{1} << 31;
    for (std::uint8_t level = 0; level <= vx::max_lod_level; ++level) {
        const LodBox top =
            vx::box_of(vx::lod_of({.x = highest, .y = highest, .z = highest}, level));
        const LodBox bottom =
            vx::box_of(vx::lod_of({.x = lowest, .y = lowest, .z = lowest}, level));
        CHECK(top.max_x == edge);
        CHECK(top.max_y == edge);
        CHECK(top.max_z == edge);
        CHECK(bottom.min_x == -edge);
        CHECK(bottom.min_y == -edge);
        CHECK(bottom.min_z == -edge);
    }
}
