#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <doctest/doctest.h>

#include <initializer_list>
#include <limits>
#include <vector>

// Where a move is cut short, the expected length is worked out with the same
// subtraction the code does, so it compares exactly: an error of 1e-6, ten
// times the tolerance, would leave a box inside a block.

namespace {

using vx::Axis;
using vx::Box;
using vx::Vec3d;
using vx::test::block_box;
using vx::test::player_box;

// A wall two blocks tall in the plane x = 1, from z = -2 to 2: the player's
// 1.8 runs into both rows.
std::vector<Box> wall_at_x1() {
    std::vector<Box> wall;
    for (int z = -2; z < 2; ++z) {
        wall.push_back(block_box(1.0, 0.0, z));
        wall.push_back(block_box(1.0, 1.0, z));
    }
    return wall;
}

} // namespace

TEST_CASE("a box moves the whole way when nothing is in its path") {
    const std::vector<Box> far = {block_box(5.0, 0.0, 5.0)};
    const Vec3d wanted{.x = 0.2, .y = -0.1, .z = -0.3};
    const vx::Move move = vx::clip_move(player_box({.x = 0.5, .y = 3.0, .z = 0.5}), wanted, far);
    CHECK(move.motion == wanted);
    CHECK_FALSE(move.x_collision);
    CHECK_FALSE(move.vertical_collision);
    CHECK_FALSE(move.z_collision);
    CHECK_FALSE(move.on_ground);
}

TEST_CASE("a falling box lands on the block below, and is on the ground") {
    const std::vector<Box> below = {block_box(0.0, 0.0, 0.0)};
    const Box box = player_box({.x = 0.5, .y = 1.5, .z = 0.5});

    const vx::Move landing = vx::clip_move(box, {.x = 0.0, .y = -0.6, .z = 0.0}, below);
    CHECK(landing.motion.y == -0.5);
    CHECK(landing.vertical_collision);
    CHECK(landing.on_ground);
    CHECK_FALSE(landing.horizontal_collision());

    const vx::Move short_of_it = vx::clip_move(box, {.x = 0.0, .y = -0.4, .z = 0.0}, below);
    CHECK(short_of_it.motion.y == -0.4);
    CHECK_FALSE(short_of_it.on_ground);
}

TEST_CASE("a falling box passes a block above it") {
    // Only blocks ahead of a move stop it: one over the head, which a jump
    // just touched, does not hold up the fall that follows.
    const std::vector<Box> above = {block_box(0.0, 2.8, 0.0)};
    const vx::Move move = vx::clip_move(player_box({.x = 0.5, .y = 1.0, .z = 0.5}),
                                        {.x = 0.0, .y = -0.5, .z = 0.0}, above);
    CHECK(move.motion.y == -0.5);
    CHECK_FALSE(move.vertical_collision);
    CHECK_FALSE(move.on_ground);
}

TEST_CASE("standing on a block, gravity's step is cut to nothing every move") {
    // RFC 0010: the move cancels gravity's step each tick, which is how the
    // next tick knows the player is on the ground. A box a hair inside the
    // block, from rounding, does not sink further either.
    const std::vector<Box> below = {block_box(0.0, 0.0, 0.0)};
    for (const double feet : {1.0, 1.0 - 5e-8}) {
        INFO("feet at ", feet);
        const vx::Move move = vx::clip_move(player_box({.x = 0.5, .y = feet, .z = 0.5}),
                                            {.x = 0.0, .y = -0.0784, .z = 0.0}, below);
        CHECK(move.motion.y == 0.0);
        CHECK(move.on_ground);
    }
}

TEST_CASE("a ceiling stops a box moving up, and that is not the ground") {
    const std::vector<Box> above = {block_box(0.0, 2.0, 0.0)};
    const Box box = player_box({.x = 0.5, .y = 0.0, .z = 0.5});
    const vx::Move move = vx::clip_move(box, {.x = 0.0, .y = 0.42, .z = 0.0}, above);
    CHECK(move.motion.y == 2.0 - box.max.y);
    CHECK(move.vertical_collision);
    CHECK_FALSE(move.on_ground);
}

