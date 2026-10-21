// RFC 0011's meshing at a level: corners lit past level 0, each face's
// outermost slice counted as its border set, and a second border set against
// the coarser neighbor's cells, each over 2 by 2 of the chunk's.

#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <tuple>
#include <vector>

namespace {

using vx::Face;

constexpr auto stone = static_cast<vx::BlockState>(1);
constexpr auto dirt = static_cast<vx::BlockState>(2);

std::vector<vx::BlockVisual> visual_table() {
    const auto all = [](std::uint16_t texture) {
        return vx::BlockVisual{.opacity = vx::Opacity::opaque,
                               .cull_group = 0,
                               .texture = {texture, texture, texture, texture, texture, texture}};
    };
    return {vx::BlockVisual{}, all(1), all(2)};
}

// Boxes of stone and dirt in random places, the border included.
void fill_with_boxes(vx::PaddedChunk& padded, std::mt19937& random, int boxes) {
    padded.blocks.fill(vx::BlockState::air);
    std::uniform_int_distribution<int> corner(0, vx::padded_size - 1);
    std::uniform_int_distribution<int> size(1, 12);
    std::uniform_int_distribution<int> kind(0, 2);
    for (int box = 0; box < boxes; ++box) {
        const int x0 = corner(random);
        const int y0 = corner(random);
        const int z0 = corner(random);
        const int width = size(random);
        const int height = size(random);
        const int depth = size(random);
        const auto state = static_cast<vx::BlockState>(kind(random));
        for (int y = y0; y < std::min(y0 + height, vx::padded_size); ++y) {
            for (int z = z0; z < std::min(z0 + depth, vx::padded_size); ++z) {
                for (int x = x0; x < std::min(x0 + width, vx::padded_size); ++x) {
                    padded.blocks[vx::padded_index(x, y, z)] = state;
                }
            }
        }
    }
}

// One face of one block: pass, direction, block, texture.
using UnitFace = std::tuple<int, int, int, int, int, int>;

std::multiset<UnitFace> faces_of(const vx::ChunkMesh& mesh) {
    std::multiset<UnitFace> faces;
    for (int pass = 0; pass < 2; ++pass) {
        for (int face = 0; face < 6; ++face) {
            for (const vx::Quad quad :
                 mesh.ranges[static_cast<std::size_t>(pass)][static_cast<std::size_t>(face)]) {
                const vx::QuadFields fields = vx::decode_quad(quad);
                const vx::FaceAxes axes = vx::face_axes(fields.face);
                for (int h = 0; h < fields.height; ++h) {
                    for (int w = 0; w < fields.width; ++w) {
                        std::array<int, 3> at{fields.block.x, fields.block.y, fields.block.z};
                        at[static_cast<std::size_t>(axes.width)] += w;
                        at[static_cast<std::size_t>(axes.height)] += h;
                        faces.emplace(pass, face, at[0], at[1], at[2], fields.texture);
                    }
                }
            }
        }
    }
    return faces;
}

// The block coordinate along a face's normal.
int depth_of(const vx::QuadFields& fields) {
    const std::array<int, 3> at{fields.block.x, fields.block.y, fields.block.z};
    return at[static_cast<std::size_t>(vx::face_axes(fields.face).normal)];
}

bool positive(Face face) {
    return static_cast<int>(face) % 2 == 0;
}

constexpr std::array<Face, 6> all_faces{Face::pos_x, Face::neg_x, Face::pos_y,
                                        Face::neg_y, Face::pos_z, Face::neg_z};

} // namespace

