#include <vx/mesh/padded_chunk.hpp>

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace vx {
namespace {

constexpr int last = chunk_size - 1;      // 31
constexpr int far_side = padded_size - 1; // 33

// Where an offset of -1, 0 or 1 along one axis puts a neighbor's blocks in
// the padded chunk, and which of its own blocks those are. Along an axis the
// neighbor is offset on, the border holds one layer: the neighbor's far
// layer below the chunk, its near layer above it. Along the others it spans
// the whole chunk.
struct Span {
    int padded_first;
    int local_first;
    int count;
};

constexpr Span span_of(int offset) noexcept {
    if (offset < 0) {
        return {.padded_first = 0, .local_first = last, .count = 1};
    }
    if (offset > 0) {
        return {.padded_first = far_side, .local_first = 0, .count = 1};
    }
    return {.padded_first = 1, .local_first = 0, .count = chunk_size};
}

// A face neighbor's layer that touches the middle chunk: the face looking
// back at it.
constexpr Face facing_back(int dx, int dy, int dz) noexcept {
    if (dx != 0) {
        return dx > 0 ? Face::neg_x : Face::pos_x;
    }
    if (dy != 0) {
        return dy > 0 ? Face::neg_y : Face::pos_y;
    }
    return dz > 0 ? Face::neg_z : Face::pos_z;
}

// The 32 by 32 layer copy_face gave (or air), into its place in the border.
// copy_face orders a layer by the other two axes in local_index's order:
// [z + 32 * y] on an x face, [x + 32 * z] on a y face, [x + 32 * y] on a z
// face. Rows along x are consecutive in both, so y and z faces copy a row
// at a time.
void place_face(int dx, int dy, int dz, std::span<const BlockState, chunk_area> layer,
                PaddedChunk& out) noexcept {
    const auto row = [&](int at) {
        return layer.subspan(static_cast<std::size_t>(at) * chunk_size, chunk_size);
    };
    const auto blocks = std::span<BlockState>(out.blocks);
    if (dx != 0) {
        const int x = span_of(dx).padded_first;
        for (int y = 0; y < chunk_size; ++y) {
            for (int z = 0; z < chunk_size; ++z) {
                out.blocks[padded_index(x, y + 1, z + 1)] = row(y)[static_cast<std::size_t>(z)];
            }
        }
    } else if (dy != 0) {
        const int y = span_of(dy).padded_first;
        for (int z = 0; z < chunk_size; ++z) {
            std::ranges::copy(row(z),
                              blocks.subspan(padded_index(1, y, z + 1), chunk_size).begin());
        }
    } else {
        const int z = span_of(dz).padded_first;
        for (int y = 0; y < chunk_size; ++y) {
            std::ranges::copy(row(y),
                              blocks.subspan(padded_index(1, y + 1, z), chunk_size).begin());
        }
    }
}

// The layer of a face neighbor that touches the middle chunk, or air when
// the neighbor is missing, into its place in the border.
void place_neighbor_face(const ChunkNeighborhood& neighborhood, int dx, int dy, int dz,
                         std::span<BlockState, chunk_area> layer, PaddedChunk& out) noexcept {
    if (const ChunkStorage* chunk = neighborhood.at(dx, dy, dz)) {
        chunk->copy_face(facing_back(dx, dy, dz), layer);
    } else {
        std::ranges::fill(layer, BlockState::air);
    }
    place_face(dx, dy, dz, layer, out);
}

// An edge of the border, 32 blocks, or a corner, 1, read block by block from
// the neighbor at that offset, or air when it is missing.
void place_edge(int dx, int dy, int dz, const ChunkStorage* chunk, PaddedChunk& out) noexcept {
    const Span x = span_of(dx);
    const Span y = span_of(dy);
    const Span z = span_of(dz);
    for (int j = 0; j < y.count; ++j) {
        for (int k = 0; k < z.count; ++k) {
            for (int i = 0; i < x.count; ++i) {
                out.blocks[padded_index(x.padded_first + i, y.padded_first + j,
                                        z.padded_first + k)] =
                    chunk == nullptr ? BlockState::air
                                     : chunk->get(local_index(x.local_first + i, y.local_first + j,
                                                              z.local_first + k));
            }
        }
    }
}

} // namespace

void assemble_padded(const ChunkNeighborhood& neighborhood, PaddedChunk& out) noexcept {
    neighborhood.middle().to_dense(out.blocks,
                                   {.offset = padded_index(1, 1, 1),
                                    .row_stride = std::size_t{padded_size},
                                    .layer_stride = std::size_t{padded_size} * padded_size});
    std::array<BlockState, chunk_area> layer{};
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int offset_axes = (dx != 0 ? 1 : 0) + (dy != 0 ? 1 : 0) + (dz != 0 ? 1 : 0);
                if (offset_axes == 1) {
                    place_neighbor_face(neighborhood, dx, dy, dz, layer, out);
                } else if (offset_axes > 1) {
                    place_edge(dx, dy, dz, neighborhood.at(dx, dy, dz), out);
                }
            }
        }
    }
}

void coarser_border(const ChunkStorage& coarser, Face side, std::array<int, 2> offset,
                    PaddedChunk& out) noexcept {
    const int sign = (static_cast<int>(side) % 2 == 0) ? 1 : -1; // pos_x, pos_y, pos_z first
    const int axis = static_cast<int>(side) / 2;
    const int dx = axis == 0 ? sign : 0;
    const int dy = axis == 1 ? sign : 0;
    const int dz = axis == 2 ? sign : 0;
    std::array<BlockState, chunk_area> layer{};
    coarser.copy_face(facing_back(dx, dy, dz), layer);
    std::array<BlockState, chunk_area> fine{};
    const auto at = [](int first, int second) {
        return static_cast<std::size_t>(first) + (static_cast<std::size_t>(second) * chunk_size);
    };
    for (int b = 0; b < chunk_size; ++b) {
        for (int a = 0; a < chunk_size; ++a) {
            fine[at(a, b)] = layer[at(offset[0] + (a / 2), offset[1] + (b / 2))];
        }
    }
    place_face(dx, dy, dz, fine, out);
}

void open_border(Face side, PaddedChunk& out) noexcept {
    const int sign = (static_cast<int>(side) % 2 == 0) ? 1 : -1; // pos_x, pos_y, pos_z first
    const int axis = static_cast<int>(side) / 2;
    std::array<BlockState, chunk_area> air{};
    air.fill(BlockState::air);
    place_face(axis == 0 ? sign : 0, axis == 1 ? sign : 0, axis == 2 ? sign : 0, air, out);
}

} // namespace vx
