#include "../benchmarks.hpp"

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <nanobench.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace vx::bench {
namespace {

constexpr BlockState state(int id) {
    return static_cast<BlockState>(static_cast<std::uint16_t>(id));
}

// A chunk like layered terrain, where RFC 0002 sets its budget: stone with
// six ores scattered through it, then dirt, a layer of grass and air. Ten
// states, so 4 bits a block.
ChunkStorage terrain_chunk(std::mt19937& generator) {
    std::uniform_int_distribution<int> roll(0, 99);
    std::uniform_int_distribution<int> ore(4, 9);
    ChunkStorage chunk;
    for (int y = 0; y < chunk_size; ++y) {
        for (int z = 0; z < chunk_size; ++z) {
            for (int x = 0; x < chunk_size; ++x) {
                BlockState block = BlockState::air;
                if (y < 20) {
                    block = roll(generator) < 5 ? state(ore(generator)) : state(1);
                } else if (y < 24) {
                    block = state(2);
                } else if (y == 24) {
                    block = state(3);
                }
                chunk.set(local_index(x, y, z), block);
            }
        }
    }
    return chunk;
}

// A chunk holding 200 states, so 8 bits a block, and a palette that set
// scans for 100 entries on average.
ChunkStorage mixed_chunk(std::mt19937& generator) {
    std::uniform_int_distribution<int> pick(0, 199);
    ChunkStorage chunk;
    for (int at = 0; at < chunk_volume; ++at) {
        chunk.set(static_cast<std::uint16_t>(at), state(pick(generator)));
    }
    return chunk;
}

} // namespace

void chunk_storage(ankerl::nanobench::Bench& bench) {
    std::mt19937 generator(11);
    ChunkStorage terrain = terrain_chunk(generator);
    ChunkStorage mixed = mixed_chunk(generator);
    std::uniform_int_distribution<int> place(0, chunk_volume - 1);
    std::vector<std::uint16_t> indices(4'096);
    for (std::uint16_t& at : indices) {
        at = static_cast<std::uint16_t>(place(generator));
    }

    // Throughput: reads at independent random places, which the CPU can
    // overlap, as RFC 0002 budgets them.
    bench.batch(indices.size()).unit("get").run("chunk, 4 bits: get, throughput", [&] {
        std::uint32_t sum = 0;
        for (const std::uint16_t at : indices) {
            sum += static_cast<std::uint32_t>(terrain.get(at));
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // Latency: each read's place depends on the one before. The random part
    // of each place comes from the index list, so only the read and an add
    // stand between one read and the next: a multiply there would be timed
    // as part of the read.
    bench.batch(indices.size()).unit("get").run("chunk, 4 bits: get, dependent latency", [&] {
        std::uint32_t at = 0;
        for (const std::uint16_t offset : indices) {
            const auto read =
                static_cast<std::uint32_t>(terrain.get(static_cast<std::uint16_t>(at)));
            at = (std::uint32_t{offset} + read) & 0x7FFFU;
        }
        ankerl::nanobench::doNotOptimizeAway(at);
    });

    // Writes of states the palette holds, so it never grows: each scans the
    // ten entries for its own.
    bench.batch(indices.size()).unit("set").run("chunk, 4 bits: set, no growth", [&] {
        int next = 0;
        for (const std::uint16_t at : indices) {
            terrain.set(at, state(next));
            next = next == 9 ? 0 : next + 1;
        }
        ankerl::nanobench::doNotOptimizeAway(terrain);
    });

    bench.batch(indices.size()).unit("set").run("chunk, 8 bits, 200 states: set", [&] {
        int next = 0;
        for (const std::uint16_t at : indices) {
            mixed.set(at, state(next));
            next = next == 199 ? 0 : next + 1;
        }
        ankerl::nanobench::doNotOptimizeAway(mixed);
    });
}

} // namespace vx::bench