TEST_CASE("past level 0 every corner is lit, the same faces show, and they merge on texture "
          "alone") {
    const std::vector<vx::BlockVisual> visuals = visual_table();
    vx::MeshScratch scratch;
    auto padded = std::make_unique<vx::PaddedChunk>();
    std::mt19937 random(0x0CC);
    std::size_t darkened = 0;
    for (int round = 0; round < 10; ++round) {
        fill_with_boxes(*padded, random, 40);
        const vx::ChunkMesh lit_mesh =
            vx::mesh_chunk(*padded, visuals, scratch, {.occlusion = false});
        const std::multiset<UnitFace> lit = faces_of(lit_mesh);
        const std::size_t lit_quads = lit_mesh.all().size();
        for (const vx::Quad quad : lit_mesh.all()) {
            const vx::QuadFields fields = vx::decode_quad(quad);
            CHECK(fields.occlusion == std::array<std::uint8_t, 4>{3, 3, 3, 3});
        }
        const vx::ChunkMesh shaded = vx::mesh_chunk(*padded, visuals, scratch);
        for (const vx::Quad quad : shaded.all()) {
            const vx::QuadFields fields = vx::decode_quad(quad);
            darkened += std::ranges::any_of(fields.occlusion, [](std::uint8_t c) { return c < 3; })
                            ? 1U
                            : 0U;
        }
        CHECK(faces_of(shaded) == lit);
        CHECK(lit_quads <= shaded.all().size());
    }
    CHECK(darkened > 0);
}

TEST_CASE("each face's outermost slice is its border set: the first quads of a negative face, "
          "the last of a positive one") {
    const std::vector<vx::BlockVisual> visuals = visual_table();
    vx::MeshScratch scratch;
    auto padded = std::make_unique<vx::PaddedChunk>();
    std::mt19937 random(0xB0D);
    std::size_t bordering = 0;
    for (int round = 0; round < 10; ++round) {
        fill_with_boxes(*padded, random, 40);
        const vx::ChunkMesh mesh = vx::mesh_chunk(*padded, visuals, scratch);
        for (std::size_t pass = 0; pass < 2; ++pass) {
            for (const Face face : all_faces) {
                const std::span<const vx::Quad> range =
                    mesh.ranges[pass][static_cast<std::size_t>(face)];
                const std::size_t border = mesh.border[pass][static_cast<std::size_t>(face)];
                REQUIRE(border <= range.size());
                const int edge = positive(face) ? vx::chunk_size - 1 : 0;
                for (std::size_t i = 0; i < range.size(); ++i) {
                    const bool in_border = positive(face) ? i >= range.size() - border : i < border;
                    CHECK((depth_of(vx::decode_quad(range[i])) == edge) == in_border);
                }
                bordering += border;
            }
        }
    }
    CHECK(bordering > 0);
}

TEST_CASE("against the chunk's own border, mesh_border makes the border set mesh_chunk made") {
    const std::vector<vx::BlockVisual> visuals = visual_table();
    vx::MeshScratch scratch;
    auto padded = std::make_unique<vx::PaddedChunk>();
    std::mt19937 random(0x5E7);
    for (int round = 0; round < 6; ++round) {
        fill_with_boxes(*padded, random, 40);
        const vx::ChunkMesh whole = vx::mesh_chunk(*padded, visuals, scratch);
        std::array<std::array<std::vector<vx::Quad>, 6>, 2> expected;
        for (std::size_t pass = 0; pass < 2; ++pass) {
            for (const Face face : all_faces) {
                const auto at = static_cast<std::size_t>(face);
                const std::span<const vx::Quad> range = whole.ranges[pass][at];
                const std::size_t border = whole.border[pass][at];
                const std::span<const vx::Quad> part =
                    positive(face) ? range.last(border) : range.first(border);
                expected[pass][at].assign(part.begin(), part.end());
            }
        }
        for (const Face side : all_faces) {
            const vx::ChunkMesh border = vx::mesh_border(*padded, visuals, scratch, side);
            for (std::size_t pass = 0; pass < 2; ++pass) {
                for (const Face face : all_faces) {
                    const auto at = static_cast<std::size_t>(face);
                    const std::span<const vx::Quad> got = border.ranges[pass][at];
                    if (face == side) {
                        CHECK(std::vector<vx::Quad>(got.begin(), got.end()) == expected[pass][at]);
                        CHECK(border.border[pass][at] == got.size());
                    } else {
                        CHECK(got.empty());
                    }
                }
            }
            CHECK(border.all().size() == expected[0][static_cast<std::size_t>(side)].size() +
                                             expected[1][static_cast<std::size_t>(side)].size());
        }
    }
}

