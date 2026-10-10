#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <doctest/doctest.h>

#include <limits>

// A segment against a box, as a line of sight between two eyes meets the
// blocks (RFC 0012).

namespace {

using vx::Box;
using vx::Vec3d;

constexpr Box block{.min = {.x = 0.0, .y = 0.0, .z = 0.0}, .max = {.x = 1.0, .y = 1.0, .z = 1.0}};

} // namespace

TEST_CASE("a segment through a box enters it but one that stops short or passes by does not") {
    CHECK(
        vx::segment_enters(block, {.x = -1.0, .y = 0.5, .z = 0.5}, {.x = 2.0, .y = 0.5, .z = 0.5}));
    CHECK(
        vx::segment_enters(block, {.x = 0.5, .y = 3.0, .z = 0.5}, {.x = 0.5, .y = -3.0, .z = 0.5}));
    // Diagonally, through a corner region.
    CHECK(vx::segment_enters(block, {.x = -0.5, .y = -0.5, .z = -0.5},
                             {.x = 0.5, .y = 0.5, .z = 0.5}));
    // Stopping short of the face, or ending on it.
    CHECK_FALSE(vx::segment_enters(block, {.x = -2.0, .y = 0.5, .z = 0.5},
                                   {.x = -0.1, .y = 0.5, .z = 0.5}));
    CHECK_FALSE(
        vx::segment_enters(block, {.x = -2.0, .y = 0.5, .z = 0.5}, {.x = 0.0, .y = 0.5, .z = 0.5}));
    // Passing beside it, above it, or diagonally past a corner.
    CHECK_FALSE(
        vx::segment_enters(block, {.x = -1.0, .y = 0.5, .z = 1.5}, {.x = 2.0, .y = 0.5, .z = 1.5}));
    CHECK_FALSE(
        vx::segment_enters(block, {.x = -1.0, .y = 1.2, .z = 0.5}, {.x = 2.0, .y = 1.2, .z = 0.5}));
    CHECK_FALSE(
        vx::segment_enters(block, {.x = -1.0, .y = 0.5, .z = 0.9}, {.x = 0.9, .y = 0.5, .z = 2.8}));
}

TEST_CASE("a segment along a face or an edge does not enter the box") {
    CHECK_FALSE(
        vx::segment_enters(block, {.x = -1.0, .y = 1.0, .z = 0.5}, {.x = 2.0, .y = 1.0, .z = 0.5}));
    CHECK_FALSE(
        vx::segment_enters(block, {.x = -1.0, .y = 1.0, .z = 1.0}, {.x = 2.0, .y = 1.0, .z = 1.0}));
}

TEST_CASE("a segment that starts or ends inside the box enters it in either direction") {
    const Vec3d inside{.x = 0.5, .y = 0.5, .z = 0.5};
    const Vec3d outside{.x = 4.0, .y = 0.5, .z = 0.5};
    CHECK(vx::segment_enters(block, inside, outside));
    CHECK(vx::segment_enters(block, outside, inside));
    CHECK(vx::segment_enters(block, inside, {.x = 0.6, .y = 0.6, .z = 0.6}));
}

TEST_CASE("a segment with a coordinate that is not finite enters nothing") {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    CHECK_FALSE(
        vx::segment_enters(block, {.x = nan, .y = 0.5, .z = 0.5}, {.x = 2.0, .y = 0.5, .z = 0.5}));
    CHECK_FALSE(vx::segment_enters(block, {.x = -1.0, .y = 0.5, .z = 0.5},
                                   {.x = infinity, .y = 0.5, .z = 0.5}));
}
