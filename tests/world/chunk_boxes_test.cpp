#include <vx/core/vec3d.hpp>
#include <vx/world/block_physics.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <vector>

namespace {

using vx::Box;
using vx::Vec3d;

constexpr vx::BlockState stone = static_cast<vx::BlockState>(1);

Box block(double x, double y, double z) {
    return {.min = {.x = x, .y = y, .z = z}, .max = {.x = x + 1.0, .y = y + 1.0, .z = z + 1.0}};
}

bool holds(const std::vector<Box>& boxes, const Box& box) {
    return std::ranges::find(boxes, box) != boxes.end();
}

} // namespace

TEST_CASE("a chunk's blocks are full cubes, air is nothing, and nothing lies outside it") {
    vx::ChunkStorage chunk;
    chunk.set(vx::local_index(0, 0, 0), stone);
    chunk.set(vx::local_index(31, 5, 7), stone);
    // The chunk at (1, 0, -1): its lowest block is (32, 0, -32).
    const vx::ChunkBoxes boxes(chunk, {.x = 1, .y = 0, .z = -1});
    std::vector<Box> out;
    boxes.collect(
        {.min = {.x = 0.0, .y = -10.0, .z = -64.0}, .max = {.x = 100.0, .y = 100.0, .z = 0.0}},
        out);
    CHECK(out.size() == 2);
    CHECK(holds(out, block(32.0, 0.0, -32.0)));
    CHECK(holds(out, block(63.0, 5.0, -25.0)));
}

TEST_CASE("a region gathers the blocks it overlaps or touches, and no others") {
    // A floor of 3 by 3 blocks at the chunk's bottom.
    vx::ChunkStorage chunk;
    for (int x = 0; x < 3; ++x) {
        for (int z = 0; z < 3; ++z) {
            chunk.set(vx::local_index(x, 0, z), stone);
        }
    }
    const vx::ChunkBoxes boxes(chunk, {});
    std::vector<Box> out;
    // Resting on the floor's top at y = 1, from x = 0.5 to the face between
    // blocks 0 and 1 at x = 1: the block under it and the one past that
    // face, and only those.
    boxes.collect({.min = {.x = 0.5, .y = 1.0, .z = 0.2}, .max = {.x = 1.0, .y = 2.8, .z = 0.8}},
                  out);
    CHECK(out.size() == 2);
    CHECK(holds(out, block(0.0, 0.0, 0.0)));
    CHECK(holds(out, block(1.0, 0.0, 0.0)));
    // Above the floor by a hair, nothing.
    out.clear();
    boxes.collect({.min = {.x = 0.5, .y = 1.001, .z = 0.2}, .max = {.x = 1.5, .y = 2.8, .z = 0.8}},
                  out);
    CHECK(out.empty());
}

TEST_CASE("a region far from the chunk, or not finite, gathers nothing") {
    vx::ChunkStorage chunk;
    chunk.set(vx::local_index(0, 0, 0), stone);
    const vx::ChunkBoxes boxes(chunk, {});
    std::vector<Box> out;
    const double huge = 1e300;
    boxes.collect(
        {.min = {.x = huge, .y = huge, .z = huge}, .max = {.x = huge + 1.0, .y = huge, .z = huge}},
        out);
    boxes.collect(
        {.min = {.x = -huge, .y = -huge, .z = -huge}, .max = {.x = -1.5, .y = -1.5, .z = -1.5}},
        out);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    boxes.collect({.min = {.x = nan, .y = 0.0, .z = 0.0}, .max = {.x = 1.0, .y = 1.0, .z = 1.0}},
                  out);
    CHECK(out.empty());
    // A region spanning everything still holds just the one block.
    boxes.collect(
        {.min = {.x = -huge, .y = -huge, .z = -huge}, .max = {.x = huge, .y = huge, .z = huge}},
        out);
    CHECK(out.size() == 1);
}

TEST_CASE("a chunk above or below the origin keeps its blocks at its own height") {
    vx::ChunkStorage chunk;
    chunk.set(vx::local_index(3, 4, 5), stone);
    // The chunk at (0, -2, 0): its lowest block is (0, -64, 0).
    const vx::ChunkBoxes boxes(chunk, {.x = 0, .y = -2, .z = 0});
    std::vector<Box> out;
    boxes.collect(
        {.min = {.x = 0.0, .y = -100.0, .z = 0.0}, .max = {.x = 32.0, .y = 100.0, .z = 32.0}}, out);
    CHECK(out.size() == 1);
    CHECK(holds(out, block(3.0, -60.0, 5.0)));
}

