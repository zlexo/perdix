#include <vx/mesh/mesh_chunk.hpp>

#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
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
using SliceKeys = MeshScratch::SliceKeys;

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

// Whether padded block (x, y, z) is opaque.
bool opaque_at(const Rows& opaque, int x, int y, int z) noexcept {
    return ((opaque[static_cast<std::size_t>(y)][static_cast<std::size_t>(z)] >>
             static_cast<unsigned>(x)) &
            1U) != 0;
}

// A corner's ambient occlusion (RFC 0003, after Lysenko): 3 is fully lit, 0
// the darkest. With both edge blocks in place, the corner block behind them
// makes no difference.
constexpr std::uint8_t corner_occlusion(bool side1, bool side2, bool corner) noexcept {
    if (side1 && side2) {
        return 0;
    }
    return static_cast<std::uint8_t>(3 - (int{side1} + int{side2} + int{corner}));
}

// Each corner of `face` of `block` looks at the blocks beside it on the
// face's open side: one along each of its two edges and one across its
// diagonal. Only opaque blocks occlude. Corner 0 first, at the low ends of
// the face's width and height axes, as in the quad.
std::array<std::uint8_t, 4> face_occlusion(const Rows& opaque, Face face, LocalPos block) noexcept {
    const FaceAxes axes = face_axes(face);
    // The block in front of the face, padded.
    std::array<int, 3> front{block.x + 1, block.y + 1, block.z + 1};
    front[axes.normal] += faces_positive(face) ? 1 : -1;
    const auto beside = [&](int along_width, int along_height) {
        std::array<int, 3> at = front;
        at[axes.width] += along_width;
        at[axes.height] += along_height;
        return opaque_at(opaque, at[0], at[1], at[2]);
    };
    const bool width_low = beside(-1, 0);
    const bool width_high = beside(1, 0);
    const bool height_low = beside(0, -1);
    const bool height_high = beside(0, 1);
    return {corner_occlusion(width_low, height_low, beside(-1, -1)),
            corner_occlusion(width_high, height_low, beside(1, -1)),
            corner_occlusion(width_high, height_high, beside(1, 1)),
            corner_occlusion(width_low, height_high, beside(-1, 1))};
}

// The bits of a run of `width` faces from bit `first` of a row, which holds
// it: first + width is 32 at most.
std::uint32_t run_bits(int first, int width) noexcept {
    return static_cast<std::uint32_t>(((std::uint64_t{1} << static_cast<unsigned>(width)) - 1U)
                                      << static_cast<unsigned>(first));
}

// Where a run that starts at `first` ends along its row: past the last face
// that goes on with the same key.
int run_end(std::uint32_t row, const std::array<std::uint32_t, chunk_size>& row_keys, int first,
            std::uint32_t key) noexcept {
    int end = first + 1;
    while (end < chunk_size && ((row >> static_cast<unsigned>(end)) & 1U) != 0 &&
           row_keys[static_cast<std::size_t>(end)] == key) {
        ++end;
    }
    return end;
}

// Where a run in row `v` ends down the slice: past the last row below that
// holds the same run, every face of it with the same key. Clears the run
// from each row it takes.
int run_bottom(std::array<std::uint32_t, chunk_size>& rows, const SliceKeys& keys, int v, int first,
               int width, std::uint32_t key) noexcept {
    const std::uint32_t run = run_bits(first, width);
    int below = v + 1;
    for (; below < chunk_size; ++below) {
        const auto below_at = static_cast<std::size_t>(below);
        const auto row_keys =
            std::span(keys[below_at])
                .subspan(static_cast<std::size_t>(first), static_cast<std::size_t>(width));
        if ((rows[below_at] & run) != run ||
            !std::ranges::all_of(row_keys, [&](std::uint32_t other) { return other == key; })) {
            break;
        }
        rows[below_at] &= ~run;
    }
    return below;
}

// Writes the quads of one slice from its rows of visible faces, clearing
// the rows as it goes. Merged greedily (RFC 0003): take the lowest face of a
// row, extend the run along the row while the faces go on with the same key,
// then down the rows while the next row holds the same run with the same
// keys. Each face alone when `merge` is off.
std::size_t emit_slice(Face face, int depth, std::array<std::uint32_t, chunk_size>& rows,
                       const SliceKeys& keys, bool merge, std::span<Quad> out) noexcept {
    std::size_t written = 0;
    for (int v = 0; v < chunk_size; ++v) {
        const auto v_at = static_cast<std::size_t>(v);
        while (rows[v_at] != 0) {
            const int first = std::countr_zero(rows[v_at]);
            const std::uint32_t key = keys[v_at][static_cast<std::size_t>(first)];
            const int width =
                (merge ? run_end(rows[v_at], keys[v_at], first, key) : first + 1) - first;
            rows[v_at] &= ~run_bits(first, width);
            const int bottom = merge ? run_bottom(rows, keys, v, first, width, key) : v + 1;
            out[written++] = {.geometry = quad_geometry(slice_block(face, depth, first, v), width,
                                                        bottom - v, face),
                              .material = key};
        }
    }
    return written;
}

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
      planes_(std::make_unique<Planes>()), keys_(std::make_unique<SliceKeys>()),
      // NOLINTNEXTLINE(modernize-avoid-c-arrays)
      quads_(std::make_unique<Quad[]>(max_chunk_quads)) {}

