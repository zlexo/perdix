// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_double3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr vx::render::Extent image_size = {.width = 64, .height = 64};
constexpr vx::render::Color black = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
constexpr float quarter_turn = std::numbers::pi_v<float> / 2.0F;

using Pixel = std::array<std::uint8_t, 4>;

// A texture's four quarters, top left, top right, bottom left, bottom right,
// as RGBA, the same at every level.
using Quarters = std::array<Pixel, 4>;

// Block texture array bytes, as build_block_textures lays them out: layer 0
// a magenta that no face here asks for, then one layer for each entry.
std::vector<std::byte> texture_bytes(std::span<const Quarters> layers) {
    const std::size_t count = layers.size() + 1;
    std::vector<std::byte> bytes(count * vx::render::block_texture_layer_bytes);
    for (std::size_t level = 0; level < 5; ++level) {
        const std::size_t side = std::size_t{16} >> level;
        for (std::size_t layer = 0; layer < count; ++layer) {
            for (std::size_t texel = 0; texel < side * side; ++texel) {
                Pixel color{255, 0, 255, 255};
                if (layer > 0 && side > 1) {
                    const std::size_t x = texel % side;
                    const std::size_t y = texel / side;
                    const std::size_t quarter = (y * 2 >= side ? 2 : 0) + (x * 2 >= side ? 1 : 0);
                    color = layers[layer - 1][quarter];
                } else if (layer > 0) {
                    color = layers[layer - 1][0];
                }
                const std::size_t at =
                    4 * ((count * vx::render::block_texture_level_start.at(level)) +
                         (layer * side * side) + texel);
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    bytes[at + channel] = std::byte{color[channel]};
                }
            }
        }
    }
    return bytes;
}

vx::Result<vx::render::Renderer> renderer_with(std::span<const std::byte> textures) {
    return vx::render::Renderer::create({.surface = std::nullopt,
                                         .framebuffer = image_size,
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo,
                                         .overlay = nullptr,
                                         .block_textures = textures});
}

constexpr Pixel red = {255, 0, 0, 255};
constexpr Pixel green = {0, 255, 0, 255};
constexpr Pixel blue = {0, 0, 255, 255};
constexpr Pixel white = {255, 255, 255, 255};
constexpr Pixel see_through = {255, 0, 0, 0}; // alpha 0: a hole where the alpha test runs

// Which of red, green, blue or white a lit pixel is, darkened as it may be
// by its face's shade; '?' for anything else, the black sky among them.
char color_of(const Pixel& pixel) {
    const bool r = pixel[0] > 100;
    const bool g = pixel[1] > 100;
    const bool b = pixel[2] > 100;
    if (r && g && b) {
        return 'W';
    }
    const int lit = int{r} + int{g} + int{b};
    if (lit == 1 && (pixel[0] < 20 || r) && (pixel[1] < 20 || g) && (pixel[2] < 20 || b)) {
        return r ? 'R' : (g ? 'G' : 'B');
    }
    return '?';
}

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image_size.width) + x) * 4;
    return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
}

// What a renderer draws for `camera`, read back.
std::vector<std::uint8_t> draw(vx::render::Renderer& renderer, const vx::render::Camera& camera) {
    const vx::Result<vx::render::Frame> frame =
        renderer.render({.sky = black, .camera = camera, .cubes = {}});
    REQUIRE(frame.has_value());
    vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());
    return std::move(pixels).value();
}

// A chunk holding one block, of state 1, at (16, 16, 16): its six faces.
struct OneBlock {
    std::vector<vx::BlockVisual> visuals;
    std::unique_ptr<vx::PaddedChunk> padded = std::make_unique<vx::PaddedChunk>();
    vx::MeshScratch scratch;

    explicit OneBlock(vx::Opacity opacity) {
        visuals.resize(2);
        visuals[1] = {.opacity = opacity, .cull_group = 0, .texture = {1, 1, 1, 1, 1, 1}};
        padded->blocks[vx::padded_index(17, 17, 17)] = static_cast<vx::BlockState>(1);
    }
    [[nodiscard]] vx::ChunkMesh mesh() { return vx::mesh_chunk(*padded, visuals, scratch); }
};

