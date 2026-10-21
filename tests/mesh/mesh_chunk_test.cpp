#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <tuple>
#include <vector>

namespace {

// A small visual table, as a block registry would build it.
namespace blocks {
constexpr auto stone = static_cast<vx::BlockState>(1);
constexpr auto grass = static_cast<vx::BlockState>(2);        // a texture of its own on each face
constexpr auto leaves = static_cast<vx::BlockState>(3);       // cutout, cull group 1
constexpr auto glass = static_cast<vx::BlockState>(4);        // cutout, cull group 2
constexpr auto bars = static_cast<vx::BlockState>(5);         // cutout, no cull group
constexpr auto birch_leaves = static_cast<vx::BlockState>(6); // cutout, cull group 1, like leaves
} // namespace blocks

std::vector<vx::BlockVisual> visual_table() {
    const auto all = [](vx::Opacity opacity, std::uint8_t group, std::uint16_t texture) {
        return vx::BlockVisual{.opacity = opacity,
                               .cull_group = group,
                               .texture = {texture, texture, texture, texture, texture, texture}};
    };
    return {
        vx::BlockVisual{},
        all(vx::Opacity::opaque, 0, 1),
        vx::BlockVisual{
            .opacity = vx::Opacity::opaque, .cull_group = 0, .texture = {10, 11, 12, 13, 14, 15}},
        all(vx::Opacity::cutout, 1, 5),
        all(vx::Opacity::cutout, 2, 6),
        all(vx::Opacity::cutout, 0, 7),
        all(vx::Opacity::cutout, 1, 8),
    };
}

// One face as a test sees it: its pass, its direction, its block and its
// texture.
using Face = std::tuple<int, int, int, int, int, int>; // pass, face, x, y, z, texture

// The faces a mesh's quads cover, one by one; a quad wider or taller than a
// block counts as each face it covers.
std::vector<Face> faces_of(const vx::ChunkMesh& mesh) {
    std::vector<Face> faces;
    for (int pass = 0; pass < 2; ++pass) {
        for (int face = 0; face < 6; ++face) {
            for (const vx::Quad quad :
                 mesh.range(static_cast<vx::Pass>(pass), static_cast<vx::Face>(face))) {
                const vx::QuadFields fields = vx::decode_quad(quad);
                if (static_cast<int>(fields.face) != face) {
                    FAIL("a quad facing ", static_cast<int>(fields.face), " in face ", face,
                         "'s range");
                }
                const vx::FaceAxes axes = vx::face_axes(fields.face);
                for (int j = 0; j < fields.height; ++j) {
                    for (int i = 0; i < fields.width; ++i) {
                        std::array<int, 3> at{fields.block.x, fields.block.y, fields.block.z};
                        at[axes.width] += i;
                        at[axes.height] += j;
                        faces.emplace_back(pass, face, at[0], at[1], at[2], fields.texture);
                    }
                }
            }
        }
    }
    std::ranges::sort(faces);
    return faces;
}

// RFC 0003's rule, block by block: a face shows where a block that draws
// faces meets one that is not opaque, unless both are cutouts of one nonzero
// cull group.
std::vector<Face> expected_faces(const vx::PaddedChunk& padded,
                                 std::span<const vx::BlockVisual> visuals) {
    constexpr std::array<std::array<int, 3>, 6> step{
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    const auto visual = [&](int x, int y, int z) -> const vx::BlockVisual& {
        return visuals[static_cast<std::size_t>(padded.at(x, y, z))];
    };
    std::vector<Face> faces;
    for (int y = 0; y < vx::chunk_size; ++y) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            for (int x = 0; x < vx::chunk_size; ++x) {
                const vx::BlockVisual& own = visual(x + 1, y + 1, z + 1);
                if (own.opacity == vx::Opacity::air) {
                    continue;
                }
                for (int face = 0; face < 6; ++face) {
                    const auto& d = step[static_cast<std::size_t>(face)];
                    const vx::BlockVisual& next = visual(x + 1 + d[0], y + 1 + d[1], z + 1 + d[2]);
                    const bool hidden = next.opacity == vx::Opacity::opaque ||
                                        (own.opacity == vx::Opacity::cutout &&
                                         next.opacity == vx::Opacity::cutout &&
                                         own.cull_group != 0 && own.cull_group == next.cull_group);
                    if (!hidden) {
                        faces.emplace_back(own.opacity == vx::Opacity::opaque ? 0 : 1, face, x, y,
                                           z, own.texture[static_cast<std::size_t>(face)]);
                    }
                }
            }
        }
    }
    std::ranges::sort(faces);
    return faces;
}