namespace {

// The depth of a face's slice at the chunk's edge that way: the slice whose
// faces the neighbor across that side decides (RFC 0011).
constexpr int border_depth(Face face) noexcept {
    return static_cast<int>(face) % 2 == 0 ? chunk_size - 1 : 0;
}

constexpr std::array<std::uint8_t, 4> fully_lit{3, 3, 3, 3};

} // namespace

// The quads of slice `depth` of `face` in `pass`, from its plane in the
// scratch, which it clears, into `out`: how many.
std::size_t MeshScratch::emit(const PaddedChunk& blocks, std::span<const BlockVisual> visuals,
                              std::size_t pass, Face face, int depth, MeshOptions options,
                              std::span<Quad> out) noexcept {
    const auto face_at = static_cast<std::size_t>(face);
    auto& rows = (*planes_)[pass][face_at][static_cast<std::size_t>(depth)];
    if (std::ranges::all_of(rows, [](std::uint32_t row) { return row == 0; })) {
        return 0;
    }
    // What each visible face looks like: faces merge only where this is equal.
    SliceKeys& keys = *keys_;
    for (int v = 0; v < chunk_size; ++v) {
        const auto v_at = static_cast<std::size_t>(v);
        for (std::uint32_t left = rows[v_at]; left != 0; left &= left - 1) {
            const int u = std::countr_zero(left);
            const LocalPos block = slice_block(face, depth, u, v);
            const BlockVisual& visual =
                visual_of(visuals, blocks.at(block.x + 1, block.y + 1, block.z + 1));
            keys[v_at][static_cast<std::size_t>(u)] = quad_material(
                visual.texture[face_at],
                options.occlusion ? face_occlusion(*opaque_, face, block) : fully_lit);
        }
    }
    return emit_slice(face, depth, rows, keys, options.merge, out);
}

ChunkMesh mesh_chunk(const PaddedChunk& blocks, std::span<const BlockVisual> visuals,
                     MeshScratch& scratch, MeshOptions options) noexcept {
    build_rows(blocks, visuals, *scratch.opaque_, *scratch.cutout_);
    cull(blocks, visuals, *scratch.opaque_, *scratch.cutout_, *scratch.planes_);

    ChunkMesh mesh{};
    const auto quads = std::span<Quad>(scratch.quads_.get(), max_chunk_quads);
    std::size_t written = 0;
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t face_at = 0; face_at < 6; ++face_at) {
            const auto face = static_cast<Face>(face_at);
            const std::size_t range_first = written;
            for (int depth = 0; depth < chunk_size; ++depth) {
                const std::size_t made = scratch.emit(blocks, visuals, pass, face, depth, options,
                                                      quads.subspan(written));
                if (depth == border_depth(face)) {
                    mesh.border[pass][face_at] = static_cast<std::uint32_t>(made);
                }
                written += made;
            }
            mesh.ranges[pass][face_at] = quads.subspan(range_first, written - range_first);
        }
    }
    return mesh;
}

ChunkMesh mesh_border(const PaddedChunk& blocks, std::span<const BlockVisual> visuals,
                      MeshScratch& scratch, Face side, MeshOptions options) noexcept {
    build_rows(blocks, visuals, *scratch.opaque_, *scratch.cutout_);
    cull(blocks, visuals, *scratch.opaque_, *scratch.cutout_, *scratch.planes_);

    ChunkMesh mesh{};
    const auto quads = std::span<Quad>(scratch.quads_.get(), max_chunk_quads);
    std::size_t written = 0;
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t face_at = 0; face_at < 6; ++face_at) {
            const auto face = static_cast<Face>(face_at);
            std::size_t made = 0;
            if (face == side) {
                made = scratch.emit(blocks, visuals, pass, face, border_depth(face), options,
                                    quads.subspan(written));
                mesh.border[pass][face_at] = static_cast<std::uint32_t>(made);
            }
            mesh.ranges[pass][face_at] = quads.subspan(written, made);
            written += made;
        }
    }
    // The other slices' planes are left as cull made them: the next mesh
    // builds them again.
    return mesh;
}

} // namespace vx
