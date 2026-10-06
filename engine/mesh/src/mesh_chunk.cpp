#include <vx/mesh/mesh_chunk.hpp>

#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace vx {
namespace {

using Rows = MeshScratch::Rows;
using Planes = MeshScratch::Planes;

// Bits 1 to 32 of a padded row: the chunk's own blocks, not the border's.
constexpr std::uint64_t inner = 0x1'FFFF'FFFEULL;

constexpr std::size_t face_index(Face face) noexcept {
    return static_cast<std::size_t>(face);
}

const BlockVisual& visual_of(std::span<const BlockVisual> visuals, BlockState state) noexcept {
    assert(static_cast<std::size_t>(state) < visuals.size());
    return visuals[static_cast<std::size_t>(state)];
}

// Each padded row along x as two masks: its opaque blocks, which hide the
// faces next to them, and its cutout blocks, which draw faces but hide
// nothing (RFC 0003).
void build_rows(const PaddedChunk& blocks, std::span<const BlockVisual> visuals, Rows& opaque,
                Rows& cutout) noexcept {
    const auto all = std::span<const BlockState>(blocks.blocks);
    for (int y = 0; y < padded_size; ++y) {
        for (int z = 0; z < padded_size; ++z) {
            const auto row = all.subspan(padded_index(0, y, z), padded_size);
            std::uint64_t opaque_row = 0;
            std::uint64_t cutout_row = 0;
            for (std::size_t x = 0; x < row.size(); ++x) {
                const Opacity opacity = visual_of(visuals, row[x]).opacity;
                opaque_row |= std::uint64_t{opacity == Opacity::opaque} << x;
                cutout_row |= std::uint64_t{opacity == Opacity::cutout} << x;
            }
            const auto y_at = static_cast<std::size_t>(y);
            const auto z_at = static_cast<std::size_t>(z);
            opaque[y_at][z_at] = opaque_row;
            cutout[y_at][z_at] = cutout_row;
        }
    }
}

// Where the neighbor of padded block (x, y, z) across `face` is.
struct Offset {
    int x;
    int y;
    int z;
};

constexpr std::array<Offset, 6> neighbor_offset{{
    {.x = 1, .y = 0, .z = 0},
    {.x = -1, .y = 0, .z = 0},
    {.x = 0, .y = 1, .z = 0},
    {.x = 0, .y = -1, .z = 0},
    {.x = 0, .y = 0, .z = 1},
    {.x = 0, .y = 0, .z = -1},
}};

// Clears the faces in `faces` between two cutout blocks of one nonzero cull
// group, which hide each other's shared faces (RFC 0003, open question 2).
// `pairs` holds the faces that have a cutout on both sides, the only ones to
// look at, so rows without cutouts never get here.
std::uint64_t cull_groups(std::uint64_t faces, std::uint64_t pairs, Face face, int y, int z,
                          const PaddedChunk& blocks,
                          std::span<const BlockVisual> visuals) noexcept {
    const Offset offset = neighbor_offset[face_index(face)];
    for (; pairs != 0; pairs &= pairs - 1) {
        const int x = std::countr_zero(pairs);
        const std::uint8_t group = visual_of(visuals, blocks.at(x, y, z)).cull_group;
        if (group != 0 &&
            group == visual_of(visuals, blocks.at(x + offset.x, y + offset.y, z + offset.z))
                         .cull_group) {
            faces &= ~(std::uint64_t{1} << x);
        }
    }
    return faces;
}

// Puts the visible faces of padded row (y, z) into their slices' planes, the
// opaque blocks' in the opaque pass and the cutouts' in the cutout pass.
// y faces go to slice y, row z, bit x, and z faces to slice z, row y, bit
// x: whole rows, since bit x of a padded row is block x - 1 of the chunk.
// x faces go to slice x, row y, bit z, a bit at a time.
void store_faces(const std::array<std::uint64_t, 6>& faces, std::uint64_t own_opaque,
                 std::uint64_t own_cutout, int y, int z, Planes& planes) noexcept {
    const auto y_at = static_cast<std::size_t>(y);
    const auto z_at = static_cast<std::size_t>(z);
    for (std::size_t pass = 0; pass < planes.size(); ++pass) {
        const std::uint64_t mine = pass == 0 ? own_opaque : own_cutout;
        auto& faces_of_pass = planes[pass];
        const auto slice_row = [&](Face face) {
            return static_cast<std::uint32_t>((faces[face_index(face)] & mine) >> 1U);
        };
        faces_of_pass[face_index(Face::pos_y)][y_at - 1][z_at - 1] = slice_row(Face::pos_y);
        faces_of_pass[face_index(Face::neg_y)][y_at - 1][z_at - 1] = slice_row(Face::neg_y);
        faces_of_pass[face_index(Face::pos_z)][z_at - 1][y_at - 1] = slice_row(Face::pos_z);
        faces_of_pass[face_index(Face::neg_z)][z_at - 1][y_at - 1] = slice_row(Face::neg_z);
        const std::uint32_t bit = std::uint32_t{1} << static_cast<unsigned>(z - 1);
        for (const Face face : {Face::pos_x, Face::neg_x}) {
            auto& slices = faces_of_pass[face_index(face)];
            for (std::uint64_t left = faces[face_index(face)] & mine; left != 0; left &= left - 1) {
                const auto x = static_cast<std::size_t>(std::countr_zero(left));
                slices[x - 1][y_at - 1] |= bit;
            }
        }
    }
}

// Culls every face of the chunk with shifts and masks on the padded rows,
// then puts each visible face into its slice's plane. Faces of the y and z
// directions come out of and-not between neighboring x rows, already in
// the rows of their slices; only the x faces are moved bit by bit.
void cull(const PaddedChunk& blocks, std::span<const BlockVisual> visuals, const Rows& opaque,
          const Rows& cutout, Planes& planes) noexcept {
    for (auto& pass : planes) {
        pass[face_index(Face::pos_x)] = {};
        pass[face_index(Face::neg_x)] = {};
    }
    for (int y = 1; y <= chunk_size; ++y) {
        const auto y_at = static_cast<std::size_t>(y);
        for (int z = 1; z <= chunk_size; ++z) {
            const auto z_at = static_cast<std::size_t>(z);
            const std::uint64_t own_opaque = opaque[y_at][z_at];
            const std::uint64_t own_cutout = cutout[y_at][z_at];
            const std::uint64_t draws = (own_opaque | own_cutout) & inner;
            // A face shows unless the block next to it, across the face, is
            // opaque.
            std::array<std::uint64_t, 6> faces{
                draws & ~(own_opaque >> 1U),     draws & ~(own_opaque << 1U),
                draws & ~opaque[y_at + 1][z_at], draws & ~opaque[y_at - 1][z_at],
                draws & ~opaque[y_at][z_at + 1], draws & ~opaque[y_at][z_at - 1],
            };
            if ((own_cutout & inner) != 0) {
                const std::array<std::uint64_t, 6> cutout_beside{
                    own_cutout >> 1U,       own_cutout << 1U,       cutout[y_at + 1][z_at],
                    cutout[y_at - 1][z_at], cutout[y_at][z_at + 1], cutout[y_at][z_at - 1],
                };
                for (std::size_t face = 0; face < faces.size(); ++face) {
                    faces[face] =
                        cull_groups(faces[face], faces[face] & own_cutout & cutout_beside[face],
                                    static_cast<Face>(face), y, z, blocks, visuals);
                }
            }
            store_faces(faces, own_opaque, own_cutout, y, z, planes);
        }
    }
}

// The block of the chunk at (u, v) in slice `depth` of `face`'s slices.
constexpr LocalPos slice_block(Face face, int depth, int u, int v) noexcept {
    switch (face_axes(face).normal) {
    case 0:
        return {.x = depth, .y = v, .z = u};
    case 1:
        return {.x = u, .y = depth, .z = v};
    default:
        return {.x = u, .y = v, .z = depth};
    }
}

// Every face fully lit, until RFC 0003's ambient occlusion.
constexpr std::array<std::uint8_t, 4> fully_lit{3, 3, 3, 3};

} // namespace