TEST_CASE("past the chunk's corner, a region gathers the corner block only touching it") {
    vx::ChunkStorage chunk;
    chunk.set(vx::local_index(31, 31, 31), stone);
    const vx::ChunkBoxes boxes(chunk, {});
    std::vector<Box> out;
    // On the corner block's top face, at y = 32.
    boxes.collect(
        {.min = {.x = 31.5, .y = 32.0, .z = 31.5}, .max = {.x = 32.5, .y = 33.0, .z = 32.5}}, out);
    CHECK(out.size() == 1);
    // More than a block above the chunk, over the same column: nothing.
    out.clear();
    boxes.collect(
        {.min = {.x = 31.5, .y = 33.5, .z = 31.5}, .max = {.x = 32.5, .y = 34.0, .z = 32.5}}, out);
    CHECK(out.empty());
}

TEST_CASE("a region with any of its six coordinates not finite gathers nothing") {
    vx::ChunkStorage chunk;
    chunk.set(vx::local_index(0, 0, 0), stone);
    const vx::ChunkBoxes boxes(chunk, {});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<Box> out;
    for (int side = 0; side < 6; ++side) {
        INFO("coordinate ", side);
        Box region = block(0.0, 0.0, 0.0);
        Vec3d& corner = side < 3 ? region.min : region.max;
        if (side % 3 == 0) {
            corner.x = nan;
        } else if (side % 3 == 1) {
            corner.y = nan;
        } else {
            corner.z = nan;
        }
        boxes.collect(region, out);
        CHECK(out.empty());
    }
    // Finite, the same region holds the block.
    boxes.collect(block(0.0, 0.0, 0.0), out);
    CHECK(out.size() == 1);
}

TEST_CASE("a box falls onto a chunk's floor and stands on it") {
    vx::ChunkStorage chunk;
    for (int x = 0; x < 32; ++x) {
        for (int z = 0; z < 32; ++z) {
            chunk.set(vx::local_index(x, 6, z), stone);
        }
    }
    const vx::ChunkBoxes boxes(chunk, {});
    std::vector<Box> scratch;
    const Box falling = {.min = {.x = 15.7, .y = 10.0, .z = 15.7},
                         .max = {.x = 16.3, .y = 11.8, .z = 16.3}};
    const vx::Move move = vx::move_box(falling, {.x = 0.0, .y = -5.0, .z = 0.0}, boxes, scratch);
    CHECK(move.motion.y == -3.0);
    CHECK(move.on_ground);
}

TEST_CASE("a state the physics do not cover is a full cube on default ground") {
    // RFC 0010: a chunk holding a state past the registry's physics, as one
    // from another registry would, still collides with it.
    vx::ChunkStorage chunk;
    chunk.set(vx::local_index(4, 0, 4), stone);
    const Box region{.min = {.x = 4.2, .y = 0.5, .z = 4.2}, .max = {.x = 4.8, .y = 0.9, .z = 4.8}};
    const std::array<vx::BlockPhysics, 1> air_only{};
    const vx::ChunkBoxes past(chunk, {}, air_only);
    std::vector<Box> out;
    past.collect(region, out);
    CHECK(out == std::vector<Box>{block(4.0, 0.0, 4.0)});
    CHECK(past.slipperiness({.x = 4, .y = 0, .z = 4}) == 0.6);
    // Covered, the same state is as its physics say.
    const std::array<vx::BlockPhysics, 2> covered{
        vx::BlockPhysics{},
        vx::BlockPhysics{.collision = vx::Collision::none, .slipperiness = 0.98}};
    const vx::ChunkBoxes read(chunk, {}, covered);
    out.clear();
    read.collect(region, out);
    CHECK(out.empty());
    CHECK(read.slipperiness({.x = 4, .y = 0, .z = 4}) == 0.98);
}

TEST_CASE("one block past each face of the chunk, the slipperiness is default ground") {
    vx::ChunkStorage chunk;
    for (int y = 0; y < vx::chunk_size; ++y) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            for (int x = 0; x < vx::chunk_size; ++x) {
                chunk.set(vx::local_index(x, y, z), stone);
            }
        }
    }
    const std::array<vx::BlockPhysics, 2> icy{
        vx::BlockPhysics{},
        vx::BlockPhysics{.collision = vx::Collision::full, .slipperiness = 0.98}};
    // The chunk at (1, -1, 0): its blocks run from (32, -32, 0) to (63, -1, 31).
    const vx::ChunkBoxes boxes(chunk, {.x = 1, .y = -1, .z = 0}, icy);
    CHECK(boxes.slipperiness({.x = 32, .y = -32, .z = 0}) == 0.98);
    CHECK(boxes.slipperiness({.x = 63, .y = -1, .z = 31}) == 0.98);
    for (const vx::BlockPos past :
         {vx::BlockPos{.x = 31, .y = -32, .z = 0}, vx::BlockPos{.x = 64, .y = -32, .z = 0},
          vx::BlockPos{.x = 32, .y = -33, .z = 0}, vx::BlockPos{.x = 32, .y = 0, .z = 0},
          vx::BlockPos{.x = 32, .y = -32, .z = -1}, vx::BlockPos{.x = 32, .y = -32, .z = 32}}) {
        INFO(past.x, " ", past.y, " ", past.z);
        CHECK(boxes.slipperiness(past) == 0.6);
    }
}
