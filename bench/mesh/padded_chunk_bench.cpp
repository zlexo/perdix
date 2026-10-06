#include "../benchmarks.hpp"
#include "test_world.hpp"

#include <vx/mesh/padded_chunk.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <nanobench.h>

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace vx::bench {

void padded_chunk(ankerl::nanobench::Bench& bench) {
    // The 27 chunks around the origin's, of rolling terrain, stored as
    // generation leaves them: the one below all stone and ores, the one
    // above all air, the middle row the surface.
    PaletteScratch scratch;
    std::vector<ChunkStorage> chunks;
    auto dense = std::make_unique<std::array<BlockState, chunk_volume>>();
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                for (int y = 0; y < chunk_size; ++y) {
                    for (int z = 0; z < chunk_size; ++z) {
                        for (int x = 0; x < chunk_size; ++x) {
                            (*dense)[local_index(x, y, z)] =
                                world::terrain((dx * chunk_size) + x, (dy * chunk_size) + y,
                                               (dz * chunk_size) + z);
                        }
                    }
                }
                chunks.push_back(ChunkStorage::from_dense(*dense, scratch));
            }
        }
    }
    const auto at = [&](int dx, int dy, int dz) -> const ChunkStorage& {
        return chunks[static_cast<std::size_t>((dx + 1) + (3 * (dz + 1)) + (9 * (dy + 1)))];
    };
    ChunkNeighborhood neighborhood(at(0, 0, 0));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    neighborhood.set(dx, dy, dz, &at(dx, dy, dz));
                }
            }
        }
    }
    auto padded = std::make_unique<PaddedChunk>();
    bench.batch(1).unit("chunk").run("padded chunk: assemble from 27 chunks of terrain", [&] {
        assemble_padded(neighborhood, *padded);
        ankerl::nanobench::doNotOptimizeAway(padded->blocks[padded_index(17, 17, 17)]);
    });
}

} // namespace vx::bench
