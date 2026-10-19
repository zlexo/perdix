// RFC 0011's levels: chunks of any level on grids fixed in the world, each
// holding exactly the 8 chunks of the level below.

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/lod.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace {

using vx::BlockPos;
using vx::LodBox;
using vx::LodPos;
using vx::LodRings;
using vx::Vec3d;

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

// The distance from the eye to a box's nearest point, worked out apart.
double distance_to(const Vec3d& eye, const LodBox& box) {
    const auto gap = [](double v, std::int64_t min, std::int64_t max) {
        return std::max({static_cast<double>(min) - v, 0.0, v - static_cast<double>(max)});
    };
    const double x = gap(eye.x, box.min_x, box.max_x);
    const double y = gap(eye.y, box.min_y, box.max_y);
    const double z = gap(eye.z, box.min_z, box.max_z);
    return std::sqrt((x * x) + (y * y) + (z * z));
}

LodRings with(std::uint32_t height, double k) {
    LodRings rings;
    rings.framebuffer_height = height;
    rings.pixels_per_cell = k;
    return rings;
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

TEST_CASE("level L starts where a cell of 2^L blocks spans k pixels") {
    const LodRings rings; // 1,440 rows, 70 degrees, k = 4
    const double unit = 1'440.0 / (2.0 * 4.0 * std::tan(1.2217304763960306 / 2.0));
    CHECK(rings.unit() == doctest::Approx(unit));
    CHECK(rings.radius(0) == 0.0);
    for (std::uint8_t level = 1; level <= vx::max_lod_level; ++level) {
        CHECK(rings.radius(level) == doctest::Approx(unit * static_cast<double>(1 << level)));
    }
    // RFC 0011's table: full detail to about 1,028, 514 and 257 blocks at
    // k = 2, 4 and 8, and a window half as tall draws coarser as near.
    CHECK(with(1'440, 2.0).radius(1) == doctest::Approx(1'028.3).epsilon(0.001));
    CHECK(rings.radius(1) == doctest::Approx(514.1).epsilon(0.001));
    CHECK(with(1'440, 8.0).radius(1) == doctest::Approx(257.1).epsilon(0.001));
    CHECK(with(720, 4.0).radius(1) == doctest::Approx(257.1).epsilon(0.001));
}

TEST_CASE("a small window's rings stop at the floor that keeps neighbors within a level") {
    // 360 rows at k = 4 would start level 1 at 129 blocks; the near
    // cylinder's reach sets the floor higher.
    LodRings small = with(360, 4.0);
    const double reach = 32.0 * std::sqrt((8.0 * 8.0) + (8.0 * 8.0));
    CHECK(small.unit() == doctest::Approx(((reach + (std::sqrt(3.0) * 64.0)) / 4.0) + 1.0));
    // Without a near cylinder, a chunk's diagonal sets it: k = 8 would
    // start level 1 at 64 blocks.
    LodRings bare = with(360, 8.0);
    bare.near_distance = 0;
    bare.near_height = 0;
    CHECK(bare.unit() == doctest::Approx((std::sqrt(3.0) * 32.0) + 1.0));
}

TEST_CASE("outside the near cylinder the level never falls with distance, and a child's never "
          "falls below its parent's") {
    std::mt19937 random(0x81);
    std::uniform_real_distribution<double> across(-1.0e6, 1.0e6);
    std::uniform_real_distribution<double> height(-100.0, 3'000.0);
    std::uniform_int_distribution<std::int32_t> columns(-520, 520);
    std::uniform_int_distribution<std::int32_t> rows(-100, 100);
    for (const LodRings& rings : {with(1'440, 4.0), with(1'440, 2.0), with(360, 8.0)}) {
        for (int trial = 0; trial < 12; ++trial) {
            const Vec3d eye{.x = across(random), .y = height(random), .z = across(random)};
            const LodPos at = vx::lod_of({.x = static_cast<std::int32_t>(std::floor(eye.x)),
                                          .y = static_cast<std::int32_t>(std::floor(eye.y)),
                                          .z = static_cast<std::int32_t>(std::floor(eye.z))},
                                         0);
            std::vector<std::pair<double, std::uint8_t>> seen;
            for (int i = 0; i < 400; ++i) {
                const LodPos chunk{.x = at.x + columns(random),
                                   .y = at.y + rows(random),
                                   .z = at.z + columns(random),
                                   .level = 0};
                const std::int64_t dx = chunk.x - at.x;
                const std::int64_t dz = chunk.z - at.z;
                const bool near = (dx * dx) + (dz * dz) <= 64 && std::abs(chunk.y - at.y) <= 8;
                if (!near) {
                    seen.emplace_back(distance_to(eye, vx::box_of(chunk)),
                                      rings.level_at(eye, chunk));
                }
                for (std::uint8_t level = 1; level <= vx::max_lod_level; ++level) {
                    const LodPos coarse = vx::lod_of({.x = chunk.x << vx::chunk_bits,
                                                      .y = chunk.y << vx::chunk_bits,
                                                      .z = chunk.z << vx::chunk_bits},
                                                     level);
                    const std::uint8_t mine = rings.level_at(eye, coarse);
                    for (const LodPos child : vx::children(coarse)) {
                        CHECK(rings.level_at(eye, child) >= mine);
                    }
                }
            }
            std::sort(seen.begin(), seen.end());
            for (std::size_t i = 1; i < seen.size(); ++i) {
                CHECK(seen[i].second >= seen[i - 1].second);
            }
        }
    }
}

TEST_CASE("the near cylinder is drawn at level 0, whatever the distance says") {
    // In a small window level 1 starts inside the cylinder's reach.
    const LodRings small = with(360, 4.0);
    const Vec3d eye{.x = 16.0, .y = 70.0, .z = 16.0}; // in chunk (0, 2, 0)
    for (std::int32_t dx = -8; dx <= 8; ++dx) {
        for (std::int32_t dz = -8; dz <= 8; ++dz) {
            if ((dx * dx) + (dz * dz) > 64) {
                continue;
            }
            for (std::int32_t dy = -8; dy <= 8; ++dy) {
                const LodPos chunk{.x = dx, .y = 2 + dy, .z = dz, .level = 0};
                CHECK(small.level_at(eye, chunk) == 0);
                CHECK(small.drawn(eye, chunk));
            }
        }
    }
    // A corner of it is 337.9 blocks away, past level 1's start: the
    // cylinder decides. A row above it, the distance does.
    const LodPos corner{.x = 5, .y = 10, .z = 6, .level = 0};
    CHECK(distance_to(eye, vx::box_of(corner)) > small.radius(1));
    CHECK(small.level_at(eye, corner) == 0);
    CHECK(small.level_at(eye, {.x = 5, .y = 11, .z = 6, .level = 0}) == 1);
}

TEST_CASE("a chunk is drawn where its place calls for its level and its parent's for a finer one") {
    const LodRings rings; // level 1 from 514 blocks, 3 from 2,056
    const Vec3d eye{.x = 0.5, .y = 100.0, .z = 0.5};
    const LodPos near = vx::lod_of({.x = 0, .y = 96, .z = 0}, 0);
    CHECK(rings.drawn(eye, near));
    CHECK_FALSE(rings.drawn(eye, vx::parent(near)));
    // At about 3,000 blocks east, level 3: not its parent, not its children.
    const LodPos far = vx::lod_of({.x = 3'000, .y = 96, .z = 0}, 3);
    CHECK(rings.level_at(eye, far) == 3);
    CHECK(rings.drawn(eye, far));
    CHECK_FALSE(rings.drawn(eye, vx::parent(far)));
    CHECK_FALSE(rings.drawn(eye, vx::children(far)[0]));
    // Past the view distance nothing is drawn.
    LodRings close = rings;
    close.view_distance = 16;
    CHECK_FALSE(close.drawn(eye, far));
    CHECK(close.drawn(eye, near));
}

TEST_CASE("levels hold at the far edges of the world") {
    const LodRings rings;
    const Vec3d eye{.x = 2'100'000'000.5, .y = 64.0, .z = -2'100'000'000.5};
    const LodPos here = vx::lod_of({.x = 2'100'000'000, .y = 64, .z = -2'100'000'001}, 0);
    CHECK(rings.level_at(eye, here) == 0);
    CHECK(rings.drawn(eye, here));
    const LodPos east = vx::lod_of({.x = 2'100'016'000, .y = 64, .z = -2'100'000'001}, 5);
    CHECK(rings.level_at(eye, east) == 5);
    CHECK(rings.drawn(eye, east));
}