TEST_CASE("against a coarser neighbor a border face shows where the coarse cell over it is not "
          "opaque, 2 by 2") {
    const std::vector<vx::BlockVisual> visuals = visual_table();
    vx::MeshScratch scratch;
    auto padded = std::make_unique<vx::PaddedChunk>();
    std::mt19937 random(0xC0A);
    std::bernoulli_distribution solid(0.5);
    vx::PaletteScratch palette;
    for (const Face side : all_faces) {
        for (const std::array<int, 2> offset :
             {std::array{0, 0}, std::array{16, 0}, std::array{0, 16}, std::array{16, 16}}) {
            // The chunk: stone throughout, its border air; the coarser
            // neighbor: stone and air at random.
            padded->blocks.fill(vx::BlockState::air);
            for (int y = 1; y <= vx::chunk_size; ++y) {
                for (int z = 1; z <= vx::chunk_size; ++z) {
                    for (int x = 1; x <= vx::chunk_size; ++x) {
                        padded->blocks[vx::padded_index(x, y, z)] = stone;
                    }
                }
            }
            std::array<vx::BlockState, vx::chunk_volume> coarse{};
            for (vx::BlockState& cell : coarse) {
                cell = solid(random) ? dirt : vx::BlockState::air;
            }
            const vx::ChunkStorage coarser = vx::ChunkStorage::from_dense(coarse, palette);
            const vx::PaddedChunk before = *padded;
            vx::coarser_border(coarser, side, offset, *padded);
            // Only the face layer toward `side` changed.
            const vx::FaceAxes axes = vx::face_axes(side);
            const int layer = positive(side) ? vx::padded_size - 1 : 0;
            for (int y = 0; y < vx::padded_size; ++y) {
                for (int z = 0; z < vx::padded_size; ++z) {
                    for (int x = 0; x < vx::padded_size; ++x) {
                        const std::array<int, 3> at{x, y, z};
                        const bool on_face =
                            at[static_cast<std::size_t>(axes.normal)] == layer &&
                            at[static_cast<std::size_t>(axes.width)] >= 1 &&
                            at[static_cast<std::size_t>(axes.width)] <= vx::chunk_size &&
                            at[static_cast<std::size_t>(axes.height)] >= 1 &&
                            at[static_cast<std::size_t>(axes.height)] <= vx::chunk_size;
                        if (!on_face) {
                            CHECK(padded->blocks[vx::padded_index(x, y, z)] ==
                                  before.blocks[vx::padded_index(x, y, z)]);
                        }
                    }
                }
            }
            // Which border faces show, against an independent reading of the
            // coarse cells: the coarser chunk's layer touching the chunk, in
            // copy_face's axis order, each cell over 2 by 2.
            std::set<std::array<int, 3>> expected;
            const int edge = positive(side) ? vx::chunk_size - 1 : 0;
            const int coarse_layer = positive(side) ? 0 : vx::chunk_size - 1;
            const auto normal = static_cast<int>(axes.normal);
            const int first = normal == 0 ? 2 : 0;  // z on an x face, else x
            const int second = normal == 1 ? 2 : 1; // z on a y face, else y
            for (int b = 0; b < vx::chunk_size; ++b) {
                for (int a = 0; a < vx::chunk_size; ++a) {
                    std::array<int, 3> cell{};
                    cell[static_cast<std::size_t>(normal)] = coarse_layer;
                    cell[static_cast<std::size_t>(first)] = offset[0] + (a / 2);
                    cell[static_cast<std::size_t>(second)] = offset[1] + (b / 2);
                    if (coarse[vx::local_index(cell[0], cell[1], cell[2])] == vx::BlockState::air) {
                        std::array<int, 3> block{};
                        block[static_cast<std::size_t>(normal)] = edge;
                        block[static_cast<std::size_t>(first)] = a;
                        block[static_cast<std::size_t>(second)] = b;
                        expected.insert(block);
                    }
                }
            }
            const vx::ChunkMesh border =
                vx::mesh_border(*padded, visuals, scratch, side, {.merge = false});
            std::set<std::array<int, 3>> shown;
            for (const vx::Quad quad : border.all()) {
                const vx::QuadFields fields = vx::decode_quad(quad);
                CHECK(fields.face == side);
                shown.insert({fields.block.x, fields.block.y, fields.block.z});
            }
            CHECK(shown == expected);
            CHECK_FALSE(expected.empty());
        }
    }
}