struct Fixture {
    std::vector<vx::BlockVisual> visuals = visual_table();
    std::unique_ptr<vx::PaddedChunk> padded = std::make_unique<vx::PaddedChunk>();
    vx::MeshScratch scratch;

    // Block (x, y, z) of the chunk; -1 and 32 are the border.
    void put(int x, int y, int z, vx::BlockState state) const {
        padded->blocks[vx::padded_index(x + 1, y + 1, z + 1)] = state;
    }
    vx::ChunkMesh mesh() { return vx::mesh_chunk(*padded, visuals, scratch); }
    // Every face a quad of its own, as before merging.
    vx::ChunkMesh unmerged() { return vx::mesh_chunk(*padded, visuals, scratch, {.merge = false}); }
};

std::size_t quad_count(const vx::ChunkMesh& mesh) {
    return mesh.all().size();
}

} // namespace

TEST_CASE("a lone block shows all six faces, each with its own texture") {
    Fixture chunk;
    chunk.put(5, 6, 7, blocks::grass);
    const vx::ChunkMesh mesh = chunk.mesh();
    REQUIRE(quad_count(mesh) == 6);
    for (int face = 0; face < 6; ++face) {
        const auto range = mesh.range(vx::Pass::opaque, static_cast<vx::Face>(face));
        REQUIRE(range.size() == 1);
        const vx::QuadFields quad = vx::decode_quad(range[0]);
        CHECK(quad.block == vx::LocalPos{.x = 5, .y = 6, .z = 7});
        CHECK(quad.width == 1);
        CHECK(quad.height == 1);
        CHECK(quad.texture == 10 + face);
        CHECK(quad.occlusion == std::array<std::uint8_t, 4>{3, 3, 3, 3});
    }
    CHECK(mesh.pass(vx::Pass::cutout).empty());
}

TEST_CASE("two blocks side by side hide the faces between them") {
    Fixture chunk;
    chunk.put(5, 6, 7, blocks::stone);
    chunk.put(6, 6, 7, blocks::stone);
    const vx::ChunkMesh mesh = chunk.unmerged();
    // RFC 0003's test plan: 10 faces without merging.
    CHECK(quad_count(mesh) == 10);
    CHECK(mesh.range(vx::Pass::opaque, vx::Face::pos_x).size() == 1);
    CHECK(mesh.range(vx::Pass::opaque, vx::Face::neg_x).size() == 1);
    CHECK(faces_of(mesh) == expected_faces(*chunk.padded, chunk.visuals));
}

TEST_CASE("a full chunk in air shows only its six sides") {
    Fixture chunk;
    for (int y = 0; y < vx::chunk_size; ++y) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            for (int x = 0; x < vx::chunk_size; ++x) {
                chunk.put(x, y, z, blocks::stone);
            }
        }
    }
    const vx::ChunkMesh mesh = chunk.unmerged();
    for (int face = 0; face < 6; ++face) {
        CHECK(mesh.range(vx::Pass::opaque, static_cast<vx::Face>(face)).size() == 1'024);
    }
    CHECK(faces_of(mesh) == expected_faces(*chunk.padded, chunk.visuals));
}

TEST_CASE("an opaque neighbor hides the faces on the chunk's border, and air does not") {
    Fixture chunk;
    // A block in each corner of the chunk's lowest layer, all touching the
    // border.
    chunk.put(0, 0, 0, blocks::stone);
    chunk.put(31, 0, 31, blocks::stone);
    SUBCASE("neighbors of air") {
        CHECK(quad_count(chunk.unmerged()) == 12);
    }
    SUBCASE("opaque neighbors under the whole chunk and beside its -x side") {
        for (int z = -1; z <= 32; ++z) {
            for (int x = -1; x <= 32; ++x) {
                chunk.put(x, -1, z, blocks::stone);
            }
        }
        for (int y = -1; y <= 32; ++y) {
            for (int z = -1; z <= 32; ++z) {
                chunk.put(-1, y, z, blocks::stone);
            }
        }
        const vx::ChunkMesh mesh = chunk.unmerged();
        CHECK(mesh.range(vx::Pass::opaque, vx::Face::neg_y).empty());
        CHECK(mesh.range(vx::Pass::opaque, vx::Face::neg_x).size() == 1); // the block at x = 31
        CHECK(quad_count(mesh) == 9);
        CHECK(faces_of(mesh) == expected_faces(*chunk.padded, chunk.visuals));
    }
}

