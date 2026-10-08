#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

using vx::Box;
using vx::Vec3d;

TEST_CASE("a body is 0.6 wide, 1.8 tall standing and 1.5 crouching") {
    const Vec3d feet{.x = 2.0, .y = 3.0, .z = -1.0};
    CHECK(vx::body_box(feet, vx::Pose::standing) ==
          Box{.min = {.x = 1.7, .y = 3.0, .z = -1.3}, .max = {.x = 2.3, .y = 4.8, .z = -0.7}});
    CHECK(vx::body_box(feet, vx::Pose::crouching).max.y == 4.5);
}

TEST_CASE("slippery blocks are slow to get going on and slow to stop on") {
    // RFC 0010's slipperiness: ground acceleration times (0.6 / s) cubed,
    // never more than 1, and drag s times 0.91.
    const vx::MovementNumbers numbers;
    CHECK(vx::ground_acceleration(0.6, false, numbers) == doctest::Approx(0.1));
    CHECK(vx::ground_acceleration(0.6, true, numbers) == doctest::Approx(0.13));
    CHECK(vx::ground_acceleration(0.98, false, numbers) == doctest::Approx(0.0229496));  // ice
    CHECK(vx::ground_acceleration(0.989, false, numbers) == doctest::Approx(0.0223288)); // blue ice
    CHECK(vx::ground_acceleration(0.8, false, numbers) == doctest::Approx(0.0421875));   // slime
    CHECK(vx::ground_acceleration(0.5, false, numbers) == doctest::Approx(0.1));         // capped
    CHECK(vx::ground_drag(0.6, numbers) == doctest::Approx(0.546));
    CHECK(vx::ground_drag(0.98, numbers) == doctest::Approx(0.8918));
    CHECK(vx::ground_drag(0.8, numbers) == doctest::Approx(0.728));
}