// A camera 1.2 blocks outside the middle of `face` of the block at
// (16, 16, 16), looking straight at it with the face's image upright: for
// side faces, up is up; looking down at the top, up on the screen is north
// (-z), and looking up at the bottom, south (+z), as RFC 0004 turns those
// images.
vx::render::Camera facing(vx::Face face) {
    constexpr double middle = 16.5;
    constexpr double away = 0.5 + 1.2;
    vx::render::Camera camera;
    camera.position = {middle, middle, middle};
    switch (face) {
    case vx::Face::pos_x:
        camera.position.x += away;
        camera.yaw = quarter_turn;
        break;
    case vx::Face::neg_x:
        camera.position.x -= away;
        camera.yaw = -quarter_turn;
        break;
    case vx::Face::pos_y:
        camera.position.y += away;
        camera.pitch = -quarter_turn;
        break;
    case vx::Face::neg_y:
        camera.position.y -= away;
        camera.pitch = quarter_turn;
        break;
    case vx::Face::pos_z:
        camera.position.z += away;
        break;
    case vx::Face::neg_z:
        camera.position.z -= away;
        camera.yaw = 2.0F * quarter_turn;
        break;
    }
    return camera;
}

constexpr std::array<vx::Face, 6> faces{vx::Face::pos_x, vx::Face::neg_x, vx::Face::pos_y,
                                        vx::Face::neg_y, vx::Face::pos_z, vx::Face::neg_z};

// A mesh of `quads` alone, all in one pass and one face's range; the other
// ranges empty, each where the one before it ends, as the mesher lays them.
vx::ChunkMesh mesh_of(std::span<const vx::Quad> quads, vx::Pass pass, vx::Face face) {
    vx::ChunkMesh mesh{};
    std::size_t at = 0;
    for (std::size_t p = 0; p < 2; ++p) {
        for (std::size_t f = 0; f < 6; ++f) {
            const bool chosen =
                p == static_cast<std::size_t>(pass) && f == static_cast<std::size_t>(face);
            mesh.ranges[p][f] = quads.subspan(at, chosen ? quads.size() : 0);
            at += chosen ? quads.size() : 0;
        }
    }
    return mesh;
}

} // namespace