TEST_CASE("cutout blocks show what is behind them, and hide only their own cull group") {
    Fixture chunk;
    SUBCASE("two leaves: the faces between them go") {
        chunk.put(1, 1, 1, blocks::leaves);
        chunk.put(2, 1, 1, blocks::leaves);
        CHECK(chunk.unmerged().range(vx::Pass::cutout, vx::Face::pos_x).size() == 1);
        CHECK(quad_count(chunk.unmerged()) == 10);
    }
    SUBCASE("two kinds of leaves in one cull group: the faces between them go") {
        chunk.put(1, 1, 1, blocks::leaves);
        chunk.put(1, 2, 1, blocks::birch_leaves);
        CHECK(quad_count(chunk.unmerged()) == 10);
    }
    SUBCASE("leaves and glass: both faces between them stay") {
        chunk.put(1, 1, 1, blocks::leaves);
        chunk.put(1, 1, 2, blocks::glass);
        CHECK(quad_count(chunk.unmerged()) == 12);
    }
    SUBCASE("two cutouts without a cull group: both faces stay") {
        chunk.put(1, 1, 1, blocks::bars);
        chunk.put(2, 1, 1, blocks::bars);
        CHECK(quad_count(chunk.unmerged()) == 12);
    }
    SUBCASE("leaves on stone: the stone shows through, the leaves' face against it goes") {
        chunk.put(1, 1, 1, blocks::leaves);
        chunk.put(1, 0, 1, blocks::stone);
        const vx::ChunkMesh mesh = chunk.unmerged();
        CHECK(mesh.range(vx::Pass::opaque, vx::Face::pos_y).size() == 1);
        CHECK(mesh.range(vx::Pass::cutout, vx::Face::neg_y).empty());
        CHECK(mesh.pass(vx::Pass::opaque).size() == 6);
        CHECK(mesh.pass(vx::Pass::cutout).size() == 5);
    }
    CHECK(faces_of(chunk.unmerged()) == expected_faces(*chunk.padded, chunk.visuals));
}

TEST_CASE("on random chunks, the faces shown are exactly those RFC 0003's rule gives") {
    Fixture chunk;
    std::mt19937 generator(7);
    // From sparse to nearly full, with every kind of block, borders included.
    for (const int percent_solid : {5, 30, 50, 70, 95}) {
        for (int round = 0; round < 4; ++round) {
            std::uniform_int_distribution<int> roll(0, 99);
            std::uniform_int_distribution<int> kind(1, 6);
            for (vx::BlockState& block : chunk.padded->blocks) {
                block = roll(generator) < percent_solid
                            ? static_cast<vx::BlockState>(kind(generator))
                            : vx::BlockState::air;
            }
            INFO(percent_solid, "% solid, round ", round);
            CHECK(faces_of(chunk.mesh()) == expected_faces(*chunk.padded, chunk.visuals));
        }
    }
}

TEST_CASE("a pass's faces and all of a mesh's lie back to back") {
    Fixture chunk;
    chunk.put(1, 1, 1, blocks::stone);
    chunk.put(3, 1, 1, blocks::leaves);
    const vx::ChunkMesh mesh = chunk.mesh();
    CHECK(mesh.pass(vx::Pass::opaque).size() == 6);
    CHECK(mesh.pass(vx::Pass::cutout).size() == 6);
    CHECK(mesh.all().size() == 12);
    CHECK(mesh.pass(vx::Pass::cutout).data() == mesh.pass(vx::Pass::opaque).data() + 6);
    // Each range starts where the one before it ends.
    const vx::Quad* next = mesh.all().data();
    for (const auto& pass : mesh.ranges) {
        for (const auto& range : pass) {
            CHECK(range.data() == next);
            next = range.data() + range.size();
        }
    }
}