TEST_CASE("a wall stops the axis that runs into it, and the other carries on") {
    const Box box = player_box({.x = 0.5, .y = 0.0, .z = 0.5});

    const vx::Move across_x = vx::clip_move(box, {.x = 0.3, .y = 0.0, .z = 0.2}, wall_at_x1());
    CHECK(across_x.motion.x == 1.0 - box.max.x);
    CHECK(across_x.motion.z == 0.2);
    CHECK(across_x.x_collision);
    CHECK_FALSE(across_x.z_collision);
    CHECK(across_x.horizontal_collision());
    CHECK_FALSE(across_x.vertical_collision);

    // The same against a wall across z.
    const std::vector<Box> wall_at_z1 = {block_box(0.0, 0.0, 1.0), block_box(0.0, 1.0, 1.0)};
    const vx::Move across_z = vx::clip_move(box, {.x = 0.0, .y = 0.0, .z = 0.3}, wall_at_z1);
    CHECK(across_z.motion.z == 1.0 - box.max.z);
    CHECK(across_z.z_collision);
    CHECK_FALSE(across_z.x_collision);
    CHECK(across_z.horizontal_collision());
}

TEST_CASE("faces a box rests against do not hold it back") {
    // Standing on a floor, and pressed flat against a wall, the box still
    // moves freely along both.
    std::vector<Box> obstacles = wall_at_x1();
    for (int x = -2; x < 1; ++x) {
        for (int z = -2; z < 2; ++z) {
            obstacles.push_back(block_box(x, -1.0, z));
        }
    }
    const Box against_the_wall = {.min = {.x = 0.4, .y = 0.0, .z = 0.2},
                                  .max = {.x = 1.0, .y = 1.8, .z = 0.8}};
    const Vec3d along_the_wall{.x = 0.0, .y = 0.0, .z = -0.5};
    const vx::Move move = vx::clip_move(against_the_wall, along_the_wall, obstacles);
    CHECK(move.motion == along_the_wall);
    CHECK_FALSE(move.horizontal_collision());
}

TEST_CASE("the larger horizontal axis moves first, so a corner stops each "
          "diagonal differently") {
    // A pillar diagonally ahead of the box: whichever axis moves first passes
    // beside it, and the second runs into it.
    const std::vector<Box> pillar = {block_box(1.0, 0.0, 1.0), block_box(1.0, 1.0, 1.0)};
    const Box box = player_box({.x = 0.5, .y = 0.0, .z = 0.5});

    const vx::Move more_x = vx::clip_move(box, {.x = 0.5, .y = 0.0, .z = 0.4}, pillar);
    CHECK(more_x.motion.x == 0.5);
    CHECK(more_x.motion.z == 1.0 - box.max.z);
    CHECK(more_x.z_collision);
    CHECK_FALSE(more_x.x_collision);

    const vx::Move more_z = vx::clip_move(box, {.x = 0.4, .y = 0.0, .z = 0.5}, pillar);
    CHECK(more_z.motion.x == 1.0 - box.max.x);
    CHECK(more_z.motion.z == 0.5);
    CHECK(more_z.x_collision);
    CHECK_FALSE(more_z.z_collision);

    // On a tie, x goes first.
    const vx::Move tie = vx::clip_move(box, {.x = 0.45, .y = 0.0, .z = 0.45}, pillar);
    CHECK(tie.motion.x == 0.45);
    CHECK(tie.motion.z == 1.0 - box.max.z);
}

