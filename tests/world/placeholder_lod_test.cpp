#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/lod.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <thread>
#include <vector>

// RFC 0011's coarse levels on the placeholder hills: level 0 by the coarse
// rules is RFC 0006's, the same on every thread, the ground within a cell of
// the full detail it covers, and sound surface ranges and out-of-reach
// answers.

namespace {

using vx::BlockState;
using vx::ChunkPos;
using vx::ColumnPos;
using vx::LodPos;
using vx::PlaceholderGenerator;

constexpr BlockState state(int id) {
    return static_cast<BlockState>(static_cast<std::uint16_t>(id));
}

constexpr vx::PlaceholderBlocks blocks{.grass = state(3), .dirt = state(2), .stone = state(1)};

std::vector<BlockState> dense(const vx::ChunkStorage& chunk) {
    std::vector<BlockState> out(vx::chunk_volume);
    chunk.to_dense(std::span<BlockState, vx::chunk_volume>(out));
    return out;
}

// RFC 0006's chunk at `pos`, built with only its own plan.
vx::ChunkStorage build(const PlaceholderGenerator& generator, ChunkPos pos,
                       vx::GenScratch& scratch) {
    const vx::ChunkPlan plan = generator.plan(pos, scratch);
    std::array<const vx::ChunkPlan*, 27> plans{};
    plans[13] = &plan;
    return generator.build(plans, scratch);
}

bool in_spire(std::int64_t x, std::int64_t z) {
    const auto mod = [](std::int64_t v) {
        return ((v % PlaceholderGenerator::spire_spacing) + PlaceholderGenerator::spire_spacing) %
               PlaceholderGenerator::spire_spacing;
    };
    return mod(x) < PlaceholderGenerator::spire_width && mod(z) < PlaceholderGenerator::spire_width;
}

} // namespace

TEST_CASE("by the coarse rules level 0 is RFC 0006's chunk, block for block") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    CHECK(generator.levels() == vx::lod_levels);
    std::mt19937 random(7);
    std::uniform_int_distribution<std::int32_t> columns(-40'000, 40'000);
    std::vector<ColumnPos> sample{{.x = 0, .z = 0}, {.x = -1, .z = 15}, {.x = 16, .z = -16}};
    for (int i = 0; i < 5; ++i) {
        sample.push_back({.x = columns(random), .z = columns(random)});
    }
    for (const ColumnPos column : sample) {
        CHECK(generator.lod_surface_range(column, 0, scratch) ==
              generator.surface_range(column, scratch));
        for (std::int32_t y = -1; y <= 21; ++y) {
            const ChunkPos pos{.x = column.x, .y = y, .z = column.z};
            const LodPos at{.x = pos.x, .y = pos.y, .z = pos.z, .level = 0};
            CHECK(generator.lod_out_of_reach(at) == generator.out_of_reach(pos));
            CHECK(dense(generator.lod_build(at, scratch)) == dense(build(generator, pos, scratch)));
        }
    }
    for (std::int32_t x = -40; x < 40; x += 7) {
        CHECK(generator.cell_height(x, 3 * x, 0) == generator.height(x, 3 * x));
    }
}

