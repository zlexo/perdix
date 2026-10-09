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
#include <optional>
#include <random>
#include <span>
#include <thread>
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

TEST_CASE("where the noise's cells meet, the ground levels off: the hills have no seams") {
    // Smoothstep weights are flat at a cell's edges. At x = 128k both
    // octaves' cells meet, so the ground there barely moves; halfway, at
    // x = 128k + 64, the broad hills climb their steepest. Linear weights
    // would slope as much at the edges: a seam along every one.
    const PlaceholderGenerator generator(0x5EED, blocks);
    const auto moves = [&](std::int32_t x, std::int32_t z) {
        return generator.height(x - 1, z) != generator.height(x + 1, z);
    };
    std::size_t samples = 0;
    std::size_t edges_moving = 0;
    std::size_t middles_moving = 0;
    for (std::int32_t z = -1'000; z <= 1'000; z += 37) {
        for (std::int32_t x = -1'024; x < 1'024; x += 128) {
            if (generator.height(x - 1, z) == PlaceholderGenerator::spire_height ||
                generator.height(x + 1, z) == PlaceholderGenerator::spire_height) {
                continue;
            }
            ++samples;
            if (moves(x, z)) {
                ++edges_moving;
            }
            if (moves(x + 64, z)) {
                ++middles_moving;
            }
        }
    }
    INFO("samples ", samples, ", moving at the edges ", edges_moving, ", in the middles ",
         middles_moving);
    // Measured: 25 of 876 at the edges and 383 in the middles; linear
    // weights move 240 at the edges.
    CHECK(samples > 800);
    CHECK(edges_moving * 10 < samples);
    CHECK(middles_moving > 4 * edges_moving);
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

TEST_CASE("chunks built on four threads in different orders are byte for byte the same") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    // The surface's chunks around a spire, and some below and above it.
    std::vector<ChunkPos> positions;
    for (std::int32_t y = -1; y <= 4; ++y) {
        for (std::int32_t z = -1; z <= 1; ++z) {
            for (std::int32_t x = -1; x <= 1; ++x) {
                positions.push_back({.x = x, .y = y, .z = z});
            }
        }
    }
    vx::GenScratch scratch;
    std::vector<std::vector<BlockState>> expected;
    expected.reserve(positions.size());
    for (const ChunkPos pos : positions) {
        expected.push_back(dense(build(generator, pos, scratch)));
    }
    std::vector<std::vector<std::vector<BlockState>>> results(4);
    std::vector<std::thread> threads;
    for (std::size_t thread = 0; thread < results.size(); ++thread) {
        threads.emplace_back([&, thread] {
            std::vector<std::size_t> order(positions.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                order[i] = i;
            }
            std::mt19937 random(static_cast<std::uint32_t>(thread) + 1);
            std::ranges::shuffle(order, random);
            vx::GenScratch own;
            std::vector<std::vector<BlockState>>& mine = results[thread];
            mine.resize(positions.size());
            for (const std::size_t i : order) {
                mine[i] = dense(build(generator, positions[i], own));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::size_t different = 0;
    for (const std::vector<std::vector<BlockState>>& mine : results) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            if (mine[i] != expected[i]) {
                ++different;
            }
        }
    }
    CHECK(different == 0);
}

TEST_CASE("whatever out_of_reach answers, a full build agrees with") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    // Fixed columns, two of them with spires, and one whose ground dips into
    // chunk 0, where stone, dirt and grass all meet the out-of-reach bounds.
    std::vector<ColumnPos> columns = {ColumnPos{.x = 0, .z = 0}, ColumnPos{.x = 3, .z = -2},
                                      ColumnPos{.x = -16, .z = 16}, ColumnPos{.x = 17, .z = 5}};
    for (std::int32_t x = 0; x < 400; ++x) {
        if (generator.surface_range({.x = x, .z = 1}, scratch).min_y == 0) {
            columns.push_back({.x = x, .z = 1});
            break;
        }
    }
    REQUIRE(columns.size() == 5);
    std::size_t answered = 0;
    for (const ColumnPos column : columns) {
        for (std::int32_t y = -3; y <= 22; ++y) {
            const ChunkPos pos{.x = column.x, .y = y, .z = column.z};
            const std::optional<BlockState> uniform = generator.out_of_reach(pos);
            if (!uniform) {
                continue;
            }
            ++answered;
            INFO("chunk ", pos.x, ", ", pos.y, ", ", pos.z);
            const vx::ChunkStorage built = build(generator, pos, scratch);
            REQUIRE(built.is_uniform());
            CHECK(built.get(0) == *uniform);
        }
    }
    CHECK(answered > 50);
    // Plainly above the hills, air; plainly under them, stone.
    CHECK(generator.out_of_reach({.x = 3, .y = 4, .z = 3}) == BlockState::air);
    CHECK(generator.out_of_reach({.x = 3, .y = -1, .z = 3}) == blocks.stone);
    // A spire's column is in reach up to 640, a chunk border, and no further;
    // a column beside it only to the hills' top.
    CHECK_FALSE(generator.out_of_reach({.x = 0, .y = 19, .z = 0}).has_value());
    CHECK(generator.out_of_reach({.x = 0, .y = 20, .z = 0}) == BlockState::air);
    CHECK(generator.out_of_reach({.x = 0, .y = 4, .z = 3}) == BlockState::air);
}

TEST_CASE("no surface lies outside a column's surface range, and nothing solid above it") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    std::mt19937 random(9);
    std::uniform_int_distribution<std::int32_t> place(-200, 200);
    // Random columns, and two with spires, whose range reaches chunk 19.
    std::vector<ColumnPos> columns = {ColumnPos{.x = 0, .z = 0}, ColumnPos{.x = -16, .z = 16}};
    for (int trial = 0; trial < 12; ++trial) {
        columns.push_back({.x = place(random), .z = place(random)});
    }
    for (const ColumnPos column : columns) {
        const ChunkRange range = generator.surface_range(column, scratch);
        INFO("column ", column.x, ", ", column.z);
        REQUIRE_FALSE(range.empty());
        std::int32_t lowest_top = 1'000;
        std::int32_t highest_top = -1'000;
        for (std::int32_t z = 0; z < vx::chunk_size; ++z) {
            for (std::int32_t x = 0; x < vx::chunk_size; ++x) {
                const std::int32_t top = generator.height((column.x * vx::chunk_size) + x,
                                                          (column.z * vx::chunk_size) + z) -
                                         1;
                lowest_top = std::min(lowest_top, top);
                highest_top = std::max(highest_top, top);
            }
        }
        // Exact: the chunks of the lowest and the highest top block.
        CHECK(range.min_y == lowest_top >> vx::chunk_bits);
        CHECK(range.max_y == highest_top >> vx::chunk_bits);
        const vx::ChunkStorage above =
            build(generator, {.x = column.x, .y = range.max_y + 1, .z = column.z}, scratch);
        REQUIRE(above.is_uniform());
        CHECK(above.get(0) == BlockState::air);
    }
}