TEST_CASE("the larger axis is the larger distance, whatever the direction") {
    // The same pillar on the other side: moving toward negative x and z, the
    // axis that moves further goes first.
    const std::vector<Box> pillar = {block_box(-1.0, 0.0, -1.0), block_box(-1.0, 1.0, -1.0)};
    const Box box = player_box({.x = 0.5, .y = 0.0, .z = 0.5});

    const vx::Move more_x = vx::clip_move(box, {.x = -0.5, .y = 0.0, .z = -0.4}, pillar);
    CHECK(more_x.motion.x == -0.5);
    CHECK(more_x.motion.z == 0.0 - box.min.z);

    const vx::Move more_z = vx::clip_move(box, {.x = -0.4, .y = 0.0, .z = -0.5}, pillar);
    CHECK(more_z.motion.x == 0.0 - box.min.x);
    CHECK(more_z.motion.z == -0.5);
}

TEST_CASE("y moves first, so the move that walks off an edge still ends on the "
          "ground") {
    // RFC 0010: the tick a player walks off an edge ends on the ground, so a
    // jump on the next tick still works; the move after that falls.
    const std::vector<Box> ledge = {block_box(0.0, 0.0, 0.0)};
    const Box at_the_edge = player_box({.x = 1.1, .y = 1.0, .z = 0.5});

    const vx::Move off = vx::clip_move(at_the_edge, {.x = 0.5, .y = -0.0784, .z = 0.0}, ledge);
    CHECK(off.motion.y == 0.0);
    CHECK(off.on_ground);
    CHECK(off.motion.x == 0.5);

    const vx::Move falling = vx::clip_move(vx::moved(at_the_edge, off.motion),
                                           {.x = 0.25, .y = -0.0784, .z = 0.0}, ledge);
    CHECK(falling.motion.y == -0.0784);
    CHECK_FALSE(falling.on_ground);
}

TEST_CASE("a box over the seam between two blocks lands on both") {
    const std::vector<Box> floor = {block_box(0.0, 0.0, 0.0), block_box(1.0, 0.0, 0.0)};
    const vx::Move move = vx::clip_move(player_box({.x = 1.0, .y = 1.25, .z = 0.5}),
                                        {.x = 0.0, .y = -1.0, .z = 0.0}, floor);
    CHECK(move.motion.y == -0.25);
    CHECK(move.on_ground);
}

TEST_CASE("clip_axis counts only obstacles ahead of the box, moving toward "
          "positive x") {
    const Box box = player_box({.x = 0.5, .y = 0.0, .z = 0.5});
    // Behind, beside, and one the box is already inside: none of them stop it.
    const std::vector<Box> not_ahead = {block_box(-2.0, 0.0, 0.0), block_box(0.0, 0.0, 3.0),
                                        block_box(0.0, 0.5, 0.0)};
    CHECK(vx::clip_axis(box, Axis::x, 0.75, not_ahead) == 0.75);
    // A block ahead stops it at its face; against the face already, at once.
    const std::vector<Box> ahead = {block_box(1.0, 0.0, 0.0)};
    CHECK(vx::clip_axis(box, Axis::x, 0.5, ahead) == 1.0 - box.max.x);
    const Box against = {.min = {.x = 0.4, .y = 0.0, .z = 0.2},
                         .max = {.x = 1.0, .y = 1.8, .z = 0.8}};
    CHECK(vx::clip_axis(against, Axis::x, 0.5, ahead) == 0.0);
    CHECK(vx::clip_axis(box, Axis::x, 0.0, ahead) == 0.0);
}

TEST_CASE("clip_axis counts only obstacles ahead of the box, moving toward "
          "negative x") {
    const Box box = player_box({.x = 0.5, .y = 0.0, .z = 0.5});
    const std::vector<Box> not_ahead = {block_box(2.0, 0.0, 0.0), block_box(0.0, 0.0, 3.0),
                                        block_box(0.0, 0.5, 0.0)};
    CHECK(vx::clip_axis(box, Axis::x, -0.75, not_ahead) == -0.75);
    const std::vector<Box> ahead = {block_box(-1.0, 0.0, 0.0)};
    CHECK(vx::clip_axis(box, Axis::x, -0.5, ahead) == 0.0 - box.min.x);
    const Box against = {.min = {.x = 0.0, .y = 0.0, .z = 0.2},
                         .max = {.x = 0.6, .y = 1.8, .z = 0.8}};
    CHECK(vx::clip_axis(against, Axis::x, -0.5, ahead) == 0.0);
}

