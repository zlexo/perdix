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
using vx::test::low_box;
using vx::test::player_box;
using vx::test::with;

// What a player standing on the ground asks of each move: gravity's step.
constexpr double gravity_step = -0.0784;

// Standing on the floor, its +x face 0.2 short of x = 1.
constexpr Box standing = player_box({.x = 0.5, .y = 0.0, .z = 0.5});

// The player's rules on the ground: step-up, and the edge guard as when
// sneaking.
constexpr vx::MoveRules sneaking_on_the_ground{
    .step_height = 0.6, .started_on_ground = true, .edge_drop = 0.6};

} // namespace

TEST_CASE("move_box gathers down to the edge's drop and up to the step's height") {
    vx::test::BoxWorld world;
    world.boxes = with(floor_boxes(), {low_box(1.0, 0.0, 0.5)});
    const Vec3d wanted{.x = 0.3, .y = gravity_step, .z = 0.0};
    std::vector<Box> scratch;

    // With the player's rules, the slab is stepped onto.
    const vx::Move stepped = vx::move_box(standing, wanted, world, scratch, sneaking_on_the_ground);
    REQUIRE(world.regions.size() == 1);
    const Box swept = vx::swept(standing, wanted);
    CHECK(world.regions[0].min.y == swept.min.y - 0.6);
    CHECK(world.regions[0].max.y == swept.max.y + 0.6);
    CHECK(stepped.motion.y == 0.5);
    CHECK(stepped.motion.x == 0.3);

    // With none, it is a wall, and only the swept region is asked for.
    const vx::Move plain = vx::move_box(standing, wanted, world, scratch);
    CHECK(world.regions[1] == swept);
    CHECK(plain.motion.x == 1.0 - standing.max.x);
}

TEST_CASE("the edge guard's cut is not a collision") {
    vx::test::BoxWorld world;
    world.boxes = {block_box(0.0, 0.0, 0.0)};
    std::vector<Box> scratch;
    const vx::Move move = vx::move_box(player_box({.x = 0.5, .y = 1.0, .z = 0.5}),
                                       {.x = 1.0, .y = gravity_step, .z = 0.0}, world, scratch,
                                       sneaking_on_the_ground);
    CHECK(move.motion.x == doctest::Approx(0.75));
    CHECK_FALSE(move.x_collision);
    CHECK(move.on_ground);
}

TEST_CASE("move_box steps a jump onto a slab only when the move began on the ground") {
    // A jump into a slab does not land, so only started_on_ground lets it
    // step.
    vx::test::BoxWorld world;
    world.boxes = with(floor_boxes(), {low_box(1.0, 0.0, 0.5)});
    std::vector<Box> scratch;
    const Vec3d jump{.x = 0.3, .y = 0.42, .z = 0.0};
    const vx::Move from_the_ground =
        vx::move_box(standing, jump, world, scratch,
                     {.step_height = 0.6, .started_on_ground = true, .edge_drop = 0.0});
    CHECK(from_the_ground.motion.y == 0.5);
    const vx::Move from_the_air =
        vx::move_box(standing, jump, world, scratch,
                     {.step_height = 0.6, .started_on_ground = false, .edge_drop = 0.0});
    CHECK(from_the_air.motion.y == 0.42);
    CHECK(from_the_air.motion.x == 1.0 - standing.max.x);
}

TEST_CASE("the edge's drop and the step's height each do their own part") {
    std::vector<Box> scratch;
    // A guard with no step-up: the slab stays a wall, and the region grows
    // down only.
    vx::test::BoxWorld slab;
    slab.boxes = with(floor_boxes(), {low_box(1.0, 0.0, 0.5)});
    const Vec3d walk{.x = 0.3, .y = gravity_step, .z = 0.0};
    const vx::Move against_it =
        vx::move_box(standing, walk, slab, scratch,
                     {.step_height = 0.0, .started_on_ground = true, .edge_drop = 0.6});
    CHECK(against_it.motion.y == 0.0);
    CHECK(against_it.motion.x == 1.0 - standing.max.x);
    REQUIRE(slab.regions.size() == 1);
    CHECK(slab.regions[0].min.y == vx::swept(standing, walk).min.y - 0.6);
    CHECK(slab.regions[0].max.y == vx::swept(standing, walk).max.y);

    // Step-up with no guard: a pillar's edge does not hold the box back, and
    // the region grows up only; with the guard alone, it does.
    vx::test::BoxWorld pillar;
    pillar.boxes = {block_box(0.0, 0.0, 0.0)};
    const Box on_top = player_box({.x = 0.5, .y = 1.0, .z = 0.5});
    const Vec3d off{.x = 1.0, .y = gravity_step, .z = 0.0};
    const vx::Move walks_off =
        vx::move_box(on_top, off, pillar, scratch,
                     {.step_height = 0.6, .started_on_ground = true, .edge_drop = 0.0});
    CHECK(walks_off.motion.x == 1.0);
    REQUIRE(pillar.regions.size() == 1);
    CHECK(pillar.regions[0].min.y == vx::swept(on_top, off).min.y);
    CHECK(pillar.regions[0].max.y == vx::swept(on_top, off).max.y + 0.6);
    const vx::Move held =
        vx::move_box(on_top, off, pillar, scratch,
                     {.step_height = 0.0, .started_on_ground = true, .edge_drop = 0.6});
    CHECK(held.motion.x == doctest::Approx(0.75));

    // The guard takes its own drop, not the step's height: with no step-up,
    // a drop of 0.5 is still no edge.
    vx::test::BoxWorld lower;
    lower.boxes = {
        block_box(0.0, -1.0, 0.0),
        {.min = {.x = 1.0, .y = -1.5, .z = 0.0}, .max = {.x = 3.0, .y = -0.5, .z = 1.0}}};
    const vx::Move down_a_little =
        vx::move_box(standing, off, lower, scratch,
                     {.step_height = 0.0, .started_on_ground = true, .edge_drop = 0.6});
    CHECK(down_a_little.motion.x == 1.0);
}

TEST_CASE("rules that are not finite do nothing, and ask the world for nothing") {
    vx::test::BoxWorld world;
    world.boxes = floor_boxes();
    std::vector<Box> scratch;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Vec3d walk{.x = 0.1, .y = gravity_step, .z = 0.0};
    CHECK(vx::move_box(standing, walk, world, scratch,
                       {.step_height = nan, .started_on_ground = true, .edge_drop = 0.6})
              .motion == Vec3d{});
    CHECK(vx::move_box(standing, walk, world, scratch,
                       {.step_height = 0.6, .started_on_ground = true, .edge_drop = nan})
              .motion == Vec3d{});
    CHECK(world.regions.empty());
}

TEST_CASE("a negative step height or drop never shrinks the region a move gathers") {
    // A jump under a ceiling 0.2 above the head: shrunk from the top, the
    // region would miss it.
    vx::test::BoxWorld world;
    world.boxes = with(floor_boxes(), {block_box(0.0, 2.0, 0.0)});
    std::vector<Box> scratch;
    const Vec3d jump{.x = 0.0, .y = 0.42, .z = 0.0};
    const vx::Move move =
        vx::move_box(standing, jump, world, scratch,
                     {.step_height = -0.5, .started_on_ground = true, .edge_drop = -0.5});
    REQUIRE(world.regions.size() == 1);
    CHECK(world.regions[0] == vx::swept(standing, jump));
    CHECK(move.motion.y == 2.0 - standing.max.y);
}