TEST_CASE("coarse chunks are the same on every thread, in any order") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    std::mt19937 random(11);
    std::vector<LodPos> chunks;
    for (std::uint8_t level = 1; level <= vx::max_lod_level; ++level) {
        std::uniform_int_distribution<std::int32_t> columns(-30'000 >> level, 30'000 >> level);
        for (int i = 0; i < 6; ++i) {
            const ColumnPos column{.x = columns(random), .z = columns(random)};
            const vx::ChunkRange range = generator.lod_surface_range(column, level, scratch);
            for (std::int32_t y = range.min_y; y <= range.max_y; ++y) {
                chunks.push_back({.x = column.x, .y = y, .z = column.z, .level = level});
            }
        }
    }
    std::vector<std::vector<BlockState>> made;
    for (const LodPos pos : chunks) {
        made.push_back(dense(generator.lod_build(pos, scratch)));
    }
    std::array<std::vector<bool>, 4> same;
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < same.size(); ++t) {
        same[t].assign(chunks.size(), false);
        threads.emplace_back([&, t] {
            vx::GenScratch own;
            std::vector<std::size_t> order(chunks.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                order[i] = i;
            }
            std::mt19937 shuffle(static_cast<std::mt19937::result_type>(t));
            std::ranges::shuffle(order, shuffle);
            for (const std::size_t i : order) {
                same[t][i] = dense(generator.lod_build(chunks[i], own)) == made[i];
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const std::vector<bool>& each : same) {
        CHECK(std::ranges::all_of(each, [](bool b) { return b; }));
    }
    CHECK(chunks.size() > 30);
}

TEST_CASE("a coarse cell's ground is within a cell of the full-detail ground it covers") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    // std::mt19937's numbers are the same in every standard library, its
    // distributions are not: the samples are taken from the numbers, so that
    // every platform checks the same places.
    std::mt19937 random(13);
    const auto blocks_across = [&random] {
        return static_cast<std::int64_t>(random() % 2'000'001U) - 1'000'000;
    };
    // The largest gap between the top of a coarse cell column as built and
    // the mean of the full-detail heights under it, in cells: within a cell,
    // as RFC 0011 asks. Over 2,000 samples a level it reaches 1.0, 0.80,
    // 0.67, 0.61 and 0.82 cells by level (2026-10-21); bounds measured on a
    // few samples only held those samples, and failed on others.
    for (std::uint8_t level = 1; level <= vx::max_lod_level; ++level) {
        const std::int64_t size = std::int64_t{1} << level;
        double largest = 0.0;
        for (int i = 0; i < 40; ++i) {
            const std::int64_t cell_x = blocks_across() >> level;
            const std::int64_t cell_z = blocks_across() >> level;
            double sum = 0.0;
            bool spire = false;
            for (std::int64_t dz = 0; dz < size; ++dz) {
                for (std::int64_t dx = 0; dx < size; ++dx) {
                    const std::int64_t x = (cell_x * size) + dx;
                    const std::int64_t z = (cell_z * size) + dz;
                    spire = spire || in_spire(x, z);
                    sum += generator.height(static_cast<std::int32_t>(x),
                                            static_cast<std::int32_t>(z));
                }
            }
            if (spire) {
                continue;
            }
            // The cell column's top solid cell, as the chunks are built.
            const ColumnPos column{.x = static_cast<std::int32_t>(cell_x >> vx::chunk_bits),
                                   .z = static_cast<std::int32_t>(cell_z >> vx::chunk_bits)};
            const auto local_x = static_cast<std::int32_t>(cell_x & (vx::chunk_size - 1));
            const auto local_z = static_cast<std::int32_t>(cell_z & (vx::chunk_size - 1));
            const vx::ChunkRange range = generator.lod_surface_range(column, level, scratch);
            std::optional<std::int64_t> top;
            for (std::int32_t y = range.min_y; y <= range.max_y; ++y) {
                const std::vector<BlockState> cells = dense(generator.lod_build(
                    {.x = column.x, .y = y, .z = column.z, .level = level}, scratch));
                for (std::int32_t cy = 0; cy < vx::chunk_size; ++cy) {
                    if (cells[vx::local_index(local_x, cy, local_z)] != BlockState::air) {
                        top = (std::int64_t{y} * vx::chunk_size) + cy;
                    }
                }
            }
            REQUIRE(top.has_value());
            const double mean = sum / static_cast<double>(size * size);
            const auto surface = static_cast<double>((*top + 1) * size);
            largest = std::max(largest, std::abs(surface - mean) / static_cast<double>(size));
        }
        MESSAGE("level " << int{level} << ": a coarse cell's top within " << largest
                         << " cells of the full-detail mean");
        CHECK(largest <= 1.0);
    }
}

TEST_CASE("a coarse level draws no hill smaller than two of its cells, so nothing speckles") {
    // Along rows of level-5 cells, the curvature of the ground, the second
    // difference of neighboring cells' heights, stays within what the broad
    // 128-block hills give. Sampling the 32-block ones there, a cell apart,
    // would alias them into bumps from cell to cell. Measured on 2026-10-19,
    // then held: 16 blocks at most here; with the smaller octave kept, 26.
    const PlaceholderGenerator generator(0x5EED, blocks);
    std::mt19937 random(19);
    std::uniform_int_distribution<std::int64_t> cells(-30'000, 30'000);
    std::int64_t steepest = 0;
    for (int row = 0; row < 40; ++row) {
        const std::int64_t z = cells(random);
        const std::int64_t start = cells(random);
        for (std::int64_t x = start + 1; x < start + 63; ++x) {
            const std::int64_t bend = generator.cell_height(x - 1, z, 5) -
                                      (2 * std::int64_t{generator.cell_height(x, z, 5)}) +
                                      generator.cell_height(x + 1, z, 5);
            steepest = std::max(steepest, std::abs(bend));
        }
    }
    MESSAGE("level 5: the ground bends at most " << steepest << " blocks from cell to cell");
    CHECK(steepest <= 18);
}

TEST_CASE("a coarse surface range lies between solid and air, and chunks out of reach build "
          "to their state") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    std::mt19937 random(17);
    for (std::uint8_t level = 1; level <= vx::max_lod_level; ++level) {
        std::uniform_int_distribution<std::int32_t> columns(-30'000 >> level, 30'000 >> level);
        // The origin's column holds a spire's cells to level 4.
        std::vector<ColumnPos> sample{{.x = 0, .z = 0}};
        for (int i = 0; i < 5; ++i) {
            sample.push_back({.x = columns(random), .z = columns(random)});
        }
        for (const ColumnPos column : sample) {
            const vx::ChunkRange range = generator.lod_surface_range(column, level, scratch);
            REQUIRE_FALSE(range.empty());
            const auto at = [&](std::int32_t y) {
                return LodPos{.x = column.x, .y = y, .z = column.z, .level = level};
            };
            // Under the range every cell is solid; over it, every cell air.
            const std::vector<BlockState> below =
                dense(generator.lod_build(at(range.min_y - 1), scratch));
            const std::vector<BlockState> above =
                dense(generator.lod_build(at(range.max_y + 1), scratch));
            CHECK(std::ranges::none_of(below, [](BlockState s) { return s == BlockState::air; }));
            CHECK(std::ranges::all_of(above, [](BlockState s) { return s == BlockState::air; }));
            // Each cell column's top solid cell lies in the range.
            std::array<std::optional<std::int32_t>, vx::chunk_area> tops{};
            for (std::int32_t y = range.min_y; y <= range.max_y; ++y) {
                const std::vector<BlockState> cells = dense(generator.lod_build(at(y), scratch));
                for (std::int32_t cy = 0; cy < vx::chunk_size; ++cy) {
                    for (std::int32_t cz = 0; cz < vx::chunk_size; ++cz) {
                        for (std::int32_t cx = 0; cx < vx::chunk_size; ++cx) {
                            if (cells[vx::local_index(cx, cy, cz)] != BlockState::air) {
                                tops[static_cast<std::size_t>(cx + (cz * vx::chunk_size))] =
                                    (y * vx::chunk_size) + cy;
                            }
                        }
                    }
                }
            }
            CHECK(std::ranges::all_of(
                tops, [](const std::optional<std::int32_t>& top) { return top.has_value(); }));
            // A chunk out of reach builds to the state it was said to be.
            for (std::int32_t y = range.min_y - 3; y <= range.max_y + 6; ++y) {
                if (const std::optional<BlockState> uniform = generator.lod_out_of_reach(at(y))) {
                    const vx::ChunkStorage chunk = generator.lod_build(at(y), scratch);
                    CHECK(chunk.is_uniform());
                    CHECK(dense(chunk).front() == *uniform);
                }
            }
        }
    }
}

