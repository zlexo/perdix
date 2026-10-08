#include <vx/mesh/padded_chunk.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace {

using WorldBlocks = std::function<vx::BlockState(int x, int y, int z)>;

constexpr vx::BlockState state(int id) {
    return static_cast<vx::BlockState>(static_cast<std::uint16_t>(id));
}

// Inside the padded chunk's reach, every place gets a block of its own:
// 1 plus its index there, so a block read from the wrong place, the wrong
// neighbor or the wrong axis shows. Coordinates are blocks from the middle
// chunk's lowest one. Further out, where the padded chunk never reads, one
// block for all.
vx::BlockState distinct_blocks(int x, int y, int z) {
    const auto inside = [](int at) { return at >= -1 && at <= vx::chunk_size; };
    if (inside(x) && inside(y) && inside(z)) {
        return state(1 + static_cast<int>(vx::padded_index(x + 1, y + 1, z + 1)));
    }
    return state(60'000);
}

// 64 states, so a face neighbor stays paletted at 8 bits and its face
// layer is read through the palette; different steps on each axis, so a
// swap of two axes still shows.
vx::BlockState few_blocks(int x, int y, int z) {
    return state(1 + (((x * 3) + (y * 5) + (z * 7)) & 63));
}

// The 27 chunks around and including the middle one, filled from `blocks`.
std::vector<vx::ChunkStorage> make_chunks(const WorldBlocks& blocks) {
    vx::PaletteScratch scratch;
    std::vector<vx::ChunkStorage> chunks;
    auto dense = std::make_unique<std::array<vx::BlockState, vx::chunk_volume>>();
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                for (int y = 0; y < vx::chunk_size; ++y) {
                    for (int z = 0; z < vx::chunk_size; ++z) {
                        for (int x = 0; x < vx::chunk_size; ++x) {
                            (*dense)[vx::local_index(x, y, z)] =
                                blocks((dx * vx::chunk_size) + x, (dy * vx::chunk_size) + y,
                                       (dz * vx::chunk_size) + z);
                        }
                    }
                }
                chunks.push_back(vx::ChunkStorage::from_dense(*dense, scratch));
            }
        }
    }
    return chunks;
}

// The chunk at offset (dx, dy, dz) in what make_chunks returned.
const vx::ChunkStorage& chunk_at(const std::vector<vx::ChunkStorage>& chunks, int dx, int dy,
                                 int dz) {
    return chunks[static_cast<std::size_t>((dx + 1) + (3 * (dz + 1)) + (9 * (dy + 1)))];
}

// How many places of `padded` differ from `expected`, which gets the
// coordinates of a block relative to the middle chunk's lowest one.
int mismatches(const vx::PaddedChunk& padded, const WorldBlocks& expected) {
    int wrong = 0;
    for (int y = 0; y < vx::padded_size; ++y) {
        for (int z = 0; z < vx::padded_size; ++z) {
            for (int x = 0; x < vx::padded_size; ++x) {
                if (padded.at(x, y, z) != expected(x - 1, y - 1, z - 1)) {
                    if (wrong == 0) {
                        INFO("first at padded (", x, ", ", y, ", ", z, ")");
                        CHECK(padded.at(x, y, z) == expected(x - 1, y - 1, z - 1));
                    }
                    ++wrong;
                }
            }
        }
    }
    return wrong;
}

} // namespace

TEST_CASE("a padded chunk is 34 blocks a side, ordered like a chunk") {
    CHECK(vx::padded_size == 34);
    CHECK(vx::padded_volume == 39'304);
    CHECK(vx::padded_index(1, 0, 0) - vx::padded_index(0, 0, 0) == 1);
    CHECK(vx::padded_index(0, 0, 1) - vx::padded_index(0, 0, 0) == 34);
    CHECK(vx::padded_index(0, 1, 0) - vx::padded_index(0, 0, 0) == 34 * 34);
    CHECK(vx::padded_index(33, 33, 33) == vx::padded_volume - 1);
}

TEST_CASE("every block of a padded chunk comes from the chunk it lies in, at its place there") {
    // Both kinds of storage the faces are read from: direct (more than 256
    // states) and paletted. The edges and corners, with a few states each,
    // are paletted or uniform either way.
    for (const auto& [name, blocks] :
         {std::pair<const char*, WorldBlocks>{"distinct", distinct_blocks},
          std::pair<const char*, WorldBlocks>{"few", few_blocks}}) {
        INFO(name, " blocks");
        const std::vector<vx::ChunkStorage> chunks = make_chunks(blocks);
        vx::ChunkNeighborhood neighborhood(chunk_at(chunks, 0, 0, 0));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx != 0 || dy != 0 || dz != 0) {
                        neighborhood.set(dx, dy, dz, &chunk_at(chunks, dx, dy, dz));
                    }
                }
            }
        }
        auto padded = std::make_unique<vx::PaddedChunk>();
        padded->blocks.fill(state(65'535)); // nothing assembled should be left like this
        vx::assemble_padded(neighborhood, *padded);
        CHECK(mismatches(*padded, blocks) == 0);
    }
    // The distinct blocks leave the middle chunk and the face neighbors
    // direct, so their storage is the one tested above.
    const std::vector<vx::ChunkStorage> chunks = make_chunks(distinct_blocks);
    CHECK(chunk_at(chunks, 0, 0, 0).bits_per_block() == 16);
    CHECK(chunk_at(chunks, 1, 0, 0).bits_per_block() == 16);
    CHECK(make_chunks(few_blocks)[0].bits_per_block() == 8);
}

TEST_CASE("a padded chunk's border is air where a neighbor is missing") {
    const std::vector<vx::ChunkStorage> chunks = make_chunks(distinct_blocks);
    SUBCASE("a lone chunk") {
        auto padded = std::make_unique<vx::PaddedChunk>();
        padded->blocks.fill(state(65'535));
        vx::assemble_padded(vx::ChunkNeighborhood(chunk_at(chunks, 0, 0, 0)), *padded);
        const auto inside = [](int at) { return at >= 0 && at < vx::chunk_size; };
        CHECK(mismatches(*padded, [&](int x, int y, int z) {
                  return inside(x) && inside(y) && inside(z) ? distinct_blocks(x, y, z)
                                                             : vx::BlockState::air;
              }) == 0);
    }
    SUBCASE("one face, one edge and one corner missing") {
        vx::ChunkNeighborhood neighborhood(chunk_at(chunks, 0, 0, 0));
        const auto missing = [](int dx, int dy, int dz) {
            return (dx == 0 && dy == 1 && dz == 0) || (dx == -1 && dy == 0 && dz == 1) ||
                   (dx == 1 && dy == -1 && dz == -1);
        };
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx != 0 || dy != 0 || dz != 0) && !missing(dx, dy, dz)) {
                        neighborhood.set(dx, dy, dz, &chunk_at(chunks, dx, dy, dz));
                    }
                }
            }
        }
        auto padded = std::make_unique<vx::PaddedChunk>();
        padded->blocks.fill(state(65'535));
        vx::assemble_padded(neighborhood, *padded);
        // Which chunk a coordinate relative to the middle chunk lies in.
        const auto offset = [](int at) { return at < 0 ? -1 : (at >= vx::chunk_size ? 1 : 0); };
        CHECK(mismatches(*padded, [&](int x, int y, int z) {
                  return missing(offset(x), offset(y), offset(z)) ? vx::BlockState::air
                                                                  : distinct_blocks(x, y, z);
              }) == 0);
    }
}
