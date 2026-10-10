// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/core/vec3d.hpp>
#include <vx/mesh/name_tag.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>
#include <vx/world/font.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_int3_sized.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// RFC 0012's test plan: one tag behind a wall, visible at half white, and in
// the open at full white; and the game's rules for when a tag shows.

namespace {

constexpr std::uint32_t size = 128;
constexpr vx::render::Color sky = {.r = 0.2F, .g = 0.4F, .b = 1.0F, .a = 1.0F};

using Pixel = std::array<std::uint8_t, 4>;

// A font whose A is a solid white square, its whole cell ink, advancing 9;
// every other character draws nothing.
std::unique_ptr<vx::Font> square_font() {
    auto font = std::make_unique<vx::Font>();
    font->advances.fill(6);
    font->advances.at('A') = 9;
    font->advances.at(' ') = 4;
    constexpr int code = 'A';
    for (int y = 0; y < vx::font_cell_size; ++y) {
        for (int x = 0; x < vx::font_cell_size; ++x) {
            const int column = ((code % 16) * vx::font_cell_size) + x;
            const int row = ((code / 16) * vx::font_cell_size) + y;
            const auto at = static_cast<std::size_t>((row * vx::font_atlas_size) + column) * 4;
            font->rgba.at(at) = 255;
            font->rgba.at(at + 1) = 255;
            font->rgba.at(at + 2) = 255;
            font->rgba.at(at + 3) = 255;
        }
    }
    return font;
}

vx::Result<vx::render::Renderer> tag_renderer(const vx::Font* font) {
    return vx::render::Renderer::create({.surface = std::nullopt,
                                         .framebuffer = {.width = size, .height = size},
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo,
                                         .overlay = nullptr,
                                         .block_textures = {},
                                         .skins = {},
                                         .font = font});
}

std::vector<std::uint8_t> draw(vx::render::Renderer& renderer, const vx::render::Camera& camera,
                               std::span<const glm::i64vec3> cubes, std::span<const vx::Tag> tags) {
    const vx::render::FrameInputs inputs = {
        .sky = sky, .camera = camera, .cubes = cubes, .players = {}, .tags = tags};
    REQUIRE(renderer.render(inputs).has_value());
    vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());
    return std::move(pixels).value();
}

struct OnImage {
    int x;
    int y;
};

OnImage project(const vx::render::Camera& camera, const vx::Vec3d& point) {
    const glm::vec4 clip = vx::render::view_projection(camera, 1.0F) *
                           glm::vec4(static_cast<float>(point.x - camera.position.x),
                                     static_cast<float>(point.y - camera.position.y),
                                     static_cast<float>(point.z - camera.position.z), 1.0F);
    const double x = (static_cast<double>(clip.x / clip.w) * 0.5) + 0.5;
    const double y = (static_cast<double>(clip.y / clip.w) * 0.5) + 0.5;
    return {.x = static_cast<int>(std::floor(x * size)),
            .y = static_cast<int>(std::floor(y * size))};
}

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, OnImage at) {
    const auto first = (static_cast<std::size_t>(at.y) * size + static_cast<std::size_t>(at.x)) * 4;
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

double decode(std::uint8_t byte) {
    const double c = byte / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

std::uint8_t encode(double linear) {
    const double c =
        linear <= 0.0031308 ? linear * 12.92 : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
    return static_cast<std::uint8_t>(std::lround(c * 255.0));
}

// What blending `color` at `alpha` over `under` gives, as the GPU blends
// into an sRGB target: in linear light.
Pixel over(const Pixel& under, double color, double alpha) {
    Pixel out = under;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        out.at(channel) = encode((color * alpha) + (decode(under.at(channel)) * (1.0 - alpha)));
    }
    return out;
}

bool near(const Pixel& pixel, const Pixel& expected) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(int{pixel.at(channel)} - int{expected.at(channel)}) > 2) {
            return false;
        }
    }
    return true;
}

// The game's colors: the box black at 63 of 255, the letters white at 128 of
// 255 through walls and at 255 where nothing is in front.
constexpr double box_alpha = 63.0 / 255.0;
constexpr double half_white = 128.0 / 255.0;