TEST_CASE("in every block column of a built chunk, the ground is where height() puts it") {
    // The single-column checks above pass on a chunk built transposed, or
    // from the wrong block column, wherever two heights happen to agree.
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    for (const ColumnPos column :
         {ColumnPos{.x = 3, .z = 1}, ColumnPos{.x = -5, .z = -7}, ColumnPos{.x = 0, .z = 0}}) {
        const ChunkRange range = generator.surface_range(column, scratch);
        std::size_t wrong = 0;
        for (std::int32_t chunk_y = range.min_y; chunk_y <= range.max_y; ++chunk_y) {
            const std::vector<BlockState> built =
                dense(build(generator, {.x = column.x, .y = chunk_y, .z = column.z}, scratch));
            for (std::int32_t z = 0; z < vx::chunk_size; ++z) {
                for (std::int32_t x = 0; x < vx::chunk_size; ++x) {
                    // The local y of the air on the ground; the top solid
                    // block is under it.
                    const std::int32_t air = generator.height((column.x * vx::chunk_size) + x,
                                                              (column.z * vx::chunk_size) + z) -
                                             (chunk_y * vx::chunk_size);
                    if (air - 1 >= 0 && air - 1 < vx::chunk_size) {
                        if (built[vx::local_index(x, air - 1, z)] == BlockState::air) {
                            ++wrong;
                        }
                    }
                    if (air >= 0 && air < vx::chunk_size) {
                        if (built[vx::local_index(x, air, z)] != BlockState::air) {
                            ++wrong;
                        }
                    }
                }
            }
        }
        INFO("column ", column.x, ", ", column.z);
        CHECK(wrong == 0);
    }
}

TEST_CASE("a build ignores its neighbors' plans, and a seed makes its own world") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    const ChunkPos pos{.x = 2, .y = 1, .z = -3};
    std::array<vx::ChunkPlan, 27> all{};
    std::array<const vx::ChunkPlan*, 27> plans{};
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = generator.plan(pos, scratch);
        plans[i] = &all[i];
    }
    CHECK(dense(generator.build(plans, scratch)) == dense(build(generator, pos, scratch)));
    const PlaceholderGenerator same(0x5EED, blocks);
    const PlaceholderGenerator other(0x5EED + 1, blocks);
    std::size_t differ = 0;
    for (std::int32_t x = 20; x < 500; x += 13) {
        CHECK(same.height(x, 77) == generator.height(x, 77));
        if (other.height(x, 77) != generator.height(x, 77)) {
            ++differ;
        }
    }
    CHECK(differ > 10);
}