namespace {

// The quad of `face` on the block at `block`, which the test knows is there.
vx::QuadFields quad_of(const vx::ChunkMesh& mesh, vx::Pass pass, vx::Face face,
                       vx::LocalPos block) {
    for (const vx::Quad quad : mesh.range(pass, face)) {
        const vx::QuadFields fields = vx::decode_quad(quad);
        if (fields.block == block) {
            return fields;
        }
    }
    FAIL("no quad on that block");
    return {};
}

// Lysenko's table: 3 is fully lit, 0 the darkest.
int lysenko(bool side1, bool side2, bool corner) {
    return side1 && side2 ? 0 : 3 - (int{side1} + int{side2} + int{corner});
}

} // namespace

TEST_CASE(
    "each corner of a face darkens with the opaque blocks around it, as Lysenko's table says") {
    Fixture chunk;
    // A block in the middle, and blocks at two corners of the chunk, whose
    // corners look into the border's edges and corners.
    for (const vx::LocalPos block :
         {vx::LocalPos{.x = 10, .y = 11, .z = 12}, vx::LocalPos{.x = 0, .y = 0, .z = 0},
          vx::LocalPos{.x = 31, .y = 31, .z = 31}}) {
        for (int face_at = 0; face_at < 6; ++face_at) {
            const auto face = static_cast<vx::Face>(face_at);
            const std::size_t normal = static_cast<std::size_t>(face_at / 2);
            const std::array<int, 3> own{block.x, block.y, block.z};
            // The layer of blocks the face looks into, and its block there.
            std::array<int, 3> front = own;
            front[normal] += face_at % 2 == 0 ? 1 : -1;
            for (int corner = 0; corner < 4; ++corner) {
                // Of the blocks in that layer touching the corner, worked out
                // in 3D: the two beside the front block, and the one across.
                const std::array<int, 3> vertex = vx::quad_corner({.block = block,
                                                                   .width = 1,
                                                                   .height = 1,
                                                                   .face = face,
                                                                   .texture = 0,
                                                                   .occlusion = {3, 3, 3, 3}},
                                                                  corner);
                std::vector<std::array<int, 3>> sides;
                std::array<int, 3> across{};
                for (int a = 0; a < 2; ++a) {
                    for (int b = 0; b < 2; ++b) {
                        std::array<int, 3> at{};
                        int step = 0;
                        int differs = 0;
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            if (axis == normal) {
                                at[axis] = front[axis];
                                continue;
                            }
                            at[axis] = vertex[axis] - ((step++ == 0 ? a : b) == 0 ? 1 : 0);
                            differs += at[axis] != front[axis] ? 1 : 0;
                        }
                        if (differs == 1) {
                            sides.push_back(at);
                        } else if (differs == 2) {
                            across = at;
                        }
                    }
                }
                REQUIRE(sides.size() == 2);
                for (int arrangement = 0; arrangement < 8; ++arrangement) {
                    const bool side1 = (arrangement & 1) != 0;
                    const bool side2 = (arrangement & 2) != 0;
                    const bool diagonal = (arrangement & 4) != 0;
                    const auto set = [&](const std::array<int, 3>& at, vx::BlockState state) {
                        chunk.put(at[0], at[1], at[2], state);
                    };
                    chunk.put(block.x, block.y, block.z, blocks::stone);
                    set(sides[0], side1 ? blocks::stone : vx::BlockState::air);
                    set(sides[1], side2 ? blocks::stone : vx::BlockState::air);
                    set(across, diagonal ? blocks::stone : vx::BlockState::air);
                    const vx::QuadFields quad =
                        quad_of(chunk.mesh(), vx::Pass::opaque, face, block);
                    INFO("block (", block.x, ", ", block.y, ", ", block.z, ") face ", face_at,
                         " corner ", corner, " arrangement ", arrangement);
                    CHECK(quad.occlusion[static_cast<std::size_t>(corner)] ==
                          lysenko(side1, side2, diagonal));
                    // The corner across the face touches none of them.
                    CHECK(quad.occlusion[static_cast<std::size_t>((corner + 2) % 4)] == 3);
                    set(sides[0], vx::BlockState::air);
                    set(sides[1], vx::BlockState::air);
                    set(across, vx::BlockState::air);
                }
            }
        }
        chunk.put(block.x, block.y, block.z, vx::BlockState::air);
    }
}

