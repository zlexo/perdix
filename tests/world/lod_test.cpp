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
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <unordered_set>
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

// The distance from the eye to the nearest point of a box's column, across.
double across_to(const Vec3d& eye, const LodBox& box) {
    const auto gap = [](double v, std::int64_t min, std::int64_t max) {
        return std::max({static_cast<double>(min) - v, 0.0, v - static_cast<double>(max)});
    };
    const double x = gap(eye.x, box.min_x, box.max_x);
    const double z = gap(eye.z, box.min_z, box.max_z);
    return std::sqrt((x * x) + (z * z));
}

LodRings with(std::uint32_t height, double k) {
    LodRings rings;
    rings.framebuffer_height = height;
    rings.pixels_per_cell = k;
    return rings;
}

// Chunks in a hash set.
struct LodHash {
    std::size_t operator()(const LodPos& pos) const noexcept {
        std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
        for (const std::uint64_t part :
             {std::uint64_t{static_cast<std::uint32_t>(pos.x)},
              std::uint64_t{static_cast<std::uint32_t>(pos.y)},
              std::uint64_t{static_cast<std::uint32_t>(pos.z)}, std::uint64_t{pos.level}}) {
            hash = (hash ^ part) * 0x100'0000'01B3ULL;
        }
        return static_cast<std::size_t>(hash);
    }
};
using LodSet = std::unordered_set<LodPos, LodHash>;

// The drawn chunk over `pos`, itself or an ancestor; none when finer chunks
// cover it.
std::optional<LodPos> drawn_over(const LodSet& drawn, LodPos pos) {
    while (!drawn.contains(pos)) {
        if (pos.level == vx::max_lod_level) {
            return std::nullopt;
        }
        pos = vx::parent(pos);
    }
    return pos;
}