std::span<const Quad> ChunkMesh::pass(Pass which) const noexcept {
    const auto& faces = ranges[static_cast<std::size_t>(which)];
    return {
        faces.front().data(),
        static_cast<std::size_t>(faces.back().data() + faces.back().size() - faces.front().data())};
}

std::span<const Quad> ChunkMesh::all() const noexcept {
    const std::span<const Quad> opaque = pass(Pass::opaque);
    const std::span<const Quad> cutout = pass(Pass::cutout);
    return {opaque.data(), static_cast<std::size_t>(cutout.data() + cutout.size() - opaque.data())};
}

MeshScratch::MeshScratch()
    : opaque_(std::make_unique<Rows>()), cutout_(std::make_unique<Rows>()),
      planes_(std::make_unique<Planes>()),
      // NOLINTNEXTLINE(modernize-avoid-c-arrays)
      quads_(std::make_unique<Quad[]>(max_chunk_quads)) {}

ChunkMesh mesh_chunk(const PaddedChunk& blocks, std::span<const BlockVisual> visuals,
                     MeshScratch& scratch) noexcept {
    build_rows(blocks, visuals, *scratch.opaque_, *scratch.cutout_);
    cull(blocks, visuals, *scratch.opaque_, *scratch.cutout_, *scratch.planes_);

    ChunkMesh mesh{};
    const auto quads = std::span<Quad>(scratch.quads_.get(), max_chunk_quads);
    std::size_t written = 0;
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t face_at = 0; face_at < 6; ++face_at) {
            const auto face = static_cast<Face>(face_at);
            const std::size_t range_first = written;
            const auto& slices = (*scratch.planes_)[pass][face_at];
            for (int depth = 0; depth < chunk_size; ++depth) {
                const auto& rows = slices[static_cast<std::size_t>(depth)];
                for (int v = 0; v < chunk_size; ++v) {
                    for (std::uint32_t left = rows[static_cast<std::size_t>(v)]; left != 0;
                         left &= left - 1) {
                        const int u = std::countr_zero(left);
                        const LocalPos block = slice_block(face, depth, u, v);
                        const BlockVisual& visual =
                            visual_of(visuals, blocks.at(block.x + 1, block.y + 1, block.z + 1));
                        quads[written++] = {.geometry = quad_geometry(block, 1, 1, face),
                                            .material =
                                                quad_material(visual.texture[face_at], fully_lit)};
                    }
                }
            }
            mesh.ranges[pass][face_at] = quads.subspan(range_first, written - range_first);
        }
    }
    return mesh;
}

} // namespace vx
