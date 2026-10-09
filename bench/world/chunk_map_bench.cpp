#include "../benchmarks.hpp"

#include <vx/world/chunk_map.hpp>
#include <vx/world/coordinates.hpp>

#include <nanobench.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace vx::bench {
namespace {

// A region about as large as RFC 0006 loads at a 32-chunk view distance:
// the 3,409 columns within 33 chunks of the origin, 5 chunks deep, 17,045
// chunks in a map of 65,536 slots.
std::vector<ChunkPos> region() {
    std::vector<ChunkPos> positions;
    for (std::int32_t x = -33; x <= 33; ++x) {
        for (std::int32_t z = -33; z <= 33; ++z) {
            if ((x * x) + (z * z) > 33 * 33) {
                continue;
            }
            for (std::int32_t y = 0; y < 5; ++y) {
                positions.push_back({.x = x, .y = y, .z = z});
            }
        }
    }
    return positions;
}

} // namespace

void chunk_map(ankerl::nanobench::Bench& bench) {
    std::mt19937 generator(13);
    std::vector<ChunkPos> keys = region();
    std::ranges::shuffle(keys, generator);
    // Each position's record names the next one to look up, so a chain of
    // finds visits the region in a random order.
    ChunkMap map;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        map.insert(keys[i], static_cast<std::uint32_t>((i + 1) % keys.size()));
    }
    const std::vector<ChunkPos> hits(keys.begin(), keys.begin() + 4'096);
    // The same columns above the region: never mapped, but hashed all over
    // the table.
    std::vector<ChunkPos> misses = hits;
    for (ChunkPos& pos : misses) {
        pos.y += 100;
    }

    // Throughput: independent finds, which the CPU can overlap, as the world
    // thread's 26 neighbor lookups for a chunk changing state are.
    bench.batch(hits.size())
        .unit("find")
        .run("chunk map, 17,045 chunks: find, hit, throughput", [&] {
            std::uint32_t sum = 0;
            for (const ChunkPos pos : hits) {
                sum += map.find(pos).value_or(0);
            }
            ankerl::nanobench::doNotOptimizeAway(sum);
        });

    // Latency: each find's position comes from the record the one before
    // found, so none overlap: RFC 0006's budget, under 20 ns.
    bench.batch(hits.size()).unit("find").run("chunk map, 17,045 chunks: find, hit, latency", [&] {
        std::uint32_t at = 0;
        for (std::size_t i = 0; i < hits.size(); ++i) {
            at = map.find(keys[at]).value_or(0);
        }
        ankerl::nanobench::doNotOptimizeAway(at);
    });

    // Independent misses too, which the CPU can overlap.
    bench.batch(misses.size())
        .unit("find")
        .run("chunk map, 17,045 chunks: find, miss, throughput", [&] {
            std::size_t found = 0;
            for (const ChunkPos pos : misses) {
                found += map.find(pos).has_value() ? 1 : 0;
            }
            ankerl::nanobench::doNotOptimizeAway(found);
        });

    // A chunk leaving the region and one entering it: an erase, which shifts
    // the rest of its run back, and an insert. Each hit goes back with its own
    // record, so the map stays as built: with every record 0, a latency run
    // after this one would time one find over and over.
    bench.batch(hits.size())
        .unit("erase and insert")
        .run("chunk map, 17,045 chunks: erase and insert", [&] {
            for (std::size_t i = 0; i < hits.size(); ++i) {
                map.erase(hits[i]);
                map.insert(hits[i], static_cast<std::uint32_t>(i + 1));
            }
        });
}

} // namespace vx::bench
