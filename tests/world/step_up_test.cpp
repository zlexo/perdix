#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <doctest/doctest.h>

#include <vector>

// RFC 0010's slab and step vectors run through the tick, which comes with
// item 3; these are the moves they are made of.

namespace {

using vx::Box;
using vx::Vec3d;
using vx::test::block_box;
using vx::test::floor_boxes;
using vx::test::low_box;
using vx::test::player_box;
using vx::test::with;

// RFC 0010's step height.
constexpr double step_height = 0.6;
// What a player standing on the ground asks of each move: gravity's step.
constexpr double gravity_step = -0.0784;

// Standing on the floor, its +x face 0.2 short of x = 1.
constexpr Box standing = player_box({.x = 0.5, .y = 0.0, .z = 0.5});

} // namespace

TEST_CASE("walking into a slab steps up onto it, losing nothing") {
    const std::vector<Box> obstacles = with(floor_boxes(), {low_box(1.0, 0.0, 0.5)});
    const Vec3d wanted{.x = 0.3, .y = gravity_step, .z = 0.0};
    const vx::Move move = vx::clip_move_stepping(standing, wanted, true, step_height, obstacles);
    CHECK(move.motion.x == 0.3);
    CHECK(move.motion.y == 0.5);
    CHECK(move.on_ground);
    CHECK_FALSE(move.x_collision);
    CHECK_FALSE(move.z_collision);
    // Without step-up, the slab is a wall.
    CHECK(vx::clip_move(standing, wanted, obstacles).motion.x == 1.0 - standing.max.x);
}

TEST_CASE("walking along z into a slab steps up too, as RFC 0010's slab vector does") {
    // The vector's slab: a box 0.5 tall at z from 1, walked into along +z.
    const std::vector<Box> obstacles = with(floor_boxes(), {low_box(0.0, 1.0, 0.5)});
    const vx::Move move = vx::clip_move_stepping(standing, {.x = 0.0, .y = gravity_step, .z = 0.3},
                                                 true, step_height, obstacles);
    CHECK(move.motion.z == 0.3);
    CHECK(move.motion.y == 0.5);
    CHECK_FALSE(move.z_collision);
}

TEST_CASE("a full block 1 above is never stepped onto: it takes a jump") {
    const std::vector<Box> obstacles = with(floor_boxes(), {low_box(1.0, 0.0, 1.0)});
    const vx::Move move = vx::clip_move_stepping(standing, {.x = 0.3, .y = gravity_step, .z = 0.0},
                                                 true, step_height, obstacles);
    CHECK(move.motion.x == 1.0 - standing.max.x);
    CHECK(move.motion.y == 0.0);
    CHECK(move.x_collision);
}

TEST_CASE("a surface exactly the step height up is stepped onto") {
    const std::vector<Box> obstacles = with(floor_boxes(), {low_box(1.0, 0.0, step_height)});
    const vx::Move move = vx::clip_move_stepping(standing, {.x = 0.3, .y = gravity_step, .z = 0.0},
                                                 true, step_height, obstacles);
    CHECK(move.motion.y == step_height);
    CHECK(move.motion.x == 0.3);
}

TEST_CASE("only a box on the ground steps up, or one that lands during the move") {
    const std::vector<Box> obstacles = with(floor_boxes(), {low_box(1.0, 0.0, 0.5)});
    const Box falling = player_box({.x = 0.5, .y = 0.25, .z = 0.5});

    // In the air, the slab is a wall.
    const vx::Move in_the_air = vx::clip_move_stepping(falling, {.x = 0.3, .y = -0.1, .z = 0.0},
                                                       false, step_height, obstacles);
    CHECK(in_the_air.motion.x == 1.0 - falling.max.x);
    CHECK(in_the_air.motion.y == -0.1);

    // Landing on the floor during the move, the box steps from where it
    // landed: down 0.25, then up 0.5.
    const vx::Move landing = vx::clip_move_stepping(falling, {.x = 0.3, .y = -0.5, .z = 0.0}, false,
                                                    step_height, obstacles);
    CHECK(landing.motion.x == 0.3);
    CHECK(landing.motion.y == 0.25);
    CHECK(landing.on_ground);
}

TEST_CASE("a box that lands during the move measures the step from where it landed") {
    // From above the slab's top, the box lands on the floor beside it and
    // steps up from the floor.
    const std::vector<Box> slab = with(floor_boxes(), {low_box(1.0, 0.0, 0.5)});
    const Box above = player_box({.x = 0.5, .y = 0.7, .z = 0.5});
    const vx::Move up_from_the_floor =
        vx::clip_move_stepping(above, {.x = 0.3, .y = -1.0, .z = 0.0}, false, step_height, slab);
    CHECK(up_from_the_floor.motion.x == 0.3);
    CHECK(up_from_the_floor.motion.y == -0.7 + 0.5);
    CHECK(up_from_the_floor.on_ground);

    // A box 0.75 tall is 0.5 above the falling box, but 0.75 above where it
    // lands: too high to step onto.
    const std::vector<Box> taller = with(floor_boxes(), {low_box(1.0, 0.0, 0.75)});
    const Box falling = player_box({.x = 0.5, .y = 0.25, .z = 0.5});
    const vx::Move against_it = vx::clip_move_stepping(falling, {.x = 0.3, .y = -0.5, .z = 0.0},
                                                       false, step_height, taller);
    CHECK(against_it.motion.y == -0.25);
    CHECK(against_it.motion.x == 1.0 - falling.max.x);
    CHECK(against_it.x_collision);
    CHECK(against_it.on_ground);
}

