#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <vector>

namespace {

using vx::Box;
using vx::Vec3d;
using vx::test::block_box;
using vx::test::floor_boxes;
using vx::test::player_box;

// The most a sneaking player may drop over an edge: RFC 0010's step height.
constexpr double edge_drop = 0.6;
// What a player standing on the ground asks of each move: gravity's step.
constexpr double gravity_step = -0.0784;

// Standing on the floor, its +x face 0.2 short of x = 1.
constexpr Box standing = player_box({.x = 0.5, .y = 0.0, .z = 0.5});

// A floor at y = 0 for x below 1, and one `drop` lower beyond, its top set
// directly so the drop is exact.
std::vector<Box> two_levels(double drop) {
    std::vector<Box> boxes;
    for (int z = -1; z < 2; ++z) {
        const double row = z;
        boxes.push_back(block_box(0.0, -1.0, row));
        boxes.push_back({.min = {.x = 1.0, .y = -drop - 1.0, .z = row},
                         .max = {.x = 3.0, .y = -drop, .z = row + 1.0}});
    }
    return boxes;
}

} // namespace

TEST_CASE("overlaps_any: inside a block by more than the tolerance, not resting on one") {
    const std::vector<Box> floor = floor_boxes();
    CHECK_FALSE(vx::overlaps_any(standing, floor));
    CHECK_FALSE(vx::overlaps_any(vx::moved(standing, {.x = 0.0, .y = -5e-8, .z = 0.0}), floor));
    CHECK(vx::overlaps_any(vx::moved(standing, {.x = 0.0, .y = -2e-7, .z = 0.0}), floor));
}

TEST_CASE("sneaking stops a little past a pillar's edge, as the game's guard does") {
    // Standing on a lone pillar; the guard keeps part of the box over it.
    const std::vector<Box> pillar = {block_box(0.0, 0.0, 0.0)};
    const Box on_top = player_box({.x = 0.5, .y = 1.0, .z = 0.5});

    const Vec3d off{.x = 1.0, .y = gravity_step, .z = 0.0};
    const Vec3d held = vx::guard_edge(on_top, off, edge_drop, pillar);
    CHECK(held.x == doctest::Approx(0.75));
    CHECK(held.y == gravity_step);
    CHECK(held.z == 0.0);

    // A move that keeps the box over the pillar is not cut.
    const Vec3d short_move{.x = 0.5, .y = gravity_step, .z = 0.0};
    CHECK(vx::guard_edge(on_top, short_move, edge_drop, pillar) == short_move);
}

TEST_CASE("the guard cuts in steps of 0.05, and to nothing once under 0.05") {
    const std::vector<Box> pillar = {block_box(0.0, 0.0, 0.0)};
    const Vec3d wanted{.x = 0.12, .y = gravity_step, .z = 0.0};
    // 0.03 of the box over the pillar: 0.12 and 0.07 go past the edge, 0.02
    // does not.
    CHECK(
        vx::guard_edge(player_box({.x = 1.27, .y = 1.0, .z = 0.5}), wanted, edge_drop, pillar).x ==
        doctest::Approx(0.02));
    // 0.01 over: 0.02 goes past it too, and under 0.05 the cut is to
    // nothing, never turned around.
    CHECK(
        vx::guard_edge(player_box({.x = 1.29, .y = 1.0, .z = 0.5}), wanted, edge_drop, pillar).x ==
        0.0);
    CHECK(vx::guard_edge(player_box({.x = 1.29, .y = 1.0, .z = 0.5}),
                         {.x = 0.03, .y = gravity_step, .z = 0.0}, edge_drop, pillar)
              .x == 0.0);
}

TEST_CASE("the guard tries each axis alone, then both: an L of blocks holds a diagonal back") {
    // The corner block of the L is missing: either axis alone keeps the box
    // over a block, both together do not.
    const std::vector<Box> l_shape = {block_box(0.0, 0.0, 0.0), block_box(1.0, 0.0, 0.0),
                                      block_box(0.0, 0.0, 1.0)};
    const Box on_top = player_box({.x = 0.5, .y = 1.0, .z = 0.5});
    const Vec3d held =
        vx::guard_edge(on_top, {.x = 0.8, .y = gravity_step, .z = 0.8}, edge_drop, l_shape);
    CHECK(held.x == doctest::Approx(0.75));
    CHECK(held.z == doctest::Approx(0.75));
}