TEST_CASE("a box a hair inside the floor still slides over the seams between "
          "its blocks") {
    // Rounding can leave a box a hair inside the block it stands on; the
    // next block of the floor must not then count as a wall.
    std::vector<Box> floor;
    for (int x = 0; x < 3; ++x) {
        floor.push_back(block_box(x, -1.0, 0.0));
    }
    const Box sunk =
        vx::moved(player_box({.x = 0.5, .y = 0.0, .z = 0.5}), {.x = 0.0, .y = -5e-8, .z = 0.0});
    CHECK(vx::clip_axis(sunk, Axis::x, 1.5, floor) == 1.5);
    // And the same under a ceiling, with the box a hair into it from below.
    std::vector<Box> ceiling;
    for (int x = 0; x < 3; ++x) {
        ceiling.push_back(block_box(x, 1.8, 0.0));
    }
    const Box raised =
        vx::moved(player_box({.x = 0.5, .y = 0.0, .z = 0.5}), {.x = 0.0, .y = 5e-8, .z = 0.0});
    CHECK(vx::clip_axis(raised, Axis::x, 1.5, ceiling) == 1.5);
}

TEST_CASE("more than the tolerance inside the floor, its seams hold the box back") {
    // The tolerance's size: 2e-7 inside is more than a hair, and the next
    // block of the floor stops the box at its face.
    const std::vector<Box> floor = {block_box(0.0, 0.0, 0.0), block_box(1.0, 0.0, 0.0)};
    const Box sunk = player_box({.x = 0.5, .y = 1.0 - 2e-7, .z = 0.5});
    CHECK(vx::clip_axis(sunk, Axis::x, 1.0, floor) == 1.0 - sunk.max.x);
}

TEST_CASE("a box a hair inside a block is still stopped by it, and not pushed back") {
    const std::vector<Box> wall = {block_box(1.0, 0.0, 0.0), block_box(1.0, 1.0, 0.0)};
    const Box into_it = {.min = {.x = 0.4 + 5e-8, .y = 0.0, .z = 0.2},
                         .max = {.x = 1.0 + 5e-8, .y = 1.8, .z = 0.8}};
    CHECK(vx::clip_axis(into_it, Axis::x, 0.3, wall) == 0.0);
    // And the same moving toward negative x, a hair into a block on that side.
    const std::vector<Box> other_wall = {block_box(-1.0, 0.0, 0.0), block_box(-1.0, 1.0, 0.0)};
    const Box into_the_other = {.min = {.x = -5e-8, .y = 0.0, .z = 0.2},
                                .max = {.x = 0.6 - 5e-8, .y = 1.8, .z = 0.8}};
    CHECK(vx::clip_axis(into_the_other, Axis::x, -0.3, other_wall) == 0.0);
}

TEST_CASE("swept grows a box toward each axis's motion, whichever its sign") {
    const Box box = player_box({.x = 0.5, .y = 0.0, .z = 0.5});

    const Box one_way = vx::swept(box, {.x = 0.5, .y = -0.25, .z = 0.75});
    CHECK(one_way.min == Vec3d{.x = box.min.x, .y = box.min.y - 0.25, .z = box.min.z});
    CHECK(one_way.max == Vec3d{.x = box.max.x + 0.5, .y = box.max.y, .z = box.max.z + 0.75});

    const Box other_way = vx::swept(box, {.x = -0.5, .y = 0.25, .z = -0.75});
    CHECK(other_way.min == Vec3d{.x = box.min.x - 0.5, .y = box.min.y, .z = box.min.z - 0.75});
    CHECK(other_way.max == Vec3d{.x = box.max.x, .y = box.max.y + 0.25, .z = box.max.z});
}
