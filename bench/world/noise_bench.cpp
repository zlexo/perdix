#include "../benchmarks.hpp"

#include <vx/core/random.hpp>
#include <vx/world/noise.hpp>

#include <nanobench.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx::bench {
namespace {

// Places spread over a million blocks each way, from a fixed stream, so
// every run times the same points.
struct Place {
    double x;
    double y;
    double z;
};

std::vector<Place> places(std::size_t count, double reach) {
    Rng rng{stream_seed(0x5EED, salt("bench.noise"))};
    std::vector<Place> result(count);
    for (Place& place : result) {
        place = {.x = (static_cast<double>(rng.next_float()) * 2 - 1) * reach,
                 .y = (static_cast<double>(rng.next_float()) * 2 - 1) * 256,
                 .z = (static_cast<double>(rng.next_float()) * 2 - 1) * reach};
    }
    return result;
}

} // namespace

// RFC 0008's budget: 4-octave 3D noise in 25 ns a sample on the reference
// machine, release build, one thread. This is the scalar reference; the
// 4-wide path is RFC 0008's item 4.
void noise(ankerl::nanobench::Bench& bench) {
    const std::vector<Place> points = places(4096, 1'000'000);
    bench.batch(points.size()).unit("sample").run("noise: simplex3, one octave", [&] {
        float sum = 0;
        for (const Place& point : points) {
            sum += simplex3(0x5EED, point.x * 0.0123, point.y * 0.0123, point.z * 0.0123);
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
    });
    bench.batch(points.size()).unit("sample").run("noise: simplex2, one octave", [&] {
        float sum = 0;
        for (const Place& point : points) {
            sum += simplex2(0x5EED, point.x * 0.0123, point.z * 0.0123);
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // The RFC's measure: 4-octave 3D fBm over 64 chunk-sized grids, a block
    // apart; one grid a run, in turn.
    const Fbm3 base{0x5EED, 1.0 / 64, 4};
    const std::vector<Place> grids = places(64, 1'000'000);
    std::vector<float> grid(std::size_t{32} * 32 * 32);
    std::size_t next_grid = 0;
    bench.batch(grid.size()).unit("sample").run("noise: 3D fBm, 4 octaves, chunk grids", [&] {
        const Place& at = grids[next_grid++ % grids.size()];
        base.fill({.x = at.x, .y = at.y, .z = at.z, .nx = 32, .ny = 32, .nz = 32}, grid);
        ankerl::nanobench::doNotOptimizeAway(grid.front());
    });

    // What density will ask: a chunk's 9 by 6 by 9 corners, 4 by 8 by 4
    // blocks apart, over 256 chunks in turn.
    const std::vector<Place> chunks = places(256, 1'000'000);
    std::array<float, 9 * 6 * 9> density{};
    std::size_t next_chunk = 0;
    bench.batch(density.size())
        .unit("sample")
        .run("noise: 3D fBm, 4 octaves, a chunk's corners", [&] {
            const Place& at = chunks[next_chunk++ % chunks.size()];
            base.fill({.x = at.x,
                       .y = at.y,
                       .z = at.z,
                       .step_x = 4,
                       .step_y = 8,
                       .step_z = 4,
                       .nx = 9,
                       .ny = 6,
                       .nz = 9},
                      density);
            ankerl::nanobench::doNotOptimizeAway(density.front());
        });

    // Climate: a column of chunks' 9 by 9 lattice columns, 4 blocks apart.
    const Fbm2 climate{0x5EED, 1.0 / 256, 4};
    std::array<float, 9 * 9> columns{};
    bench.batch(columns.size())
        .unit("sample")
        .run("noise: 2D fBm, 4 octaves, a column's lattice", [&] {
            const Place& at = chunks[next_chunk++ % chunks.size()];
            climate.fill({.x = at.x, .z = at.z, .step_x = 4, .step_z = 4, .nx = 9, .nz = 9},
                         columns);
            ankerl::nanobench::doNotOptimizeAway(columns.front());
        });
}

} // namespace vx::bench