TEST_CASE("every face of a block shows from outside, its image unmirrored and the right way up") {
    // Four colors in the texture's four quarters: each face, seen head on,
    // must show them in the same places on the screen.
    const std::array<Quarters, 1> layers = {Quarters{red, green, blue, white}};
    const std::vector<std::byte> textures = texture_bytes(layers);
    vx::Result<vx::render::Renderer> created = renderer_with(textures);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    OneBlock block(vx::Opacity::opaque);
    REQUIRE(renderer.set_chunk({.x = 0, .y = 0, .z = 0}, block.mesh()).has_value());

    for (const vx::Face face : faces) {
        INFO("face ", static_cast<int>(face));
        const std::vector<std::uint8_t> pixels = draw(renderer, facing(face));
        // Each quarter's middle: a quarter block from the face's middle,
        // about 10 pixels at this distance.
        CHECK(color_of(pixel_at(pixels, 22, 22)) == 'R');
        CHECK(color_of(pixel_at(pixels, 42, 22)) == 'G');
        CHECK(color_of(pixel_at(pixels, 22, 42)) == 'B');
        CHECK(color_of(pixel_at(pixels, 42, 42)) == 'W');
        // Around it, the sky.
        CHECK(pixel_at(pixels, 1, 1) == Pixel{0, 0, 0, 255});
    }
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("a quad splits along its brighter diagonal on the GPU, as on the CPU") {
    // Two +z quads, white, side by side, darkened at opposite corners: at
    // corners 1 and 3, then at corners 0 and 2. Split along the brighter
    // diagonal, the middle of each lies on it and is lit; split along the
    // dark one, it would be at 0.4 of the light.
    const std::array<Quarters, 1> layers = {Quarters{white, white, white, white}};
    const std::vector<std::byte> textures = texture_bytes(layers);
    vx::Result<vx::render::Renderer> created = renderer_with(textures);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    for (const std::array<std::uint8_t, 4> occlusion :
         {std::array<std::uint8_t, 4>{3, 0, 3, 0}, std::array<std::uint8_t, 4>{0, 3, 0, 3}}) {
        INFO("occlusion ", int{occlusion[0]}, int{occlusion[1]}, int{occlusion[2]},
             int{occlusion[3]});
        const std::array<vx::Quad, 1> quad = {vx::encode_quad({.block = {.x = 16, .y = 16, .z = 16},
                                                               .width = 1,
                                                               .height = 1,
                                                               .face = vx::Face::pos_z,
                                                               .texture = 1,
                                                               .occlusion = occlusion})};
        REQUIRE(renderer
                    .set_chunk({.x = 0, .y = 0, .z = 0},
                               mesh_of(quad, vx::Pass::opaque, vx::Face::pos_z))
                    .has_value());
        const std::vector<std::uint8_t> pixels = draw(renderer, facing(vx::Face::pos_z));
        // A +z face's shade is 0.8 of the light, which sRGB stores as 231.
        const Pixel middle = pixel_at(pixels, 32, 32);
        CHECK(middle[0] >= 215);
        // A dark corner's own quarter is darker.
        CHECK(pixel_at(pixels, occlusion[0] == 0 ? 20 : 44, 44)[0] < middle[0]);
    }
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("a cutout block's see-through texels are holes, an opaque block's are not") {
    // The left half see-through, the right half white.
    const std::array<Quarters, 1> layers = {Quarters{see_through, white, see_through, white}};
    const std::vector<std::byte> textures = texture_bytes(layers);
    vx::Result<vx::render::Renderer> created = renderer_with(textures);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    OneBlock cutout(vx::Opacity::cutout);
    REQUIRE(renderer.set_chunk({.x = 0, .y = 0, .z = 0}, cutout.mesh()).has_value());
    std::vector<std::uint8_t> pixels = draw(renderer, facing(vx::Face::pos_z));
    // Through the hole, past the block's far faces, which face away, the sky.
    CHECK(pixel_at(pixels, 22, 32) == Pixel{0, 0, 0, 255});
    CHECK(color_of(pixel_at(pixels, 42, 32)) == 'W');

    // The opaque pass draws the same texels whole, in the color they hold.
    OneBlock opaque(vx::Opacity::opaque);
    REQUIRE(renderer.set_chunk({.x = 0, .y = 0, .z = 0}, opaque.mesh()).has_value());
    pixels = draw(renderer, facing(vx::Face::pos_z));
    CHECK(color_of(pixel_at(pixels, 22, 32)) == 'R');
    CHECK(color_of(pixel_at(pixels, 42, 32)) == 'W');
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("a chunk is drawn where it is, and an empty mesh leaves nothing") {
    const std::array<Quarters, 1> layers = {Quarters{white, white, white, white}};
    const std::vector<std::byte> textures = texture_bytes(layers);
    vx::Result<vx::render::Renderer> created = renderer_with(textures);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    OneBlock block(vx::Opacity::opaque);

    // The chunk at (-3, 2, 5), its block at (-96 + 16, 64 + 16, 160 + 16):
    // seen from where that block is, and from where the block of the
    // origin's chunk would be, which is empty.
    const vx::ChunkPos away{.x = -3, .y = 2, .z = 5};
    REQUIRE(renderer.set_chunk(away, block.mesh()).has_value());
    vx::render::Camera there = facing(vx::Face::pos_z);
    there.position += glm::dvec3(-96.0, 64.0, 160.0);
    CHECK(color_of(pixel_at(draw(renderer, there), 32, 32)) == 'W');
    CHECK(pixel_at(draw(renderer, facing(vx::Face::pos_z)), 32, 32) == Pixel{0, 0, 0, 255});

    REQUIRE(renderer.set_chunk(away, vx::ChunkMesh{}).has_value());
    CHECK(pixel_at(draw(renderer, there), 32, 32) == Pixel{0, 0, 0, 255});
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

namespace {

// Block texture array bytes: layer 0 left empty, never drawn, and layer 1 a checker of single
// black and white texels at every level but the last, which is their gray.
std::vector<std::byte> single_texel_checker() {
    std::vector<std::byte> bytes(2 * vx::render::block_texture_layer_bytes);
    for (std::size_t level = 0; level < 5; ++level) {
        const std::size_t side = std::size_t{16} >> level;
        for (std::size_t texel = 0; texel < side * side; ++texel) {
            const bool light = ((texel % side) + (texel / side)) % 2 == 0;
            Pixel color = light ? white : Pixel{0, 0, 0, 255};
            if (side == 1) {
                color = {188, 188, 188, 255}; // half the light, in sRGB
            }
            const std::size_t at =
                4 * ((2 * vx::render::block_texture_level_start.at(level)) + (side * side) + texel);
            for (std::size_t channel = 0; channel < 4; ++channel) {
                bytes[at + channel] = std::byte{color[channel]};
            }
        }
    }
    return bytes;
}

// How many pixels of a row mix red and green: the blend between the
// texture's red and green quarters, which meet down the middle of a face
// seen head on.
int mixed_pixels(const std::vector<std::uint8_t>& pixels, std::uint32_t row) {
    int mixed = 0;
    for (std::uint32_t x = 0; x < image_size.width; ++x) {
        const Pixel pixel = pixel_at(pixels, x, row);
        mixed += pixel[0] > 40 && pixel[1] > 40 ? 1 : 0;
    }
    return mixed;
}

} // namespace

TEST_CASE("up close, the sharpened filter keeps texels crisp, blending only at their edges") {
    // Seen from 0.6 blocks, a texel spans about 5 pixels: plain linear
    // filtering would blend red into green across about as many, nearest
    // across none, and the sharpened filter across the one pixel the edge
    // falls in, or two.
    const std::array<Quarters, 1> layers = {Quarters{red, green, blue, white}};
    const std::vector<std::byte> textures = texture_bytes(layers);
    vx::Result<vx::render::Renderer> created = renderer_with(textures);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    OneBlock block(vx::Opacity::opaque);
    REQUIRE(renderer.set_chunk({.x = 0, .y = 0, .z = 0}, block.mesh()).has_value());
    // A quarter of a pixel off the middle (a pixel is about 1/76 of a block
    // here), so one pixel's center lies within half a pixel of the edge:
    // with the edge exactly between two pixels, none would blend.
    vx::render::Camera close = facing(vx::Face::pos_z);
    close.position.z = 17.6;
    close.position.x += 0.0033;

    CHECK(renderer.block_filter() == vx::render::BlockFilter::sharpened);
    for (const vx::render::BlockFilter filter :
         {vx::render::BlockFilter::sharpened, vx::render::BlockFilter::sharpened_isotropic}) {
        renderer.set_block_filter(filter);
        const std::vector<std::uint8_t> pixels = draw(renderer, close);
        INFO("filter ", static_cast<int>(filter));
        CHECK(mixed_pixels(pixels, 16) >= 1);
        CHECK(mixed_pixels(pixels, 16) <= 2);
    }
    renderer.set_block_filter(vx::render::BlockFilter::nearest);
    CHECK(mixed_pixels(draw(renderer, close), 16) == 0);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("binding 3 filters anisotropically where the device offers it, binding 4 does not") {
    // A floor of black and white texels seen across, nearly edge on: the
    // texels shrink far more along the view than across it, where
    // anisotropic filtering keeps detail that trilinear filtering blurs.
    vx::Result<vx::render::Renderer> created = renderer_with(single_texel_checker());
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    // A floor 32 by 32 at y = 0, its tops facing up.
    std::vector<vx::BlockVisual> visuals(2);
    visuals[1] = {.opacity = vx::Opacity::opaque, .cull_group = 0, .texture = {1, 1, 1, 1, 1, 1}};
    auto padded = std::make_unique<vx::PaddedChunk>();
    for (int z = 0; z < vx::chunk_size; ++z) {
        for (int x = 0; x < vx::chunk_size; ++x) {
            padded->blocks[vx::padded_index(x + 1, 1, z + 1)] = static_cast<vx::BlockState>(1);
        }
    }
    vx::MeshScratch scratch;
    REQUIRE(renderer
                .set_chunk({.x = 0, .y = 0, .z = 0},
                           vx::mesh_chunk(*padded, visuals, scratch, {.merge = false}))
                .has_value());
    vx::render::Camera across;
    across.position = {16.0, 1.4, 31.5};
    across.pitch = -0.12F;

    renderer.set_block_filter(vx::render::BlockFilter::sharpened);
    const std::vector<std::uint8_t> anisotropic = draw(renderer, across);
    renderer.set_block_filter(vx::render::BlockFilter::sharpened_isotropic);
    const std::vector<std::uint8_t> isotropic = draw(renderer, across);
    // How far apart the two images are, on average, over the floor's far half.
    long difference = 0;
    for (std::uint32_t y = 26; y < 40; ++y) {
        for (std::uint32_t x = 0; x < image_size.width; ++x) {
            difference +=
                std::abs(int{pixel_at(anisotropic, x, y)[0]} - int{pixel_at(isotropic, x, y)[0]});
        }
    }
    const float anisotropy = renderer.textures().block_anisotropy();
    INFO("anisotropy ", static_cast<double>(anisotropy), ", difference ", difference);
    if (anisotropy > 1.0F) {
        CHECK(difference > 2'000);
    } else {
        CHECK(difference == 0);
    }
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("occlusion strength 0 leaves dark corners lit, 1 darkens them fully, the default") {
    // A white +z quad dark at every corner: 0.4 of its shade at full
    // strength, all of it at none.
    const std::array<Quarters, 1> layers = {Quarters{white, white, white, white}};
    const std::vector<std::byte> textures = texture_bytes(layers);
    vx::Result<vx::render::Renderer> created = renderer_with(textures);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    const std::array<vx::Quad, 1> quad = {vx::encode_quad({.block = {.x = 16, .y = 16, .z = 16},
                                                           .width = 1,
                                                           .height = 1,
                                                           .face = vx::Face::pos_z,
                                                           .texture = 1,
                                                           .occlusion = {0, 0, 0, 0}})};
    REQUIRE(
        renderer
            .set_chunk({.x = 0, .y = 0, .z = 0}, mesh_of(quad, vx::Pass::opaque, vx::Face::pos_z))
            .has_value());
    const auto middle = [&] { return pixel_at(draw(renderer, facing(vx::Face::pos_z)), 32, 32); };

    CHECK(renderer.occlusion_strength() == 1.0F);
    // 0.8 of the light, the +z shade, is sRGB 231; 0.4 of it, sRGB 153.
    const Pixel full = middle();
    CHECK(full[0] >= 146);
    CHECK(full[0] <= 160);
    renderer.set_occlusion_strength(0.5F);
    const Pixel half = middle(); // 0.7 of the shade: sRGB 197
    CHECK(half[0] >= 190);
    CHECK(half[0] <= 204);
    renderer.set_occlusion_strength(0.0F);
    CHECK(middle()[0] >= 225);

    // Outside 0 to 1, the nearer end.
    renderer.set_occlusion_strength(2.0F);
    CHECK(renderer.occlusion_strength() == 1.0F);
    renderer.set_occlusion_strength(-1.0F);
    CHECK(renderer.occlusion_strength() == 0.0F);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}