TEST_CASE("an open border makes a face for every opaque block of the outermost slice that way, "
          "and leaves the rest") {
    const std::vector<vx::BlockVisual> visuals = visual_table();
    vx::MeshScratch scratch;
    auto padded = std::make_unique<vx::PaddedChunk>();
    std::mt19937 random(0x0BE);
    for (const Face side : all_faces) {
        fill_with_boxes(*padded, random, 40);
        // At least one opaque block in the outermost slice that way.
        const vx::FaceAxes slice = vx::face_axes(side);
        std::array<int, 3> seed{16, 16, 16};
        seed.at(static_cast<std::size_t>(slice.normal)) = positive(side) ? vx::chunk_size : 1;
        padded->blocks[vx::padded_index(seed[0], seed[1], seed[2])] = stone;
        const vx::PaddedChunk before = *padded;
        vx::open_border(side, *padded);
        // Only the face layer toward `side` changed, and it is air.
        const vx::FaceAxes axes = vx::face_axes(side);
        const int layer = positive(side) ? vx::padded_size - 1 : 0;
        std::size_t changed_elsewhere = 0;
        for (int y = 0; y < vx::padded_size; ++y) {
            for (int z = 0; z < vx::padded_size; ++z) {
                for (int x = 0; x < vx::padded_size; ++x) {
                    const std::array<int, 3> at{x, y, z};
                    const bool on_face =
                        at[static_cast<std::size_t>(axes.normal)] == layer &&
                        at[static_cast<std::size_t>(axes.width)] >= 1 &&
                        at[static_cast<std::size_t>(axes.width)] <= vx::chunk_size &&
                        at[static_cast<std::size_t>(axes.height)] >= 1 &&
                        at[static_cast<std::size_t>(axes.height)] <= vx::chunk_size;
                    const vx::BlockState now = padded->blocks[vx::padded_index(x, y, z)];
                    if (on_face) {
                        CHECK(now == vx::BlockState::air);
                    } else {
                        changed_elsewhere +=
                            now == before.blocks[vx::padded_index(x, y, z)] ? 0U : 1U;
                    }
                }
            }
        }
        CHECK(changed_elsewhere == 0);
        // Each opaque block of the outermost slice shows its face that way.
        std::set<std::array<int, 3>> expected;
        const int edge = positive(side) ? vx::chunk_size - 1 : 0;
        const auto normal = static_cast<std::size_t>(axes.normal);
        for (int b = 0; b < vx::chunk_size; ++b) {
            for (int a = 0; a < vx::chunk_size; ++a) {
                std::array<int, 3> block{};
                block.at(normal) = edge;
                block.at(static_cast<std::size_t>(axes.width)) = a;
                block.at(static_cast<std::size_t>(axes.height)) = b;
                if (padded->blocks[vx::padded_index(block[0] + 1, block[1] + 1, block[2] + 1)] !=
                    vx::BlockState::air) {
                    expected.insert(block);
                }
            }
        }
        const vx::ChunkMesh border =
            vx::mesh_border(*padded, visuals, scratch, side, {.merge = false});
        std::set<std::array<int, 3>> shown;
        for (const vx::Quad quad : border.all()) {
            const vx::QuadFields fields = vx::decode_quad(quad);
            CHECK(fields.face == side);
            shown.insert({fields.block.x, fields.block.y, fields.block.z});
        }
        CHECK(shown == expected);
        CHECK_FALSE(expected.empty());
    }
}