// The A tag's glyph middle and its box's left margin: the pen starts at
// -4.5 pixels, the box 1 further left; 0.025 blocks a pixel, rows down from
// the anchor.
vx::Vec3d glyph_middle(const vx::Vec3d& anchor) {
    return anchor + vx::Vec3d{.x = -0.5 * 0.025, .y = -4.0 * 0.025, .z = 0.0};
}
vx::Vec3d box_margin(const vx::Vec3d& anchor) {
    return anchor + vx::Vec3d{.x = -5.0 * 0.025, .y = -4.0 * 0.025, .z = 0.0};
}

} // namespace

TEST_CASE("a tag in the open shows its letters fully white over its box") {
    const std::unique_ptr<vx::Font> font = square_font();
    vx::Result<vx::render::Renderer> renderer = tag_renderer(font.get());
    INFO((renderer.has_value() ? std::string() : renderer.error().message));
    REQUIRE(renderer.has_value());
    vx::render::Camera camera;
    camera.position = {0.5, 66.2, 1.5};
    const vx::Vec3d anchor{.x = 0.5, .y = 66.3, .z = 0.5};
    const std::array tags = {vx::Tag{.anchor = anchor, .text = "A"}};

    const std::vector<std::uint8_t> before = draw(renderer.value(), camera, {}, {});
    const std::vector<std::uint8_t> seen = draw(renderer.value(), camera, {}, tags);
    const OnImage letter = project(camera, glyph_middle(anchor));
    const OnImage margin = project(camera, box_margin(anchor));
    INFO("letter at ", letter.x, ", ", letter.y, ", margin at ", margin.x, ", ", margin.y);
    CHECK(near(pixel_at(seen, letter), {255, 255, 255, 255}));
    // Only the pass seen through walls draws the box.
    CHECK(near(pixel_at(seen, margin), over(pixel_at(before, margin), 0.0, box_alpha)));
}

TEST_CASE("a tag behind a wall shows through it at half white and a discreet one not at all") {
    const std::unique_ptr<vx::Font> font = square_font();
    vx::Result<vx::render::Renderer> renderer = tag_renderer(font.get());
    INFO((renderer.has_value() ? std::string() : renderer.error().message));
    REQUIRE(renderer.has_value());
    vx::render::Camera camera;
    camera.position = {0.5, 66.2, 2.5};
    // A cube from z 0 to 1 between the camera and the tag, half a block
    // behind it.
    const std::array cubes = {glm::i64vec3(0, 65, 0), glm::i64vec3(0, 66, 0)};
    const vx::Vec3d anchor{.x = 0.5, .y = 66.3, .z = -0.5};

    const std::vector<std::uint8_t> wall = draw(renderer.value(), camera, cubes, {});
    const OnImage letter = project(camera, glyph_middle(anchor));
    const OnImage margin = project(camera, box_margin(anchor));
    INFO("letter at ", letter.x, ", ", letter.y);
    const std::array tags = {vx::Tag{.anchor = anchor, .text = "A"}};
    const std::vector<std::uint8_t> seen = draw(renderer.value(), camera, cubes, tags);
    const Pixel boxed = over(pixel_at(wall, letter), 0.0, box_alpha);
    CHECK(near(pixel_at(seen, letter), over(boxed, 1.0, half_white)));
    CHECK(near(pixel_at(seen, margin), over(pixel_at(wall, margin), 0.0, box_alpha)));

    // A sneaking player's tag is drawn once, where nothing is in front.
    const std::array discreet = {vx::Tag{.anchor = anchor, .text = "A", .discreet = true}};
    const std::vector<std::uint8_t> hidden = draw(renderer.value(), camera, cubes, discreet);
    CHECK(near(pixel_at(hidden, letter), pixel_at(wall, letter)));
    CHECK(near(pixel_at(hidden, margin), pixel_at(wall, margin)));
}