TEST_CASE("cutout blocks cast no ambient occlusion") {
    Fixture chunk;
    // A floor with leaves and glass standing on it: the floor's top faces
    // around them stay fully lit.
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 5; ++x) {
            chunk.put(x, 0, z, blocks::stone);
        }
    }
    chunk.put(2, 1, 2, blocks::leaves);
    chunk.put(3, 1, 2, blocks::glass);
    const vx::ChunkMesh lit = chunk.mesh();
    for (const vx::Quad quad : lit.range(vx::Pass::opaque, vx::Face::pos_y)) {
        CHECK(vx::decode_quad(quad).occlusion == std::array<std::uint8_t, 4>{3, 3, 3, 3});
    }
    // Stone in their place darkens the faces beside it.
    chunk.put(2, 1, 2, blocks::stone);
    const vx::QuadFields beside =
        quad_of(chunk.mesh(), vx::Pass::opaque, vx::Face::pos_y, {.x = 1, .y = 0, .z = 2});
    // +y faces: width along x, height along z. The block at x = 2 sits beside
    // corners 1 and 2, the ones at the high end of x.
    CHECK(beside.occlusion == std::array<std::uint8_t, 4>{3, 2, 2, 3});
}

namespace {

// Each face a mesh covers, with the material it gets: texture and
// occlusion. A merged quad's faces get the quad's.
using MaterialFace =
    std::tuple<int, int, int, int, int, std::uint32_t>; // pass, face, x, y, z, material

std::vector<MaterialFace> material_faces_of(const vx::ChunkMesh& mesh) {
    std::vector<MaterialFace> faces;
    for (int pass = 0; pass < 2; ++pass) {
        for (int face = 0; face < 6; ++face) {
            for (const vx::Quad quad :
                 mesh.range(static_cast<vx::Pass>(pass), static_cast<vx::Face>(face))) {
                const vx::QuadFields fields = vx::decode_quad(quad);
                const vx::FaceAxes axes = vx::face_axes(fields.face);
                for (int j = 0; j < fields.height; ++j) {
                    for (int i = 0; i < fields.width; ++i) {
                        std::array<int, 3> at{fields.block.x, fields.block.y, fields.block.z};
                        at[axes.width] += i;
                        at[axes.height] += j;
                        faces.emplace_back(pass, static_cast<int>(fields.face), at[0], at[1], at[2],
                                           quad.material);
                    }
                }
            }
        }
    }
    std::ranges::sort(faces);
    return faces;
}

// Boxes of random blocks in random places: faces that can merge, broken up
// by other textures, by occlusion and by cutouts.
void fill_with_boxes(vx::PaddedChunk& padded, std::mt19937& generator, int boxes) {
    padded.blocks.fill(vx::BlockState::air);
    std::uniform_int_distribution<int> corner(0, vx::padded_size - 1);
    std::uniform_int_distribution<int> size(1, 12);
    std::uniform_int_distribution<int> kind(0, 6);
    for (int box = 0; box < boxes; ++box) {
        const int x0 = corner(generator);
        const int y0 = corner(generator);
        const int z0 = corner(generator);
        const int width = size(generator);
        const int height = size(generator);
        const int depth = size(generator);
        const auto state = static_cast<vx::BlockState>(kind(generator));
        for (int y = y0; y < std::min(y0 + height, vx::padded_size); ++y) {
            for (int z = z0; z < std::min(z0 + depth, vx::padded_size); ++z) {
                for (int x = x0; x < std::min(x0 + width, vx::padded_size); ++x) {
                    padded.blocks[vx::padded_index(x, y, z)] = state;
                }
            }
        }
    }
}

} // namespace

TEST_CASE("two blocks side by side merge into six quads, four of them two blocks wide") {
    Fixture chunk;
    chunk.put(5, 6, 7, blocks::stone);
    chunk.put(6, 6, 7, blocks::stone);
    const vx::ChunkMesh mesh = chunk.mesh();
    REQUIRE(quad_count(mesh) == 6);
    int two_wide = 0;
    for (const vx::Quad quad : mesh.all()) {
        const vx::QuadFields fields = vx::decode_quad(quad);
        two_wide += fields.width * fields.height == 2 ? 1 : 0;
    }
    CHECK(two_wide == 4);
}