bool in_world(LodPos pos) {
    constexpr std::int64_t edge = std::int64_t{1} << 31;
    const LodBox box = vx::box_of(pos);
    return box.max_x > -edge && box.min_x < edge && box.max_y > -edge && box.min_y < edge &&
           box.max_z > -edge && box.min_z < edge;
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

TEST_CASE("outside the near cylinder the level never falls with distance across, and a child's "
          "never falls below its parent's") {
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
                // Levels go by column: the height counts for nothing.
                const bool near = (dx * dx) + (dz * dz) <= 64;
                if (!near) {
                    seen.emplace_back(across_to(eye, vx::box_of(chunk)),
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

TEST_CASE("the near cylinder's columns are drawn at level 0 at every height, whatever the "
          "distance says") {
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
    // cylinder decides.
    const LodPos corner{.x = 5, .y = 10, .z = 6, .level = 0};
    CHECK(distance_to(eye, vx::box_of(corner)) > small.radius(1));
    CHECK(small.level_at(eye, corner) == 0);
    // Its edge columns lie past level 1's start even across, and a column
    // changes level whole: rows far above and below are level 0 too.
    for (const std::int32_t x : {-8, 8}) {
        CHECK(across_to(eye, vx::box_of({.x = x, .y = 2, .z = 0, .level = 0})) > small.radius(1));
        for (const std::int32_t y : {-300, 2, 11, 300}) {
            CHECK(small.level_at(eye, {.x = x, .y = y, .z = 0, .level = 0}) == 0);
        }
    }
    // A column just past the cylinder, past level 1's start across, is
    // level 1 at every height, the camera's own included.
    const LodPos past{.x = 9, .y = 2, .z = 0, .level = 0};
    CHECK(across_to(eye, vx::box_of(past)) > small.radius(1));
    CHECK(small.level_at(eye, past) == 1);
    CHECK(small.level_at(eye, {.x = 9, .y = -40, .z = 0, .level = 0}) == 1);
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

TEST_CASE("the chunks drawn cover the view once, each within a level of its neighbors") {
    std::mt19937 random(0xC0);
    std::uniform_real_distribution<double> across(-5.0e5, 5.0e5);
    std::uniform_real_distribution<double> height(-200.0, 3'000.0);
    std::size_t pairs = 0;
    const std::array<std::pair<LodRings, std::int32_t>, 5> cases{{{with(1'440, 4.0), 64},
                                                                  {with(1'440, 4.0), 512},
                                                                  {with(1'440, 2.0), 64},
                                                                  {with(360, 8.0), 64},
                                                                  {with(360, 8.0), 512}}};
    for (const auto& [base, distance] : cases) {
        LodRings rings = base;
        rings.view_distance = distance;
        for (int trial = 0; trial < 2; ++trial) {
            const Vec3d eye{.x = across(random), .y = height(random), .z = across(random)};
            const LodPos at = vx::lod_of({.x = static_cast<std::int32_t>(std::floor(eye.x)),
                                          .y = static_cast<std::int32_t>(std::floor(eye.y)),
                                          .z = static_cast<std::int32_t>(std::floor(eye.z))},
                                         0);
            const std::int32_t min_y = at.y - 8;
            const std::int32_t max_y = at.y + 8;
            const std::vector<LodPos> cover = vx::lod_cover(rings, eye, min_y, max_y);
            const LodSet drawn(cover.begin(), cover.end());
            REQUIRE(drawn.size() == cover.size());
            std::size_t not_drawn = 0;
            std::size_t under_coarser = 0;
            std::size_t unbalanced = 0;
            for (const LodPos chunk : cover) {
                not_drawn += rings.drawn(eye, chunk) ? 0U : 1U;
                if (chunk.level < vx::max_lod_level &&
                    drawn_over(drawn, vx::parent(chunk)).has_value()) {
                    ++under_coarser;
                }
                // Each face's neighbor at the same level lies under a drawn
                // chunk at most a level coarser, or finer ones cover it,
                // which check this one from their side.
                for (const std::array<std::int32_t, 3> step :
                     {std::array{1, 0, 0}, std::array{-1, 0, 0}, std::array{0, 1, 0},
                      std::array{0, -1, 0}, std::array{0, 0, 1}, std::array{0, 0, -1}}) {
                    const LodPos next{.x = chunk.x + step[0],
                                      .y = chunk.y + step[1],
                                      .z = chunk.z + step[2],
                                      .level = chunk.level};
                    const LodBox box = vx::box_of(next);
                    if (box.max_y <= std::int64_t{min_y} * 32 ||
                        box.min_y > std::int64_t{max_y} * 32 || !in_world(next) ||
                        !rings.in_view(eye, next)) {
                        continue;
                    }
                    if (const std::optional<LodPos> over = drawn_over(drawn, next)) {
                        unbalanced += over->level > chunk.level + 1 ? 1U : 0U;
                        ++pairs;
                    }
                }
            }
            CHECK(not_drawn == 0);
            CHECK(under_coarser == 0);
            CHECK(unbalanced == 0);
            // Every level-0 chunk of the disc's columns and the rows is under
            // exactly one drawn chunk; the near cylinder's are drawn
            // themselves.
            std::uniform_int_distribution<std::int32_t> columns(-distance, distance);
            std::uniform_int_distribution<std::int32_t> rows(min_y, max_y);
            std::size_t uncovered = 0;
            std::size_t near_coarse = 0;
            for (int i = 0; i < 1'000; ++i) {
                const std::int32_t dx = columns(random);
                const std::int32_t dz = columns(random);
                if ((std::int64_t{dx} * dx) + (std::int64_t{dz} * dz) >
                    std::int64_t{distance} * distance) {
                    continue;
                }
                LodPos pos{.x = at.x + dx, .y = rows(random), .z = at.z + dz, .level = 0};
                const bool near = (dx * dx) + (dz * dz) <= 64 && std::abs(pos.y - at.y) <= 8;
                near_coarse += near && !drawn.contains(pos) ? 1U : 0U;
                int over = 0;
                for (;;) {
                    over += drawn.contains(pos) ? 1 : 0;
                    if (pos.level == vx::max_lod_level) {
                        break;
                    }
                    pos = vx::parent(pos);
                }
                uncovered += over == 1 ? 0 : 1;
            }
            CHECK(uncovered == 0);
            CHECK(near_coarse == 0);
        }
    }
    CHECK(pairs > 10'000);
}

TEST_CASE("each level streams a disc that holds every chunk of the level drawn, for any eye") {
    std::mt19937 random(0x57D);
    std::uniform_real_distribution<double> across(-5.0e5, 5.0e5);
    std::uniform_real_distribution<double> ground(-100.0, 200.0);
    std::uniform_real_distribution<double> height(-200.0, 4'000.0);
    std::uniform_int_distribution<std::int32_t> near(0, 12);
    std::uniform_real_distribution<double> fov(0.8, 1.9);
    // Chunks drawn at each level, and the most any reached of its level's
    // disc, as a share of the radius.
    std::array<std::size_t, vx::lod_levels> checked{};
    std::array<double, vx::lod_levels> reach{};
    std::size_t outside = 0;
    const std::array<std::pair<LodRings, std::int32_t>, 6> cases{{{with(1'440, 4.0), 48},
                                                                  {with(1'440, 4.0), 512},
                                                                  {with(2'160, 2.0), 96},
                                                                  {with(480, 8.0), 48},
                                                                  {with(360, 8.0), 512},
                                                                  {with(1'080, 4.0), 160}}};
    for (const auto& [base, distance] : cases) {
        for (int trial = 0; trial < 6; ++trial) {
            LodRings rings = base;
            rings.view_distance = distance;
            rings.near_distance = near(random);
            rings.near_height = near(random);
            rings.fov_y = fov(random);
            // Half on the ground, where the near cylinder meets its rows.
            const double y = trial % 2 == 0 ? ground(random) : height(random);
            const Vec3d eye{.x = across(random), .y = y, .z = across(random)};
            const LodPos at = vx::lod_of({.x = static_cast<std::int32_t>(std::floor(eye.x)),
                                          .y = static_cast<std::int32_t>(std::floor(eye.y)),
                                          .z = static_cast<std::int32_t>(std::floor(eye.z))},
                                         0);
            // The ground's rows, from a camera on it or high above.
            for (const LodPos chunk : vx::lod_cover(rings, eye, -3, 5)) {
                const std::int64_t dx = std::int64_t{chunk.x} - (at.x >> chunk.level);
                const std::int64_t dz = std::int64_t{chunk.z} - (at.z >> chunk.level);
                const std::int64_t radius = rings.stream_distance(chunk.level);
                outside += (dx * dx) + (dz * dz) <= radius * radius ? 0 : 1;
                ++checked.at(chunk.level);
                reach.at(chunk.level) = std::max(
                    reach.at(chunk.level), std::sqrt(static_cast<double>((dx * dx) + (dz * dz))) /
                                               static_cast<double>(radius));
            }
        }
    }
    CHECK(outside == 0);
    for (std::uint8_t level = 0; level < vx::lod_levels; ++level) {
        INFO("level " << int{level});
        CHECK(checked.at(level) > 100);
        // Not wasteful: some chunk comes within 2 chunks of the edge.
        CHECK(reach.at(level) > 0.9);
    }
}

TEST_CASE("at the default rings every level streams 19 chunks out, the top level its view") {
    LodRings rings;
    // r_(L+1) / (32 x 2^L) = 1,440 / (16 x 2 x 4 x tan 35 degrees), 16.07,
    // plus 2 sqrt(2) for the parent's corners and the camera's chunk.
    for (std::uint8_t level = 0; level < vx::max_lod_level; ++level) {
        CHECK(rings.stream_distance(level) == 19);
    }
    // 512 / 32 + sqrt(2): the view, at the top.
    CHECK(rings.stream_distance(vx::max_lod_level) == 18);
    // A nearer view: whichever is nearer, the view or the ring.
    rings.view_distance = 32;
    CHECK(rings.stream_distance(0) == 19);
    CHECK(rings.stream_distance(1) == 18);
    CHECK(rings.stream_distance(2) == 10);
    CHECK(rings.stream_distance(5) == 3);
    rings.view_distance = 12;
    CHECK(rings.stream_distance(0) == 12);
    rings.view_distance = -1;
    CHECK(rings.stream_distance(0) == -1);
    CHECK(rings.stream_distance(3) == -1);
    // A small window's floor, under a near cylinder of 10 chunks, 320
    // blocks: the cylinder holds level 0 out.
    LodRings small = with(240, 8.0);
    small.near_distance = 10;
    CHECK(small.radius(1) < 270.0);
    CHECK(small.stream_distance(0) == 13); // 320 / 32 + 2 sqrt(2)
}

TEST_CASE("the chunks drawn stop at the edge of the 32-bit world") {
    const LodRings rings;
    const Vec3d eye{.x = 2'147'483'000.5, .y = 64.0, .z = -2'147'483'000.5};
    const std::vector<LodPos> cover = vx::lod_cover(rings, eye, 0, 4);
    CHECK_FALSE(cover.empty());
    for (const LodPos chunk : cover) {
        CHECK(in_world(chunk));
    }
}

namespace {

using vx::LodReady;

// A world for the swaps: each chunk's mesh arrives at a step of its own, a
// hash of its position and level, with faces or without; a chunk whose
// blocks all lie at 160 or above is outside the terrain, at every level.
struct Arrivals {
    std::uint64_t seed = 1;
    std::int32_t steps = 8;

    [[nodiscard]] std::uint64_t hash(LodPos pos) const {
        return LodHash{}(pos) ^ (seed * 0x9E37'79B9'7F4A'7C15ULL);
    }
    [[nodiscard]] LodReady at(LodPos pos, std::int32_t step) const {
        if (vx::box_of(pos).min_y >= 160) {
            return LodReady::outside;
        }
        const std::uint64_t mixed = hash(pos) * 0xBF58'476D'1CE4'E5B9ULL;
        if (static_cast<std::int32_t>((mixed >> 33U) % static_cast<std::uint64_t>(steps)) > step) {
            return LodReady::waiting;
        }
        return (mixed >> 13U) % 4 == 0 ? LodReady::empty : LodReady::mesh;
    }
};

// Terrain as the swaps see it: in each column of each level one row has
// faces, the one the column's surface lies in, and its mesh arrives at a
// step of its own. A coarse cell is solid when its middle is under the
// ground, so a level's surface lies up to half a cell under the finer one's:
// near the bottom of a row the two lie in different rows, as the
// placeholder's hills do at 64 blocks.
struct Ground {
    std::uint64_t seed = 1;
    std::int32_t steps = 8;

    // The ground's height at a block column, in blocks: 40 to 250.
    [[nodiscard]] static double height(double x, double z) {
        return 145.0 + (60.0 * std::sin(x / 211.0)) + (45.0 * std::cos(z / 157.0));
    }
    // The row of a level's column that its surface lies in.
    [[nodiscard]] static std::int32_t surface(std::int32_t x, std::int32_t z, std::uint8_t level) {
        const double cell = std::ldexp(1.0, level);
        const double ground = height(((x * 32.0) + 16.0) * cell, ((z * 32.0) + 16.0) * cell);
        const double top = std::floor(ground - (cell / 2.0));
        return static_cast<std::int32_t>(std::floor(top / (32.0 * cell)));
    }
    [[nodiscard]] LodReady at(LodPos pos, std::int32_t step) const {
        const std::uint64_t mixed =
            (LodHash{}(pos) ^ (seed * 0x9E37'79B9'7F4A'7C15ULL)) * 0xBF58'476D'1CE4'E5B9ULL;
        if (static_cast<std::int32_t>((mixed >> 33U) % static_cast<std::uint64_t>(steps)) > step) {
            return LodReady::waiting;
        }
        // Once all have come, a level-0 mesh in 8 goes for a step, as an
        // unload takes one: its column's parent, ready by then, stands in.
        const std::int32_t away = steps + 2 + static_cast<std::int32_t>((mixed >> 40U) % 10U);
        if (pos.level == 0 && (mixed >> 7U) % 8 == 0 && step == away) {
            return LodReady::waiting;
        }
        return pos.y == surface(pos.x, pos.z, pos.level) ? LodReady::mesh : LodReady::empty;
    }
    // Whether the ground of level-0 column (x, z) is drawn, at some level.
    [[nodiscard]] static bool drawn(const LodSet& drawn, std::int32_t x, std::int32_t z) {
        for (std::uint8_t level = 0; level < vx::lod_levels; ++level) {
            const std::int32_t cx = x >> level;
            const std::int32_t cz = z >> level;
            if (drawn.contains({.x = cx, .y = surface(cx, cz, level), .z = cz, .level = level})) {
                return true;
            }
        }
        return false;
    }
};

bool under_drawn(const LodSet& drawn, LodPos pos) {
    while (pos.level < vx::max_lod_level) {
        pos = vx::parent(pos);
        if (drawn.contains(pos)) {
            return true;
        }
    }
    return false;
}

bool in_rows(const LodRings& rings, const Vec3d& eye, LodPos pos, std::int32_t min_y,
             std::int32_t max_y) {
    const LodBox box = vx::box_of(pos);
    return box.max_y > std::int64_t{min_y} * 32 && box.min_y < (std::int64_t{max_y} + 1) * 32 &&
           in_world(pos) && rings.in_view(eye, pos);
}

// Whether `pos`, of the region, is drawn: itself, by a chunk around it, or
// through each of its children, where one with nothing to draw counts.
bool covered(const LodSet& drawn, LodPos pos, const std::function<LodReady(LodPos)>& ready,
             const LodRings& rings, const Vec3d& eye, std::int32_t min_y, std::int32_t max_y) {
    if (drawn.contains(pos) || under_drawn(drawn, pos)) {
        return true;
    }
    // A chunk around it with nothing to draw, drawn at its level now,
    // draws it too: there that level shows nothing.
    for (LodPos around = pos; around.level < vx::max_lod_level;) {
        around = vx::parent(around);
        if (ready(around) == LodReady::empty && rings.drawn(eye, around)) {
            return true;
        }
    }
    if (pos.level == 0) {
        return false;
    }
    return std::ranges::all_of(vx::children(pos), [&](LodPos child) {
        if (!in_rows(rings, eye, child, min_y, max_y)) {
            return true;
        }
        const LodReady state = ready(child);
        return state == LodReady::empty || state == LodReady::outside ||
               covered(drawn, child, ready, rings, eye, min_y, max_y);
    });
}

} // namespace

TEST_CASE("with every mesh ready, the swaps draw lod_cover's chunks that have faces") {
    std::mt19937 random(0x5E1);
    std::uniform_real_distribution<double> across(-5.0e5, 5.0e5);
    std::uniform_real_distribution<double> height(-100.0, 2'000.0);
    std::size_t compared = 0;
    for (const auto& [base, distance] : std::array<std::pair<LodRings, std::int32_t>, 3>{
             {{with(1'440, 4.0), 48}, {with(360, 8.0), 64}, {with(1'440, 2.0), 32}}}) {
        for (int trial = 0; trial < 3; ++trial) {
            LodRings rings = base;
            rings.view_distance = distance;
            const Vec3d eye{.x = across(random), .y = height(random), .z = across(random)};
            const std::vector<LodPos> cover = vx::lod_cover(rings, eye, -3, 6);
            const Arrivals arrivals{.seed = static_cast<std::uint64_t>(trial), .steps = 1};
            const std::vector<LodPos> all =
                vx::lod_select(rings, eye, -3, 6, [](LodPos) { return LodReady::mesh; });
            CHECK(LodSet(all.begin(), all.end()) == LodSet(cover.begin(), cover.end()));
            CHECK(all.size() == cover.size());
            // Some empty, some outside: those with faces.
            const auto ready = [&arrivals](LodPos pos) { return arrivals.at(pos, 0); };
            const std::vector<LodPos> some = vx::lod_select(rings, eye, -3, 6, ready);
            LodSet expected;
            for (const LodPos chunk : cover) {
                if (ready(chunk) == LodReady::mesh) {
                    expected.insert(chunk);
                }
            }
            CHECK(LodSet(some.begin(), some.end()) == expected);
            compared += expected.size();
        }
    }
    CHECK(compared > 1'000);
}

TEST_CASE("RFC 0011's swaps: as meshes arrive and the camera wanders, no chunk is drawn with a "
          "chunk around it, and nothing drawn goes until what replaces it can be drawn") {
    // std::mt19937's numbers are the same in every standard library, its
    // distributions are not: the walk is made from the numbers, so that
    // every platform takes the same one.
    std::mt19937 random(0x5A9);
    const auto between = [&random](double low, double high) {
        return low + ((high - low) * static_cast<double>(random()) / 4'294'967'296.0);
    };
    constexpr std::int32_t min_y = -3;
    constexpr std::int32_t max_y = 6;
    std::size_t kept = 0;   // chunks drawn before and still in the region
    std::size_t swaps = 0;  // steps that changed what is drawn
    std::size_t levels = 0; // levels drawn at once, summed over the steps
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        LodRings rings = with(360, 8.0);
        rings.view_distance = 40;
        rings.near_distance = 2;
        rings.near_height = 2;
        const Arrivals arrivals{.seed = seed, .steps = 10};
        Vec3d eye{.x = 1'000.5 * static_cast<double>(seed), .y = 70.0, .z = -333.25};
        std::vector<LodPos> before;
        for (std::int32_t step = 0; step < 24; ++step) {
            const auto move = random() % 10U;
            if (move < 6) {
                eye = eye + Vec3d{.x = between(-48.0, 48.0), .y = 0.0, .z = between(-48.0, 48.0)};
            } else if (move < 8) {
                eye =
                    eye + Vec3d{.x = between(-900.0, 900.0), .y = 0.0, .z = between(-900.0, 900.0)};
            } else {
                eye.y = std::clamp(eye.y + between(-250.0, 250.0), -50.0, 1'500.0);
            }
            const auto ready = [&arrivals, step](LodPos pos) { return arrivals.at(pos, step); };
            const std::vector<LodPos> drawn =
                vx::lod_select(rings, eye, min_y, max_y, ready, before);
            const LodSet now(drawn.begin(), drawn.end());
            REQUIRE(now.size() == drawn.size());
            std::array<bool, vx::lod_levels> seen{};
            for (const LodPos chunk : drawn) {
                CHECK(ready(chunk) == LodReady::mesh);
                CHECK_FALSE(under_drawn(now, chunk));
                seen.at(chunk.level) = true;
            }
            levels += static_cast<std::size_t>(std::ranges::count(seen, true));
            for (const LodPos chunk : before) {
                if (!in_rows(rings, eye, chunk, min_y, max_y)) {
                    continue;
                }
                ++kept;
                INFO("seed " << seed << ", step " << step << ", level " << int{chunk.level});
                CHECK(covered(now, chunk, ready, rings, eye, min_y, max_y));
            }
            swaps += LodSet(before.begin(), before.end()) == now ? 0U : 1U;
            before = drawn;
        }
        // Still, with every mesh in: lod_cover's chunks with faces.
        const auto all_in = [&arrivals](LodPos pos) { return arrivals.at(pos, 10); };
        const std::vector<LodPos> settled =
            vx::lod_select(rings, eye, min_y, max_y, all_in, before);
        LodSet expected;
        for (const LodPos chunk : vx::lod_cover(rings, eye, min_y, max_y)) {
            if (all_in(chunk) == LodReady::mesh) {
                expected.insert(chunk);
            }
        }
        CHECK(LodSet(settled.begin(), settled.end()) == expected);
    }
    CHECK(kept > 2'000);
    CHECK(swaps > 60);
    CHECK(levels > 150); // several levels at once, most steps
}

TEST_CASE("as meshes arrive and the camera wanders, ground drawn stays drawn, though a coarse "
          "level's surface can lie a row below the finer one's") {
    // The flicker this guards: a coarse chunk whose surface has no finer
    // counterpart inside it (its children are all empty: the finer surface
    // is a row up) was replaced at once, while the row above it, empty at
    // the coarse level, still waited for its children. Neither drew the
    // ground there until they came.
    std::mt19937 random(0x6E0);
    std::uniform_real_distribution<double> step_by(-48.0, 48.0);
    std::uniform_real_distribution<double> jump_by(-900.0, 900.0);
    std::uniform_real_distribution<double> rise_by(-250.0, 250.0);
    std::uniform_int_distribution<int> kind(0, 9);
    constexpr std::int32_t min_y = -1;
    constexpr std::int32_t max_y = 9;
    std::size_t kept = 0;     // columns whose ground was drawn and still is in view
    std::size_t crossing = 0; // of them, those whose levels' surfaces lie in different rows
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        LodRings rings = with(360, 8.0);
        rings.view_distance = 40;
        rings.near_distance = 2;
        rings.near_height = 2;
        const Ground ground{.seed = seed, .steps = 10};
        Vec3d eye{.x = 1'000.5 * static_cast<double>(seed), .y = 160.0, .z = -333.25};
        std::vector<LodPos> before;
        std::vector<std::pair<std::int32_t, std::int32_t>> was_drawn;
        for (std::int32_t step = 0; step < 24; ++step) {
            const int move = kind(random);
            if (move < 6) {
                eye = eye + Vec3d{.x = step_by(random), .y = 0.0, .z = step_by(random)};
            } else if (move < 8) {
                eye = eye + Vec3d{.x = jump_by(random), .y = 0.0, .z = jump_by(random)};
            } else {
                eye.y = std::clamp(eye.y + rise_by(random), 0.0, 1'500.0);
            }
            const auto ready = [&ground, step](LodPos pos) { return ground.at(pos, step); };
            const std::vector<LodPos> drawn =
                vx::lod_select(rings, eye, min_y, max_y, ready, before);
            const LodSet now(drawn.begin(), drawn.end());
            for (const auto& [x, z] : was_drawn) {
                const LodPos column{.x = x, .y = 0, .z = z, .level = 0};
                if (!rings.in_view(eye, column)) {
                    continue;
                }
                ++kept;
                crossing +=
                    (Ground::surface(x, z, 0) >> 1) != Ground::surface(x >> 1, z >> 1, 1) ? 1U : 0U;
                INFO("seed " << seed << ", step " << step << ", column " << x << ", " << z);
                CHECK(Ground::drawn(now, x, z));
            }
            was_drawn.clear();
            const auto ex = static_cast<std::int32_t>(std::floor(eye.x)) >> vx::chunk_bits;
            const auto ez = static_cast<std::int32_t>(std::floor(eye.z)) >> vx::chunk_bits;
            for (std::int32_t z = ez - rings.view_distance; z <= ez + rings.view_distance; ++z) {
                for (std::int32_t x = ex - rings.view_distance; x <= ex + rings.view_distance;
                     ++x) {
                    if (Ground::drawn(now, x, z)) {
                        was_drawn.emplace_back(x, z);
                    }
                }
            }
            before = drawn;
        }
    }
    CHECK(kept > 20'000);
    CHECK(crossing > 200);
}

TEST_CASE("going finer, a coarse chunk stays until all its children can be drawn, then they "
          "replace it at once, and going coarser the children stay until it can be drawn") {
    LodRings rings = with(360, 8.0);
    rings.view_distance = 40;
    rings.near_distance = 0;
    rings.near_height = 0;
    // Far from the eye a level-2 chunk P is drawn; nearer, its 8 children
    // are, each at level 1.
    const Vec3d far{.x = -2'000.5, .y = 40.0, .z = 0.5};
    const std::vector<LodPos> cover_far = vx::lod_cover(rings, far, -1, 2);
    std::optional<LodPos> picked;
    Vec3d near = far;
    for (const LodPos chunk : cover_far) {
        if (chunk.level != 2 || chunk.y != 0) {
            continue;
        }
        // An eye a little farther than the ring's start from the chunk, on
        // its x side, at its height.
        const LodBox box = vx::box_of(chunk);
        for (double gap = 1.0; gap < 600.0; gap += 1.0) {
            const Vec3d eye{.x = static_cast<double>(box.min_x) - gap,
                            .y = 40.0,
                            .z = static_cast<double>(box.min_z + box.max_z) / 2.0};
            const std::vector<LodPos> cover = vx::lod_cover(rings, eye, -1, 2);
            const LodSet set(cover.begin(), cover.end());
            const auto kids = vx::children(chunk);
            if (std::ranges::all_of(kids,
                                    [&](LodPos kid) {
                                        return kid.y < 0 || set.contains(kid) ||
                                               !in_rows(rings, eye, kid, -1, 2);
                                    }) &&
                !set.contains(chunk)) {
                picked = chunk;
                near = eye;
                break;
            }
        }
        if (picked) {
            break;
        }
    }
    REQUIRE(picked.has_value());
    const LodPos coarse = *picked;
    const auto kids = vx::children(coarse);
    // Everything ready but one child.
    const LodPos late = kids[5];
    bool late_ready = false;
    const auto ready = [&](LodPos pos) {
        return pos == late && !late_ready ? LodReady::waiting : LodReady::mesh;
    };
    const std::vector<LodPos> before = vx::lod_select(rings, far, -1, 2, ready);
    REQUIRE(LodSet(before.begin(), before.end()).contains(coarse));
    const std::vector<LodPos> waiting = vx::lod_select(rings, near, -1, 2, ready, before);
    const LodSet waiting_set(waiting.begin(), waiting.end());
    CHECK(waiting_set.contains(coarse));
    for (const LodPos kid : kids) {
        CHECK_FALSE(waiting_set.contains(kid));
    }
    late_ready = true;
    const std::vector<LodPos> swapped = vx::lod_select(rings, near, -1, 2, ready, waiting);
    const LodSet swapped_set(swapped.begin(), swapped.end());
    CHECK_FALSE(swapped_set.contains(coarse));
    std::size_t children_drawn = 0;
    for (const LodPos kid : kids) {
        children_drawn += swapped_set.contains(kid) ? 1U : 0U;
    }
    CHECK(children_drawn >= 4);
    // Back out, the coarse chunk's mesh gone meanwhile: its children stay
    // until it is ready again.
    bool coarse_ready = false;
    const auto again = [&](LodPos pos) {
        return pos == coarse && !coarse_ready ? LodReady::waiting : LodReady::mesh;
    };
    const std::vector<LodPos> out = vx::lod_select(rings, far, -1, 2, again, swapped);
    const LodSet out_set(out.begin(), out.end());
    CHECK_FALSE(out_set.contains(coarse));
    for (const LodPos kid : kids) {
        CHECK(out_set.contains(kid) == swapped_set.contains(kid));
    }
    // A kept child's mesh gone too: it is not drawn, and its siblings keep
    // what they drew.
    LodPos gone = kids[0];
    for (const LodPos kid : kids) {
        if (out_set.contains(kid)) {
            gone = kid;
            break;
        }
    }
    REQUIRE(out_set.contains(gone));
    const auto fewer = [&](LodPos pos) {
        return pos == gone || (pos == coarse && !coarse_ready) ? LodReady::waiting : LodReady::mesh;
    };
    const std::vector<LodPos> holed = vx::lod_select(rings, far, -1, 2, fewer, out);
    CHECK_FALSE(LodSet(holed.begin(), holed.end()).contains(gone));
    coarse_ready = true;
    const std::vector<LodPos> back = vx::lod_select(rings, far, -1, 2, again, out);
    const LodSet back_set(back.begin(), back.end());
    CHECK(back_set.contains(coarse));
    for (const LodPos kid : kids) {
        CHECK_FALSE(back_set.contains(kid));
    }
}

TEST_CASE("a coarse chunk with nothing to draw does not replace finer chunks drawn before, while "
          "a new child is not ready") {
    LodRings rings = with(360, 8.0);
    rings.view_distance = 40;
    rings.near_distance = 0;
    rings.near_height = 0;
    // A level-2 chunk whose place, near the eye, calls for its children.
    const Vec3d eye{.x = -2'000.5, .y = 40.0, .z = 0.5};
    std::optional<LodPos> coarse;
    for (const LodPos chunk : vx::lod_cover(rings, eye, -1, 2)) {
        if (chunk.level == 1 && chunk.y >= 0) {
            const LodPos up = vx::parent(chunk);
            if (rings.level_at(eye, up) < up.level &&
                std::ranges::all_of(vx::children(up), [&](LodPos kid) {
                    return kid.y < 0 || in_rows(rings, eye, kid, -1, 2);
                })) {
                coarse = up;
                break;
            }
        }
    }
    REQUIRE(coarse.has_value());
    const auto kids = vx::children(*coarse);
    // Drawn before: every child in the rows but one, which had nothing to
    // draw then and now waits for its mesh. The coarse chunk draws nothing.
    std::vector<LodPos> before;
    for (const LodPos kid : kids) {
        if (kid != kids[5] && kid.y >= -1) {
            before.push_back(kid);
        }
    }
    const auto ready = [&](LodPos pos) {
        if (pos == *coarse) {
            return LodReady::empty;
        }
        return pos == kids[5] ? LodReady::waiting : LodReady::mesh;
    };
    const std::vector<LodPos> drawn = vx::lod_select(rings, eye, -1, 2, ready, before);
    const LodSet now(drawn.begin(), drawn.end());
    for (const LodPos kid : before) {
        CHECK(now.contains(kid));
    }
}

TEST_CASE("a chunk outside its level's terrain ends the descent: nothing below it is asked") {
    LodRings rings = with(1'440, 4.0);
    rings.view_distance = 64;
    const Vec3d eye{.x = 0.5, .y = 3'000.0, .z = 0.5};
    std::size_t asked = 0;
    const std::vector<LodPos> drawn = vx::lod_select(rings, eye, -3, 6, [&asked](LodPos) {
        ++asked;
        return LodReady::outside;
    });
    CHECK(drawn.empty());
    // At most the top level's chunks over the disc's square, 5 by 5, in
    // the rows' two top-level rows; lod_cover goes down to level 3 here.
    CHECK(asked > 0);
    CHECK(asked <= 50);
}

TEST_CASE("where chunks of two levels meet, the camera is on the finer chunk's side of the face "
          "between them") {
    std::mt19937 random(0xFACE);
    std::uniform_real_distribution<double> across(-5.0e5, 5.0e5);
    std::uniform_real_distribution<double> height(-200.0, 3'000.0);
    std::size_t faces = 0;
    std::size_t wrong = 0;
    for (const auto& [base, distance] : std::array<std::pair<LodRings, std::int32_t>, 3>{
             {{with(1'440, 4.0), 64}, {with(360, 8.0), 96}, {with(1'440, 2.0), 48}}}) {
        for (int trial = 0; trial < 4; ++trial) {
            LodRings rings = base;
            rings.view_distance = distance;
            const Vec3d eye{.x = across(random), .y = height(random), .z = across(random)};
            const std::vector<LodPos> cover = vx::lod_cover(rings, eye, -4, 8);
            const LodSet drawn(cover.begin(), cover.end());
            for (const LodPos chunk : cover) {
                // Each face's neighbor drawn one level coarser.
                if (chunk.level == vx::max_lod_level) {
                    continue;
                }
                const LodBox box = vx::box_of(chunk);
                for (int axis = 0; axis < 3; ++axis) {
                    for (const int sign : {-1, 1}) {
                        std::array<std::int32_t, 3> at{chunk.x, chunk.y, chunk.z};
                        at.at(static_cast<std::size_t>(axis)) += sign;
                        const LodPos next{.x = at[0], .y = at[1], .z = at[2], .level = chunk.level};
                        const std::optional<LodPos> over = drawn_over(drawn, next);
                        if (!over || over->level != chunk.level + 1) {
                            continue;
                        }
                        // The face's plane, and the eye's side of it.
                        const std::array<std::int64_t, 2> extent =
                            axis == 0 ? std::array{box.min_x, box.max_x}
                                      : (axis == 1 ? std::array{box.min_y, box.max_y}
                                                   : std::array{box.min_z, box.max_z});
                        const double plane = static_cast<double>(sign > 0 ? extent[1] : extent[0]);
                        const std::array<double, 3> e{eye.x, eye.y, eye.z};
                        const double side = (e.at(static_cast<std::size_t>(axis)) - plane) * sign;
                        ++faces;
                        wrong += side > 0.0 ? 1 : 0; // on the coarser chunk's side
                    }
                }
            }
        }
    }
    CHECK(faces > 1'000);
    CHECK(wrong == 0);
}