TEST_CASE("a jump into a slab steps onto it, as the game's jump cancel does") {
    const std::vector<Box> slab = with(floor_boxes(), {low_box(1.0, 0.0, 0.5)});
    const Vec3d jump{.x = 0.3, .y = 0.42, .z = 0.0};
    const vx::Move onto_the_slab = vx::clip_move_stepping(standing, jump, true, step_height, slab);
    CHECK(onto_the_slab.motion.x == 0.3);
    CHECK(onto_the_slab.motion.y == 0.5);
    // The jump's height was cut short, and moving up that is not the ground.
    CHECK(onto_the_slab.vertical_collision);
    CHECK_FALSE(onto_the_slab.on_ground);

    // Into a full block the jump rises along its face instead.
    const std::vector<Box> block = with(floor_boxes(), {low_box(1.0, 0.0, 1.0)});
    const vx::Move up_the_face = vx::clip_move_stepping(standing, jump, true, step_height, block);
    CHECK(up_the_face.motion.y == 0.42);
    CHECK(up_the_face.motion.x == 1.0 - standing.max.x);
}

TEST_CASE("a jump under a lintel walks in on the floor, and never ends in the air") {
    // A two-high opening: risen 0.42 the box meets the lintel, and on the
    // floor it passes under. The floor counts as a surface at 0 when the
    // plain move did not end there.
    const std::vector<Box> opening = with(floor_boxes(), {block_box(1.0, 2.0, 0.0)});
    const Vec3d jump{.x = 0.3, .y = 0.42, .z = 0.0};
    const vx::Move under = vx::clip_move_stepping(standing, jump, true, step_height, opening);
    CHECK(under.motion.x == 0.3);
    CHECK(under.motion.y == 0.0);

    // A carpet beside the path, not under the box, must not lift it 1/16.
    const std::vector<Box> carpet =
        with(opening,
             {{.min = {.x = 0.0, .y = 0.0, .z = 0.8}, .max = {.x = 1.0, .y = 0.0625, .z = 1.8}}});
    const vx::Move beside = vx::clip_move_stepping(standing, jump, true, step_height, carpet);
    CHECK(beside.motion.x == 0.3);
    CHECK(beside.motion.y == 0.0);
}

TEST_CASE("the lowest surface that lets the move go further wins") {
    // A 0.25 step, and a slab further on: rising 0.25 clears the step, and
    // the move ends short of the slab, on the step.
    const std::vector<Box> step_then_slab =
        with(floor_boxes(),
             {low_box(1.0, 0.0, 0.25),
              {.min = {.x = 1.4, .y = 0.0, .z = 0.0}, .max = {.x = 2.4, .y = 0.5, .z = 1.0}}});
    const Vec3d walk{.x = 0.3, .y = gravity_step, .z = 0.0};
    const vx::Move onto_the_step =
        vx::clip_move_stepping(standing, walk, true, step_height, step_then_slab);
    CHECK(onto_the_step.motion.y == 0.25);
    CHECK(onto_the_step.motion.x == 0.3);

    // A second 0.25 stacked on the first: rising 0.25 still runs into it,
    // so 0.5 wins.
    const std::vector<Box> stacked =
        with(floor_boxes(),
             {low_box(1.0, 0.0, 0.25),
              {.min = {.x = 1.0, .y = 0.25, .z = 0.0}, .max = {.x = 2.0, .y = 0.5, .z = 1.0}}});
    const vx::Move onto_both = vx::clip_move_stepping(standing, walk, true, step_height, stacked);
    CHECK(onto_both.motion.y == 0.5);
    CHECK(onto_both.motion.x == 0.3);
}

TEST_CASE("a stepped move cut short further on keeps its step, and tells the collision") {
    // On the slab, a wall 0.1 further on: the box steps up, then stops there.
    const std::vector<Box> obstacles =
        with(floor_boxes(),
             {low_box(1.0, 0.0, 0.5),
              {.min = {.x = 1.1, .y = 0.5, .z = 0.0}, .max = {.x = 2.1, .y = 2.5, .z = 1.0}}});
    const vx::Move move = vx::clip_move_stepping(standing, {.x = 0.5, .y = gravity_step, .z = 0.0},
                                                 true, step_height, obstacles);
    CHECK(move.motion.y == 0.5);
    CHECK(move.motion.x == 1.1 - standing.max.x);
    CHECK(move.x_collision);
    CHECK(move.on_ground);
}

TEST_CASE("a ceiling over the step keeps the box from stepping") {
    const std::vector<Box> obstacles =
        with(floor_boxes(), {low_box(1.0, 0.0, 0.5), block_box(1.0, 2.0, 0.0)});
    const vx::Move move = vx::clip_move_stepping(standing, {.x = 0.3, .y = gravity_step, .z = 0.0},
                                                 true, step_height, obstacles);
    CHECK(move.motion.y == 0.0);
    CHECK(move.motion.x == 1.0 - standing.max.x);
}

TEST_CASE("a step height of 0 never steps, not even onto the floor") {
    // MoveRules' default: no step-up at all, so a jump under a lintel stays
    // the plain move.
    const std::vector<Box> opening = with(floor_boxes(), {block_box(1.0, 2.0, 0.0)});
    const vx::Move move =
        vx::clip_move_stepping(standing, {.x = 0.3, .y = 0.42, .z = 0.0}, true, 0.0, opening);
    CHECK(move.motion.y == 0.42);
    CHECK(move.motion.x == 1.0 - standing.max.x);
}
