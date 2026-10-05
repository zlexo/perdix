#include "../benchmarks.hpp"

#include <vx/world/coordinates.hpp>
#include <vx/world/packed_array.hpp>

#include <nanobench.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace vx::bench {
namespace {

// RFC 0002 budgets its operations in a 4-bit chunk: up to 16 states, as in
// layered terrain.
constexpr int bits = 4;

// A 4-bit array of seeded random entries, and indices to read it at.
struct Fixture {
    PackedArray array{bits};
    std::vector<std::uint16_t> indices;

    Fixture() {
        std::mt19937 generator(7);
        std::uniform_int_distribution<std::uint32_t> entry(0, (1U << bits) - 1);
        std::uniform_int_distribution<int> index(0, chunk_volume - 1);
        for (int at = 0; at < chunk_volume; ++at) {
            array.set(static_cast<std::uint16_t>(at), entry(generator));
        }
        indices.resize(4'096);
        for (std::uint16_t& at : indices) {
            at = static_cast<std::uint16_t>(index(generator));
        }
    }
};

} // namespace

void packed_array(ankerl::nanobench::Bench& bench) {
    Fixture fixture;
    const std::vector<std::uint16_t>& indices = fixture.indices;

    // Throughput: reads at independent random places, which the CPU can
    // overlap, as a mesher or a save reading many blocks does.
    bench.batch(indices.size()).unit("get").run("packed array, 4 bits: get, throughput", [&] {
        std::uint32_t sum = 0;
        for (const std::uint16_t at : indices) {
            sum += fixture.array.get(at);
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // Latency: each read's place depends on the one before, so none overlap.
    // The random part of each place comes from the index list, so only the
    // read and an add stand between one read and the next: a multiply there
    // would be timed as part of the read.
    bench.batch(indices.size())
        .unit("get")
        .run("packed array, 4 bits: get, dependent latency", [&] {
            std::uint32_t at = 0;
            for (const std::uint16_t offset : indices) {
                at = (std::uint32_t{offset} + fixture.array.get(static_cast<std::uint16_t>(at))) &
                     0x7FFFU;
            }
            ankerl::nanobench::doNotOptimizeAway(at);
        });

    bench.batch(indices.size()).unit("set").run("packed array, 4 bits: set", [&] {
        std::uint32_t value = 0;
        for (const std::uint16_t at : indices) {
            fixture.array.set(at, value);
            value = (value + 1) & 0xFU;
        }
        ankerl::nanobench::doNotOptimizeAway(fixture.array);
    });

    // A palette outgrowing 4 bits: the whole chunk moves to 8.
    bench.batch(1).unit("chunk").run("packed array: repack 4 to 8 bits", [&] {
        PackedArray wider = fixture.array.repacked(8);
        ankerl::nanobench::doNotOptimizeAway(wider);
    });
}

} // namespace vx::bench
