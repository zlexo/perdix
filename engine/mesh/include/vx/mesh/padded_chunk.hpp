#pragma once

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <array>
#include <cassert>
#include <cstddef>

namespace vx {

// The mesher's input (RFC 0003): a chunk's blocks inside a border one block
// deep taken from its 26 neighbors, 34 blocks a side. Faces need the 6 face
// neighbors; ambient occlusion at the chunk's edges and corners needs the 12
// edge and 8 corner neighbors as well.
inline constexpr int padded_size = chunk_size + 2;                            // 34
inline constexpr int padded_volume = padded_size * padded_size * padded_size; // 39,304

// Where block (x, y, z) of a padded chunk is, each from 0 to 33, with the
// chunk's own blocks at 1 to 32: x varies fastest, then z, then y, as in a
// chunk (local_index).
constexpr std::size_t padded_index(int x, int y, int z) noexcept {
    assert(x >= 0 && x < padded_size && y >= 0 && y < padded_size && z >= 0 && z < padded_size);
    constexpr std::size_t side = padded_size;
    return static_cast<std::size_t>(x) + (static_cast<std::size_t>(z) * side) +
           (static_cast<std::size_t>(y) * side * side);
}

// 78,608 bytes: too large for a worker thread's stack, so the mesher's
// caller owns one and reuses it (RFC 0005 keeps one in each worker's scratch).
struct PaddedChunk {
    std::array<BlockState, padded_volume> blocks{};

    [[nodiscard]] BlockState at(int x, int y, int z) const noexcept {
        return blocks[padded_index(x, y, z)];
    }
};

// A chunk and the 26 chunks around it, each found by its offset: -1, 0 or 1
// on each axis. A neighbor left null reads as air (RFC 0003), as for M2's
// lone chunk and in tests; streaming (RFC 0006) meshes a chunk only once all
// 26 are loaded.
class ChunkNeighborhood {
public:
    explicit ChunkNeighborhood(const ChunkStorage& middle) noexcept {
        chunks_[index(0, 0, 0)] = &middle;
    }

    // `chunk` is the neighbor at offset (dx, dy, dz), not the middle.
    void set(int dx, int dy, int dz, const ChunkStorage* chunk) noexcept {
        assert(dx != 0 || dy != 0 || dz != 0);
        chunks_[index(dx, dy, dz)] = chunk;
    }

    [[nodiscard]] const ChunkStorage& middle() const noexcept { return *chunks_[index(0, 0, 0)]; }
    [[nodiscard]] const ChunkStorage* at(int dx, int dy, int dz) const noexcept {
        return chunks_[index(dx, dy, dz)];
    }

private:
    static std::size_t index(int dx, int dy, int dz) noexcept {
        assert(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && dz >= -1 && dz <= 1);
        return static_cast<std::size_t>(dx + 1) + (3 * static_cast<std::size_t>(dz + 1)) +
               (9 * static_cast<std::size_t>(dy + 1));
    }

    std::array<const ChunkStorage*, 27> chunks_{};
};

// Fills all of `out`: the middle chunk in one pass of to_dense, the layer of
// each face neighbor that touches it through copy_face, and the 12 edges and
// 8 corners block by block, 392 reads in all.
void assemble_padded(const ChunkNeighborhood& neighborhood, PaddedChunk& out) noexcept;

// RFC 0011's second border set: the border toward `side` taken from
// `coarser`, the chunk one level coarser across that side, whose cells each
// cover 2 by 2 of the chunk's on the face between them. `offset` is where
// the chunk's face starts on the coarser chunk's face, in its cells, along
// the axes copy_face orders a face by (z then y on an x face, x then z on a
// y face, x then y on a z face): 0 or 16 each. The edges and corners stay.
void coarser_border(const ChunkStorage& coarser, Face side, std::array<int, 2> offset,
                    PaddedChunk& out) noexcept;

// RFC 0011's open border: the border toward `side` all air, as if nothing
// were drawn across it, so mesh_border then makes a face for each opaque
// block of the chunk's outermost slice that way. A coarse chunk draws that
// set toward a side where its own level's neighbor is not drawn, the finer
// chunks there being nearer the camera than the face between them. The
// edges and corners stay.
void open_border(Face side, PaddedChunk& out) noexcept;

} // namespace vx