TEST_CASE("a discreet tag in the open shows its box and its letters at half") {
    const std::unique_ptr<vx::Font> font = square_font();
    vx::Result<vx::render::Renderer> renderer = tag_renderer(font.get());
    REQUIRE(renderer.has_value());
    vx::render::Camera camera;
    camera.position = {0.5, 66.2, 1.5};
    const vx::Vec3d anchor{.x = 0.5, .y = 66.3, .z = 0.5};
    const std::vector<std::uint8_t> before = draw(renderer.value(), camera, {}, {});
    const std::array tags = {vx::Tag{.anchor = anchor, .text = "A", .discreet = true}};
    const std::vector<std::uint8_t> seen = draw(renderer.value(), camera, {}, tags);
    const OnImage letter = project(camera, glyph_middle(anchor));
    const Pixel boxed = over(pixel_at(before, letter), 0.0, box_alpha);
    CHECK(near(pixel_at(seen, letter), over(boxed, 1.0, half_white)));
}

TEST_CASE("a tag shows within 64 blocks of the camera and a discreet one within 32") {
    const std::unique_ptr<vx::Font> font = square_font();
    vx::Result<vx::render::Renderer> renderer = tag_renderer(font.get());
    REQUIRE(renderer.has_value());
    // A narrow view, so a tag far away still covers pixels.
    vx::render::Camera camera;
    camera.position = {0.5, 66.2, 0.0};
    camera.fov_y = glm::radians(2.0F);
    const auto shows = [&](double distance, bool discreet) {
        const vx::Vec3d anchor{.x = 0.5, .y = 66.2 + (4.0 * 0.025), .z = -distance};
        const std::array tags = {vx::Tag{.anchor = anchor, .text = "A", .discreet = discreet}};
        const std::vector<std::uint8_t> seen = draw(renderer.value(), camera, {}, tags);
        const std::vector<std::uint8_t> before = draw(renderer.value(), camera, {}, {});
        return !near(pixel_at(seen, project(camera, glyph_middle(anchor))),
                     pixel_at(before, project(camera, glyph_middle(anchor))));
    };
    CHECK(shows(60.0, false));
    CHECK_FALSE(shows(66.0, false));
    CHECK(shows(30.0, true));
    CHECK_FALSE(shows(34.0, true));
}

TEST_CASE("without a font no tag is drawn and a frame draws at most 64 tags") {
    vx::Result<vx::render::Renderer> fontless = tag_renderer(nullptr);
    REQUIRE(fontless.has_value());
    vx::render::Camera camera;
    camera.position = {0.5, 66.2, 1.5};
    const vx::Vec3d anchor{.x = 0.5, .y = 66.3, .z = 0.5};
    const std::array tags = {vx::Tag{.anchor = anchor, .text = "A"}};
    const std::vector<std::uint8_t> before = draw(fontless.value(), camera, {}, {});
    const std::vector<std::uint8_t> seen = draw(fontless.value(), camera, {}, tags);
    CHECK(seen == before);

    const std::vector<vx::Tag> many(vx::render::Renderer::max_tags + 1,
                                    vx::Tag{.anchor = anchor, .text = "A"});
    const vx::Result<vx::render::Frame> frame = fontless.value().render(
        {.sky = sky, .camera = camera, .cubes = {}, .players = {}, .tags = many});
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error().message == "a frame draws at most 64 name tags, not 65");
}

TEST_CASE("two tags in one frame each show at their own place") {
    const std::unique_ptr<vx::Font> font = square_font();
    vx::Result<vx::render::Renderer> renderer = tag_renderer(font.get());
    REQUIRE(renderer.has_value());
    vx::render::Camera camera;
    camera.position = {0.5, 66.2, 2.5};
    // In different blocks, at different distances: the farther is drawn
    // first.
    const vx::Vec3d near_anchor{.x = 0.1, .y = 66.3, .z = 0.5};
    const vx::Vec3d far_anchor{.x = 1.6, .y = 66.3, .z = -1.5};
    const std::array tags = {vx::Tag{.anchor = near_anchor, .text = "A"},
                             vx::Tag{.anchor = far_anchor, .text = "A"}};
    const std::vector<std::uint8_t> seen = draw(renderer.value(), camera, {}, tags);
    CHECK(near(pixel_at(seen, project(camera, glyph_middle(near_anchor))), {255, 255, 255, 255}));
    CHECK(near(pixel_at(seen, project(camera, glyph_middle(far_anchor))), {255, 255, 255, 255}));
}
