#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

// RFC 0006's placeholder generator: hills, layers and spires, the same on
// every thread, with exact surface ranges and sound out-of-reach answers.

namespace {

using vx::BlockState;
using vx::ChunkPos;
using vx::ChunkRange;
using vx::ColumnPos;
using vx::PlaceholderGenerator;

constexpr BlockState state(int id) {
    return static_cast<BlockState>(static_cast<std::uint16_t>(id));
}

constexpr vx::PlaceholderBlocks blocks{.grass = state(3), .dirt = state(2), .stone = state(1)};

// The chunk at `pos`, built with only its own plan: the placeholder reads no
// neighbor's.
vx::ChunkStorage build(const PlaceholderGenerator& generator, ChunkPos pos,
                       vx::GenScratch& scratch) {
    const vx::ChunkPlan plan = generator.plan(pos, scratch);
    std::array<const vx::ChunkPlan*, 27> plans{};
    plans[13] = &plan;
    return generator.build(plans, scratch);
}

std::vector<BlockState> dense(const vx::ChunkStorage& chunk) {
    std::vector<BlockState> blocks_out(vx::chunk_volume);
    chunk.to_dense(std::span<BlockState, vx::chunk_volume>(blocks_out));
    return blocks_out;
}

} // namespace

TEST_CASE("hills run from 16 to 111 blocks high, with a spire up to 640 every 512 blocks") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    std::int32_t lowest = 1'000;
    std::int32_t highest = 0;
    for (std::int32_t z = -3'000; z <= 3'000; z += 7) {
        for (std::int32_t x = -3'000; x <= 3'000; x += 5) {
            const std::int32_t height = generator.height(x, z);
            if (height == PlaceholderGenerator::spire_height) {
                continue;
            }
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }
    CHECK(lowest >= 16);
    CHECK(highest <= 111);
    // Rolling, not flat: most of the range is used.
    CHECK(highest - lowest > 50);
    // The spires: 16 by 16, every 512 blocks on both axes, negative included.
    CHECK(generator.height(0, 0) == 640);
    CHECK(generator.height(15, 15) == 640);
    CHECK(generator.height(-512, 1'024 + 15) == 640);
    CHECK(generator.height(16, 0) < 112);
    CHECK(generator.height(-1, 0) < 112);
}

TEST_CASE("the hills roll: neighboring columns differ by at most three blocks, across zero too") {
    // Value noise is smooth: a slope of at most about two blocks a block. A
    // cell boundary misplaced for negative coordinates would leave a cliff.
    const PlaceholderGenerator generator(0x5EED, blocks);
    std::int32_t steepest = 0;
    std::int32_t steepest_south = 0;
    for (std::int32_t z = -300; z <= 300; z += 3) {
        for (std::int32_t x = -300; x <= 300; ++x) {
            const std::int32_t here = generator.height(x, z);
            const std::int32_t east = generator.height(x + 1, z);
            const std::int32_t south = generator.height(x, z + 1);
            if (here == PlaceholderGenerator::spire_height ||
                east == PlaceholderGenerator::spire_height ||
                south == PlaceholderGenerator::spire_height) {
                continue;
            }
            steepest = std::max({steepest, std::abs(east - here), std::abs(south - here)});
            steepest_south = std::max(steepest_south, std::abs(south - here));
        }
    }
    // Along z too: noise that lost its z would lie flat that way.
    CHECK(steepest_south >= 1);
    CHECK(steepest <= 3);
    CHECK(steepest >= 1);
}

TEST_CASE("the ground is grass over three blocks of dirt over stone, and a spire stone to its "
          "top") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    // Block column (100, 37) and the chunk holding its surface.
    const std::int32_t height = generator.height(100, 37);
    const ChunkPos chunk{
        .x = 100 >> vx::chunk_bits, .y = (height - 1) >> vx::chunk_bits, .z = 37 >> vx::chunk_bits};
    const std::vector<BlockState> column_blocks = dense(build(generator, chunk, scratch));
    const auto at = [&](std::int32_t y) {
        const std::int32_t local_y = y - (chunk.y * vx::chunk_size);
        return column_blocks[vx::local_index(100 % 32, local_y, 37 % 32)];
    };
    CHECK(at(height - 1) == blocks.grass);
    if (((height - 1) & 31) >= 4) {
        CHECK(at(height - 2) == blocks.dirt);
        CHECK(at(height - 4) == blocks.dirt);
        CHECK(at(height - 5) == blocks.stone);
    }
    if (((height - 1) & 31) < 31) {
        CHECK(at(height) == BlockState::air);
    }
    // A spire's top is stone, with air above it.
    const std::vector<BlockState> spire_top =
        dense(build(generator, {.x = 0, .y = 19, .z = 0}, scratch));
    CHECK(spire_top[vx::local_index(3, 31, 3)] == blocks.stone);
    const std::vector<BlockState> above_spire =
        dense(build(generator, {.x = 0, .y = 20, .z = 0}, scratch));
    CHECK(std::ranges::all_of(above_spire,
                              [](BlockState block) { return block == BlockState::air; }));
}
