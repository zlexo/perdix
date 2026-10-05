#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>

TEST_CASE("a chunk is 32 blocks a side") {
    CHECK(vx::chunk_size == 32);
    CHECK(vx::chunk_volume == 32'768);
}

TEST_CASE("blocks below zero belong to chunks below zero, counted from their far end") {
    // RFC 0002's test plan, on x; y and z follow the same arithmetic.
    CHECK(vx::chunk_of({.x = -1, .y = 0, .z = 0}).x == -1);
    CHECK(vx::local_of({.x = -1, .y = 0, .z = 0}).x == 31);
    CHECK(vx::chunk_of({.x = -32, .y = 0, .z = 0}).x == -1);
    CHECK(vx::local_of({.x = -32, .y = 0, .z = 0}).x == 0);
    CHECK(vx::chunk_of({.x = -33, .y = 0, .z = 0}).x == -2);
    CHECK(vx::local_of({.x = -33, .y = 0, .z = 0}).x == 31);
    // And on the other two axes, in one block.
    const vx::BlockPos block = {.x = 31, .y = -1, .z = -33};
    CHECK(vx::chunk_of(block) == vx::ChunkPos{.x = 0, .y = -1, .z = -2});
    CHECK(vx::local_of(block) == vx::LocalPos{.x = 31, .y = 31, .z = 31});
    CHECK(vx::chunk_of({.x = 32, .y = 32, .z = 32}) == vx::ChunkPos{.x = 1, .y = 1, .z = 1});
}

TEST_CASE("a block's chunk and its place in it give the block back, at the ends of the range") {
    constexpr std::int32_t lowest = std::numeric_limits<std::int32_t>::min();
    constexpr std::int32_t highest = std::numeric_limits<std::int32_t>::max();
    for (const std::int32_t coordinate :
         {lowest, lowest + 1, lowest + 31, lowest + 32, -33, -32, -1, 0, 1, 31, 32, highest - 32,
          highest - 31, highest - 1, highest}) {
        const vx::BlockPos block = {.x = coordinate, .y = coordinate, .z = coordinate};
        INFO("coordinate ", coordinate);
        CHECK(vx::block_at(vx::chunk_of(block), vx::local_of(block)) == block);
    }
    CHECK(vx::chunk_of({.x = lowest, .y = 0, .z = 0}).x == lowest / 32);
    CHECK(vx::chunk_of({.x = highest, .y = 0, .z = 0}).x == highest / 32);
}

TEST_CASE("each axis keeps its own value through chunk_of, local_of and block_at") {
    // Different values on each axis, so reading one axis for another shows.
    const vx::BlockPos block = {.x = 35, .y = -27, .z = -57};
    CHECK(vx::chunk_of(block) == vx::ChunkPos{.x = 1, .y = -1, .z = -2});
    CHECK(vx::local_of(block) == vx::LocalPos{.x = 3, .y = 5, .z = 7});
    CHECK(vx::block_at({.x = 1, .y = -1, .z = -2}, {.x = 3, .y = 5, .z = 7}) == block);
}

TEST_CASE("in a chunk's arrays, x neighbors are 1 apart, z neighbors 32 and y neighbors 1,024") {
    CHECK(vx::local_index(0, 0, 0) == 0);
    CHECK(vx::local_index(1, 0, 0) - vx::local_index(0, 0, 0) == 1);
    CHECK(vx::local_index(0, 0, 1) - vx::local_index(0, 0, 0) == 32);
    CHECK(vx::local_index(0, 1, 0) - vx::local_index(0, 0, 0) == 1'024);
    CHECK(vx::local_index(5, 7, 9) + 1 == vx::local_index(6, 7, 9));
    // The last block is the last entry, so the indices fill the chunk.
    CHECK(vx::local_index(31, 31, 31) == vx::chunk_volume - 1);
}

TEST_CASE("the six faces come in RFC 0003's order") {
    CHECK(static_cast<int>(vx::Face::pos_x) == 0);
    CHECK(static_cast<int>(vx::Face::neg_x) == 1);
    CHECK(static_cast<int>(vx::Face::pos_y) == 2);
    CHECK(static_cast<int>(vx::Face::neg_y) == 3);
    CHECK(static_cast<int>(vx::Face::pos_z) == 4);
    CHECK(static_cast<int>(vx::Face::neg_z) == 5);
}
