#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/life.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

// RFC 0012's stroll spot, as 26.2 picks a cow's: 10 tries within 10 blocks
// across and 7 up or down, standing on a full block, lifted out of the
// ground but for 1 pick in 1,000, with room for his body and within 24
// blocks of the leash.

namespace {

using vx::Box;
using vx::Vec3d;
using vx::test::block_box;

constexpr Vec3d feet{.x = 0.5, .y = 0.0, .z = 0.5};

// Picks `count` spots from `feet`, leashed at `leash`, and keeps those
// found.
std::vector<Vec3d> picks(const vx::BlockBoxes& world, int count, Vec3d leash = feet,
                         std::uint64_t seed = 1) {
    vx::LifeRandom random(seed);
    std::vector<Box> scratch;
    std::vector<Vec3d> found;
    for (int i = 0; i < count; ++i) {
        if (const std::optional<Vec3d> spot =
                vx::stroll_spot(feet, leash, random, world, scratch)) {
            found.push_back(*spot);
        }
    }
    return found;
}

// Whether a box of `world` reaches into the block whose lowest corner is
// `corner`.
bool solid_block(const vx::BlockBoxes& world, Vec3d corner) {
    const Box cube{.min = corner, .max = corner + Vec3d{.x = 1.0, .y = 1.0, .z = 1.0}};
    std::vector<Box> boxes;
    world.collect(cube, boxes);
    return vx::overlaps_any(cube, boxes);
}

bool fits(const vx::BlockBoxes& world, Vec3d spot) {
    const Box body = vx::body_box(spot, vx::Pose::standing);
    std::vector<Box> boxes;
    world.collect(body, boxes);
    return !vx::overlaps_any(body, boxes);
}

double distance(Vec3d a, Vec3d b) {
    const Vec3d d = b - a;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

} // namespace

TEST_CASE("on flat ground a pick nearly always finds a spot on the floor within 10 across") {
    // Tries above the floor stand on air and fail, 7 in 15; tries in it
    // are lifted to its top. All 10 fail 1 pick in 2,000 or so, and the 1
    // pick in 1,000 that does not lift fails half the time.
    const vx::test::FlatWorld world;
    const std::vector<Vec3d> spots = picks(world, 20'000);
    const auto misses = static_cast<int>(20'000 - spots.size());
    INFO(misses, " picks found nothing");
    CHECK(misses >= 5);
    CHECK(misses <= 60);
    double farthest = 0.0;
    for (const Vec3d& spot : spots) {
        CHECK(spot.y == 0.0);
        CHECK(std::abs(spot.x - feet.x) <= 10.0);
        CHECK(std::abs(spot.z - feet.z) <= 10.0);
        farthest = std::max({farthest, std::abs(spot.x - feet.x), std::abs(spot.z - feet.z)});
    }
    CHECK(farthest == 10.0);
}

TEST_CASE("every spot stands on a block with room for him in reach and leashed") {
    // Piles a block and two blocks high, half blocks, and a low roof: spots
    // stand on the floor, the piles, or over the half blocks a try inside
    // them is lifted out of, as in the game; never under the roof.
    vx::test::FlatWorld world;
    for (int x = -12; x <= 12; x += 2) {
        for (int z = -12; z <= 12; z += 3) {
            const int kind = ((x * 7) + (z * 3) + 99) % 4;
            if (kind == 1 || kind == 2) {
                world.boxes.push_back(block_box(x, 0.0, z));
            }
            if (kind == 2) {
                world.boxes.push_back(block_box(x, 1.0, z));
            }
            if (kind == 3) {
                world.boxes.push_back(vx::test::low_box(x, z, 0.5));
            }
        }
    }
    world.boxes.push_back(
        {.min = {.x = -6.0, .y = 1.5, .z = 4.0}, .max = {.x = -2.0, .y = 2.0, .z = 8.0}});
    const Vec3d leash{.x = 6.0, .y = 0.0, .z = -6.0};
    const std::vector<Vec3d> spots = picks(world, 5'000, leash);
    CHECK(spots.size() > 4'000);
    bool on_piles = false;
    for (const Vec3d& spot : spots) {
        INFO("a spot at ", spot.x, " ", spot.y, " ", spot.z);
        CHECK(std::abs(std::floor(spot.x) - std::floor(feet.x)) <= 10.0);
        CHECK(std::abs(std::floor(spot.z) - std::floor(feet.z)) <= 10.0);
        CHECK(std::abs(spot.y - feet.y) <= 7.0);
        CHECK(spot.x - std::floor(spot.x) == 0.5);
        CHECK(spot.z - std::floor(spot.z) == 0.5);
        CHECK(solid_block(world,
                          {.x = std::floor(spot.x), .y = spot.y - 1.0, .z = std::floor(spot.z)}));
        CHECK(fits(world, spot));
        CHECK(distance(spot, leash) <= 24.0);
        on_piles = on_piles || spot.y > 0.0;
    }
    CHECK(on_piles);
}

TEST_CASE("a try inside the ground is lifted to its top") {
    // Solid ground 3 blocks above the floor to the east: tries in it come
    // out on its top.
    vx::test::FlatWorld world;
    for (int x = 3; x <= 6; ++x) {
        for (int z = -10; z <= 10; ++z) {
            for (int y = 0; y < 3; ++y) {
                world.boxes.push_back(block_box(x, y, z));
            }
        }
    }
    const std::vector<Vec3d> spots = picks(world, 5'000);
    bool lifted = false;
    for (const Vec3d& spot : spots) {
        INFO("a spot at ", spot.x, " ", spot.y, " ", spot.z);
        const bool over_the_ground = spot.x > 3.0 && spot.x < 7.0;
        CHECK(spot.y == (over_the_ground ? 3.0 : 0.0));
        lifted = lifted || over_the_ground;
    }
    CHECK(lifted);
}

TEST_CASE("lifting gives up after 16 blocks") {
    // A wall 30 blocks high to the west, its top beyond 16 blocks of
    // lifting from any try, and the leash halfway up, within reach of both
    // its top and the floor.
    vx::test::FlatWorld world;
    world.boxes.push_back(
        {.min = {.x = -11.0, .y = 0.0, .z = -11.0}, .max = {.x = -5.0, .y = 30.0, .z = 11.0}});
    const std::vector<Vec3d> spots = picks(world, 5'000, {.x = -8.0, .y = 15.0, .z = 0.5});
    CHECK(spots.size() > 1'000);
    for (const Vec3d& spot : spots) {
        INFO("a spot at ", spot.x, " ", spot.y, " ", spot.z);
        CHECK(spot.x > -5.0);
        CHECK(spot.y == 0.0);
    }
}

TEST_CASE("a try stands on a full block 7 blocks up and no higher") {
    // Shelves hanging in the air: of blocks at y = 6 to the east, of blocks
    // at y = 7 to the west, of half blocks at y = 6 to the north. Tries 7 up
    // stand on the first; none reach the second, and none stand on half
    // blocks.
    vx::test::FlatWorld world;
    for (int z = -10; z <= 10; ++z) {
        for (int x = 3; x <= 6; ++x) {
            world.boxes.push_back(block_box(x, 6.0, z));
        }
        for (int x = -7; x <= -4; ++x) {
            world.boxes.push_back(block_box(x, 7.0, z));
        }
    }
    for (int x = -2; x <= 2; ++x) {
        for (int z = -10; z <= -3; ++z) {
            const Vec3d low{.x = static_cast<double>(x), .y = 6.0, .z = static_cast<double>(z)};
            world.boxes.push_back({.min = low, .max = low + Vec3d{.x = 1.0, .y = 0.5, .z = 1.0}});
        }
    }
    const std::vector<Vec3d> spots = picks(world, 5'000);
    bool on_the_shelf = false;
    for (const Vec3d& spot : spots) {
        INFO("a spot at ", spot.x, " ", spot.y, " ", spot.z);
        on_the_shelf = on_the_shelf || spot.y == 7.0;
        CHECK(spot.y != 8.0);
        CHECK((spot.y == 0.0 || (spot.x > 3.0 && spot.x < 7.0)));
    }
    CHECK(on_the_shelf);
}

TEST_CASE("no spot lies farther than 24 blocks from the leash") {
    const vx::test::FlatWorld world;
    const Vec3d leash{.x = 30.0, .y = 0.0, .z = 0.5};
    const std::vector<Vec3d> spots = picks(world, 5'000, leash);
    CHECK(spots.size() > 100);
    for (const Vec3d& spot : spots) {
        CHECK(distance(spot, leash) <= 24.0);
    }
    CHECK(picks(world, 5'000, {.x = 40.0, .y = 0.0, .z = 0.5}).empty());
}

TEST_CASE("the same seed picks the same spots") {
    const vx::test::FlatWorld world;
    CHECK(picks(world, 200, feet, 9) == picks(world, 200, feet, 9));
    CHECK(picks(world, 200, feet, 9) != picks(world, 200, feet, 10));
}