TEST_CASE("grass caps every coarse column, as it caps the full-detail ones") {
    // Each cell column's top solid cell is grass: from afar a hill is as
    // green as up close, wherever its ground falls inside the cell. Its top
    // can lie up to half a cell under the full-detail ground, which is grass
    // there too. The spires' cells are stone.
    const PlaceholderGenerator generator(0x5EED, blocks);
    vx::GenScratch scratch;
    std::mt19937 random(23);
    std::uniform_int_distribution<std::int32_t> chunks_across(-200, 200);
    for (std::uint8_t level = 1; level < vx::lod_levels; ++level) {
        CAPTURE(int{level});
        std::size_t columns = 0;
        std::size_t grass = 0;
        std::size_t grass_under = 0;
        for (int sample = 0; sample < 8; ++sample) {
            const ColumnPos column{.x = chunks_across(random) >> level,
                                   .z = chunks_across(random) >> level};
            const vx::ChunkRange range = generator.lod_surface_range(column, level, scratch);
            // Each cell column's top two solid cells, from the bottom up.
            std::vector<BlockState> top(vx::chunk_size * vx::chunk_size, BlockState::air);
            std::vector<BlockState> under(vx::chunk_size * vx::chunk_size, BlockState::air);
            for (std::int32_t y = range.min_y; y <= range.max_y; ++y) {
                const std::vector<BlockState> cells = dense(generator.lod_build(
                    {.x = column.x, .y = y, .z = column.z, .level = level}, scratch));
                for (std::int32_t z = 0; z < vx::chunk_size; ++z) {
                    for (std::int32_t x = 0; x < vx::chunk_size; ++x) {
                        const auto at = static_cast<std::size_t>(x + (z * vx::chunk_size));
                        for (std::int32_t cy = 0; cy < vx::chunk_size; ++cy) {
                            const BlockState cell = cells[vx::local_index(x, cy, z)];
                            if (cell != BlockState::air) {
                                under[at] = top[at];
                                top[at] = cell;
                            }
                        }
                    }
                }
            }
            for (std::int32_t z = 0; z < vx::chunk_size; ++z) {
                for (std::int32_t x = 0; x < vx::chunk_size; ++x) {
                    const std::int64_t cell_x = (std::int64_t{column.x} * vx::chunk_size) + x;
                    const std::int64_t cell_z = (std::int64_t{column.z} * vx::chunk_size) + z;
                    if (generator.cell_height(cell_x, cell_z, level) ==
                        PlaceholderGenerator::spire_height) {
                        continue;
                    }
                    ++columns;
                    const auto at = static_cast<std::size_t>(x + (z * vx::chunk_size));
                    grass += top[at] == blocks.grass ? 1U : 0U;
                    // Under the grass, dirt or stone, as at level 0.
                    grass_under += under[at] == blocks.grass ? 1U : 0U;
                }
            }
        }
        CHECK(columns > 7'000);
        CHECK(grass == columns);
        CHECK(grass_under == 0);
    }
}

TEST_CASE("the spires are whole cells to level 4 and fall between the cells at 5") {
    const PlaceholderGenerator generator(0x5EED, blocks);
    for (std::uint8_t level = 0; level <= 4; ++level) {
        CHECK(generator.cell_height(0, 0, level) == PlaceholderGenerator::spire_height);
        CHECK(generator.cell_height(512 >> level, 512 >> level, level) ==
              PlaceholderGenerator::spire_height);
    }
    CHECK(generator.cell_height(0, 0, 5) < PlaceholderGenerator::spire_height);
    // And the grass caps a coarse hill.
    vx::GenScratch scratch;
    const ColumnPos column{.x = 3, .z = 5};
    const std::uint8_t level = 2;
    const vx::ChunkRange range = generator.lod_surface_range(column, level, scratch);
    const std::vector<BlockState> cells = dense(generator.lod_build(
        {.x = column.x, .y = range.max_y, .z = column.z, .level = level}, scratch));
    BlockState top = BlockState::air;
    for (std::int32_t cy = vx::chunk_size - 1; cy >= 0 && top == BlockState::air; --cy) {
        top = cells[vx::local_index(0, cy, 0)];
    }
    CHECK((top == blocks.grass || top == BlockState::air));
}