TEST_CASE("a full chunk in air becomes six quads of 32 by 32") {
    Fixture chunk;
    for (int y = 0; y < vx::chunk_size; ++y) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            for (int x = 0; x < vx::chunk_size; ++x) {
                chunk.put(x, y, z, blocks::stone);
            }
        }
    }
    const vx::ChunkMesh mesh = chunk.mesh();
    for (int face = 0; face < 6; ++face) {
        const auto range = mesh.range(vx::Pass::opaque, static_cast<vx::Face>(face));
        REQUIRE(range.size() == 1);
        const vx::QuadFields quad = vx::decode_quad(range[0]);
        CHECK(quad.width == 32);
        CHECK(quad.height == 32);
        // The block with the smallest coordinates: on the far side for
        // the faces looking along +x, +y and +z.
        const int far = face % 2 == 0 ? 31 : 0;
        CHECK(quad.block == vx::LocalPos{.x = face / 2 == 0 ? far : 0,
                                         .y = face / 2 == 1 ? far : 0,
                                         .z = face / 2 == 2 ? far : 0});
        CHECK(quad.occlusion == std::array<std::uint8_t, 4>{3, 3, 3, 3});
    }
}

TEST_CASE("faces merge only where they look the same") {
    Fixture chunk;
    SUBCASE("two textures in a row: two tops") {
        chunk.put(1, 1, 1, blocks::stone);
        chunk.put(2, 1, 1, blocks::grass);
        CHECK(chunk.mesh().range(vx::Pass::opaque, vx::Face::pos_y).size() == 2);
    }
    SUBCASE("a block on a floor breaks the floor's top into pieces lit alike") {
        for (int z = 0; z < 5; ++z) {
            for (int x = 0; x < 5; ++x) {
                chunk.put(x, 0, z, blocks::stone);
            }
        }
        chunk.put(2, 1, 2, blocks::stone);
        // The ring of 8 faces around the block is darkened, each in its own
        // way, and the outer ring of 16 is fully lit: with the block's own
        // top, 25 faces up without merging. Merged, fewer, and each merged
        // quad's faces lit alike. (A mesh lives in the scratch, so each is
        // read before the next is made.)
        const std::size_t merged_tops =
            chunk.mesh().range(vx::Pass::opaque, vx::Face::pos_y).size();
        const std::vector<MaterialFace> merged = material_faces_of(chunk.mesh());
        CHECK(chunk.unmerged().range(vx::Pass::opaque, vx::Face::pos_y).size() == 25);
        CHECK(merged_tops < 25);
        CHECK(merged == material_faces_of(chunk.unmerged()));
    }
    SUBCASE("a cutout's faces merge with its own kind, in their own pass") {
        for (int x = 0; x < 4; ++x) {
            chunk.put(x, 1, 1, blocks::glass);
        }
        const vx::ChunkMesh mesh = chunk.mesh();
        CHECK(mesh.pass(vx::Pass::cutout).size() == 6);
        CHECK(mesh.pass(vx::Pass::opaque).empty());
    }
}

TEST_CASE(
    "merged quads cover exactly the faces of the unmerged mesh, each once, looking the same") {
    // RFC 0003's coverage property: on every chunk it tries, merging adds,
    // drops and changes no face.
    Fixture chunk;
    std::mt19937 generator(11);
    std::size_t merged_total = 0;
    std::size_t unmerged_total = 0;
    for (const int boxes : {3, 10, 30, 80}) {
        for (int round = 0; round < 10; ++round) {
            fill_with_boxes(*chunk.padded, generator, boxes);
            const std::vector<MaterialFace> unmerged = material_faces_of(chunk.unmerged());
            unmerged_total += chunk.unmerged().all().size();
            const vx::ChunkMesh merged = chunk.mesh();
            merged_total += merged.all().size();
            INFO(boxes, " boxes, round ", round);
            CHECK(material_faces_of(merged) == unmerged);
        }
    }
    // And merging did merge.
    CHECK(merged_total * 2 < unmerged_total);
}
