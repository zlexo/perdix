#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <doctest/doctest.h>

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

} // namespace

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