TEST_CASE("sneaking diagonally against a bridge's side keeps the motion along it") {
    // A bridge along z, one block wide: x alone runs off its side and z alone
    // stays on it, so x is cut and z is not.
    std::vector<Box> bridge;
    for (int z = -2; z < 4; ++z) {
        bridge.push_back(block_box(0.0, 0.0, z));
    }
    const Box at_the_side = player_box({.x = 1.27, .y = 1.0, .z = 0.5});
    const Vec3d held =
        vx::guard_edge(at_the_side, {.x = 0.32, .y = gravity_step, .z = 0.3}, edge_drop, bridge);
    CHECK(held.x == doctest::Approx(0.02));
    CHECK(held.z == 0.3);
}

TEST_CASE("a drop of 0.6 or less is not an edge, and a deeper one is") {
    const Vec3d off{.x = 1.0, .y = gravity_step, .z = 0.0};
    CHECK(vx::guard_edge(standing, off, edge_drop, two_levels(0.5)).x == 1.0);
    CHECK(vx::guard_edge(standing, off, edge_drop, two_levels(0.6)).x == 1.0);
    CHECK(vx::guard_edge(standing, off, edge_drop, two_levels(0.62)).x == doctest::Approx(0.75));
    CHECK(vx::guard_edge(standing, off, edge_drop, two_levels(1.0)).x == doctest::Approx(0.75));
}

TEST_CASE("only the space under the feet is ground: a block at chest height is not") {
    // RFC 0010: a block box within 0.6 below the bottom. This one is 0.5 to
    // 1.5 above the feet, its face 0.62 past the edge: a box stopped by it
    // would already be past the edge.
    const std::vector<Box> obstacles = {
        block_box(0.0, -1.0, 0.0),
        {.min = {.x = 1.62, .y = 0.5, .z = 0.0}, .max = {.x = 2.62, .y = 1.5, .z = 1.0}}};
    const Box at_the_edge = player_box({.x = 1.28, .y = 0.0, .z = 0.5});
    const Vec3d held = vx::guard_edge(at_the_edge, {.x = 0.065, .y = gravity_step, .z = 0.0},
                                      edge_drop, obstacles);
    CHECK(held.x == doctest::Approx(0.015));
}

TEST_CASE("the guard does nothing moving up, or once the box is high above the ground") {
    const std::vector<Box> pillar = {block_box(0.0, 0.0, 0.0)};
    const Vec3d jumping_off{.x = 1.0, .y = 0.42, .z = 0.0};
    CHECK(vx::guard_edge(player_box({.x = 0.5, .y = 1.0, .z = 0.5}), jumping_off, edge_drop,
                         pillar) == jumping_off);
    const Vec3d falling{.x = 1.0, .y = -0.2, .z = 0.0};
    CHECK(vx::guard_edge(player_box({.x = 0.5, .y = 2.0, .z = 0.5}), falling, edge_drop, pillar) ==
          falling);
}

TEST_CASE("a motion of 12 blocks is still cut back to the edge") {
    const std::vector<Box> pillar = {block_box(0.0, 0.0, 0.0)};
    const Box on_top = player_box({.x = 0.5, .y = 1.0, .z = 0.5});
    CHECK(vx::guard_edge(on_top, {.x = 12.0, .y = 0.0, .z = 0.0}, edge_drop, pillar).x ==
          doctest::Approx(0.75));
}

TEST_CASE("a motion that never finds ground is cut to nothing, and the guard ends") {
    const std::vector<Box> pillar = {block_box(0.0, 0.0, 0.0)};
    const Box on_top = player_box({.x = 0.5, .y = 1.0, .z = 0.5});
    CHECK(vx::guard_edge(on_top, {.x = 1e9, .y = 0.0, .z = 0.0}, edge_drop, pillar).x == 0.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(vx::guard_edge(on_top, {.x = nan, .y = 0.0, .z = 0.0}, edge_drop, pillar).x == 0.0);
}
