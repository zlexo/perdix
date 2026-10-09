#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

// RFC 0007's frustum: the near and four side planes, conservative for boxes.

namespace {

using vx::Frustum;
using vx::Vec3d;

// Looking down -z with +x right and +y up, 90 degrees across vertically,
// twice as wide as tall, the near plane 0.05 ahead.
Frustum looking_down_minus_z() {
    return {{.x = 0.0, .y = 0.0, .z = -1.0},
            {.x = 1.0, .y = 0.0, .z = 0.0},
            {.x = 0.0, .y = 1.0, .z = 0.0},
            std::tan(std::numbers::pi / 4.0),
            2.0,
            0.05};
}

// A unit box centered on `center`.
bool sees(const Frustum& frustum, Vec3d center) {
    const Vec3d half{.x = 0.5, .y = 0.5, .z = 0.5};
    return frustum.may_contain(center - half, center + half);
}

} // namespace

TEST_CASE("a frustum holds what is ahead within its field of view, and not what is behind") {
    const Frustum frustum = looking_down_minus_z();
    CHECK(sees(frustum, {.x = 0.0, .y = 0.0, .z = -10.0}));
    CHECK_FALSE(sees(frustum, {.x = 0.0, .y = 0.0, .z = 10.0}));
    // 45 degrees up is the edge: a box well past it is out, one inside is in.
    CHECK(sees(frustum, {.x = 0.0, .y = 8.0, .z = -10.0}));
    CHECK_FALSE(sees(frustum, {.x = 0.0, .y = 12.0, .z = -10.0}));
    CHECK_FALSE(sees(frustum, {.x = 0.0, .y = -12.0, .z = -10.0}));
    // Twice as wide: 63 degrees to the side is in, 66 is out.
    CHECK(sees(frustum, {.x = 18.0, .y = 0.0, .z = -10.0}));
    CHECK_FALSE(sees(frustum, {.x = 23.0, .y = 0.0, .z = -10.0}));
    CHECK_FALSE(sees(frustum, {.x = -23.0, .y = 0.0, .z = -10.0}));
    // Nothing is too far: the far plane is at infinity.
    CHECK(sees(frustum, {.x = 0.0, .y = 0.0, .z = -1.0e7}));
}

TEST_CASE("a box the camera is inside, or one crossing a plane, counts as in") {
    const Frustum frustum = looking_down_minus_z();
    CHECK(frustum.may_contain({.x = -16.0, .y = -16.0, .z = -16.0},
                              {.x = 16.0, .y = 16.0, .z = 16.0}));
    // Mostly behind, its far end just past the near plane.
    CHECK(frustum.may_contain({.x = -1.0, .y = -1.0, .z = -0.1}, {.x = 1.0, .y = 1.0, .z = 30.0}));
    // Wholly behind the near plane.
    CHECK_FALSE(
        frustum.may_contain({.x = -1.0, .y = -1.0, .z = -0.04}, {.x = 1.0, .y = 1.0, .z = 30.0}));
}

TEST_CASE("a frustum turns with the view's axes, and a default one holds everything") {
    // Looking along +x, up still +y, so right is +z.
    const Frustum turned({.x = 1.0, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 0.0, .z = 1.0},
                         {.x = 0.0, .y = 1.0, .z = 0.0}, std::tan(std::numbers::pi / 4.0), 1.0,
                         0.05);
    CHECK(sees(turned, {.x = 10.0, .y = 0.0, .z = 0.0}));
    CHECK_FALSE(sees(turned, {.x = -10.0, .y = 0.0, .z = 0.0}));
    CHECK_FALSE(sees(turned, {.x = 0.0, .y = 0.0, .z = -10.0}));
    // Rolled a quarter turn: up is now -z, right +y, and the field is wider
    // than tall, so a box 18 to the side at 10 ahead is in only along +y.
    const Frustum rolled({.x = 1.0, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 1.0, .z = 0.0},
                         {.x = 0.0, .y = 0.0, .z = -1.0}, std::tan(std::numbers::pi / 4.0), 2.0,
                         0.05);
    CHECK(sees(rolled, {.x = 10.0, .y = 18.0, .z = 0.0}));
    CHECK_FALSE(sees(rolled, {.x = 10.0, .y = 0.0, .z = 18.0}));
    const Frustum everything;
    CHECK(sees(everything, {.x = 0.0, .y = 0.0, .z = 1.0e9}));
}
