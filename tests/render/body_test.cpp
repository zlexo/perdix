// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/core/vec3d.hpp>
#include <vx/mesh/player_model.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/fog.hpp>
#include <vx/render/renderer.hpp>
#include <vx/world/skin.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_double3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// RFC 0012's test plan: one body from a fixed camera, front and side, with
// checked pixels on the face, an arm and the background, and its silhouette
// inside the projected key points.

namespace {

constexpr std::uint32_t size = 128;
constexpr vx::render::Color sky = {.r = 0.2F, .g = 0.4F, .b = 1.0F, .a = 1.0F};

using Pixel = std::array<std::uint8_t, 4>;
using Texel = std::array<std::uint8_t, 4>;

constexpr Texel clear = {0, 0, 0, 0};
constexpr Texel gray = {128, 128, 128, 255};
constexpr Texel red = {255, 0, 0, 255};
constexpr Texel green = {0, 255, 0, 255};
constexpr Texel blue = {0, 0, 255, 255};
constexpr Texel yellow = {255, 255, 0, 255};
constexpr Texel cyan = {0, 255, 255, 255};

void fill(vx::Skin& skin, int left, int top, int right, int bottom, const Texel& texel) {
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const auto at = static_cast<std::size_t>((y * vx::skin_size) + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                skin.rgba.at(at + channel) = texel.at(channel);
            }
        }
    }
}

// A skin of solid colors where the test looks, on RFC 0012's layout: the
// face red, the chest green, the right arm's front blue and the left arm's
// outer side yellow; the head's top clear, so its bottom shows from above,
// from inside, cyan. The rest of the base layer is gray, the outer layer
// clear.
vx::Skin test_skin() {
    vx::Skin skin;
    fill(skin, 0, 0, 64, 64, clear);
    fill(skin, 0, 0, 32, 16, gray);
    fill(skin, 0, 16, 64, 32, gray);
    fill(skin, 16, 48, 48, 64, gray);
    fill(skin, 8, 0, 16, 8, clear);     // the head's top
    fill(skin, 16, 0, 24, 8, cyan);     // the head's bottom
    fill(skin, 8, 8, 16, 16, red);      // the head's front
    fill(skin, 20, 20, 28, 32, green);  // the body's front
    fill(skin, 44, 20, 48, 32, blue);   // the right arm's front
    fill(skin, 40, 52, 44, 64, yellow); // the left arm's left side
    return skin;
}

// The game's entity light on a face, as sRGB bytes of a channel at 255: 1
// up, 0.4 down, 0.7395 north and south, 0.497 east and west, then encoded
// for the sRGB target.
std::uint8_t lit(double light) {
    const double encoded =
        light <= 0.0031308 ? light * 12.92 : (1.055 * std::pow(light, 1.0 / 2.4)) - 0.055;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0));
}

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, int x, int y) {
    const auto at = (static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)) * 4;
    return {pixels.at(at), pixels.at(at + 1), pixels.at(at + 2), pixels.at(at + 3)};
}

// Within two steps in every color channel: each driver rounds its own way
// while encoding to sRGB.
bool near(const Pixel& pixel, const Pixel& expected) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(int{pixel.at(channel)} - int{expected.at(channel)}) > 2) {
            return false;
        }
    }
    return true;
}

const Pixel sky_pixel = {lit(0.2), lit(0.4), lit(1.0), 255};

// Where a world point lands on the image, as the renderer projects it.
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

vx::Result<vx::render::Renderer> body_renderer(std::span<const vx::Skin> skins) {
    return vx::render::Renderer::create({.surface = std::nullopt,
                                         .framebuffer = {.width = size, .height = size},
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo,
                                         .overlay = nullptr,
                                         .block_textures = {},
                                         .skins = skins});
}

std::vector<std::uint8_t> draw(vx::render::Renderer& renderer, const vx::render::Camera& camera,
                               std::span<const vx::PlayerPose> players,
                               const std::optional<vx::render::Fog>& fog = std::nullopt) {
    const vx::render::FrameInputs inputs = {
        .sky = sky, .camera = camera, .cubes = {}, .players = players, .fog = fog};
    REQUIRE(renderer.render(inputs).has_value());
    vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());
    return std::move(pixels).value();
}

// A body standing at (x, 64, 0.5) and facing +z, toward the front camera,
// or -z, its arms hanging still: the idle sway's roll is 0 at this age.
vx::PlayerPose standing(std::uint32_t skin, double x = 0.5, bool south = true) {
    const float yaw = south ? std::numbers::pi_v<float> : 0.0F;
    return {.feet = {.x = x, .y = 64.0, .z = 0.5},
            .age = std::numbers::pi / 0.09,
            .body_yaw = yaw,
            .head_yaw = yaw,
            .skin = skin};
}

} // namespace

TEST_CASE("a body draws in its skin from the front and the side, lit as the game lights it") {
    const std::array skins = {test_skin()};
    vx::Result<vx::render::Renderer> renderer = body_renderer(skins);
    INFO((renderer.has_value() ? std::string() : renderer.error().message));
    REQUIRE(renderer.has_value());
    const std::array players = {standing(0)};

    // From the front: three blocks south of the body, level with its chest,
    // looking north.
    vx::render::Camera front;
    front.position = {0.5, 65.0, 3.5};
    const std::vector<std::uint8_t> seen = draw(renderer.value(), front, players);
    // The face's middle, 1.64 above the feet, 0.23 in front of them: red, lit
    // as a face looking south is, 0.7395.
    const OnImage face = project(front, {.x = 0.5, .y = 65.64, .z = 0.73});
    INFO("face at ", face.x, ", ", face.y);
    CHECK(near(pixel_at(seen, face.x, face.y), {lit(0.7395), 0, 0, 255}));
    // The chest, green, and the right arm, blue: the player's right is to
    // the camera's left, at x 0.15.
    const OnImage chest = project(front, {.x = 0.5, .y = 65.05, .z = 0.62});
    CHECK(near(pixel_at(seen, chest.x, chest.y), {0, lit(0.7395), 0, 255}));
    const OnImage arm = project(front, {.x = 0.15, .y = 65.05, .z = 0.62});
    CHECK(arm.x < chest.x);
    CHECK(near(pixel_at(seen, arm.x, arm.y), {0, 0, lit(0.7395), 255}));
    // The sky around it, in every corner.
    for (const auto& [x, y] :
         {std::pair{2, 2}, std::pair{125, 2}, std::pair{2, 125}, std::pair{125, 125}}) {
        CHECK(near(pixel_at(seen, x, y), sky_pixel));
    }
    // Nothing of it outside its key points: the hat's top 1.905 above the
    // feet, the soles, and the arms 0.47 either side, 0.25 deep.
    const OnImage top_left = project(front, {.x = 0.5 - 0.47, .y = 64.0 + 1.91, .z = 0.75});
    const OnImage bottom_right = project(front, {.x = 0.5 + 0.47, .y = 64.0, .z = 0.75});
    int outside = 0;
    int inside = 0;
    for (int y = 0; y < static_cast<int>(size); ++y) {
        for (int x = 0; x < static_cast<int>(size); ++x) {
            const bool in_box = x >= top_left.x - 1 && x <= bottom_right.x + 1 &&
                                y >= top_left.y - 1 && y <= bottom_right.y + 1;
            const bool body = !near(pixel_at(seen, x, y), sky_pixel);
            outside += body && !in_box ? 1 : 0;
            inside += body && in_box ? 1 : 0;
        }
    }
    CHECK(outside == 0);
    CHECK(inside > 500);

    // From the side: three blocks east, looking west, sees the body's left
    // side, its left arm's outer face yellow, lit as a face looking east is,
    // 0.497.
    vx::render::Camera side;
    side.position = {3.5, 65.0, 0.5};
    side.yaw = std::numbers::pi_v<float> / 2.0F;
    const std::vector<std::uint8_t> from_side = draw(renderer.value(), side, players);
    const OnImage left_arm = project(side, {.x = 0.5 + 0.47, .y = 65.05, .z = 0.5});
    INFO("left arm at ", left_arm.x, ", ", left_arm.y);
    CHECK(near(pixel_at(from_side, left_arm.x, left_arm.y), {lit(0.497), lit(0.497), 0, 255}));
}

TEST_CASE("seen from above a body's tops are lit fully and its insides as the game lights them") {
    const std::array skins = {test_skin()};
    vx::Result<vx::render::Renderer> renderer = body_renderer(skins);
    INFO((renderer.has_value() ? std::string() : renderer.error().message));
    REQUIRE(renderer.has_value());
    const std::array players = {standing(0)};
    // Two blocks over the head, looking down.
    vx::render::Camera above;
    above.position = {0.5, 68.0, 0.5};
    above.pitch = -1.5F;
    const std::vector<std::uint8_t> seen = draw(renderer.value(), above, players);
    // Through the head's clear top, the inside of its bottom: a face seen
    // from inside its box is lit as if it faced the other way, so this one,
    // facing down, is lit as a top is, fully: cyan.
    const OnImage inside = project(above, {.x = 0.5, .y = 65.41, .z = 0.5});
    INFO("inside at ", inside.x, ", ", inside.y);
    CHECK(near(pixel_at(seen, inside.x, inside.y), {0, 255, 255, 255}));
    // The right arm's top, gray, lit fully as every top is.
    const OnImage arm = project(above, {.x = 0.5 - 0.38, .y = 65.41, .z = 0.5});
    INFO("arm at ", arm.x, ", ", arm.y);
    CHECK(near(pixel_at(seen, arm.x, arm.y), {128, 128, 128, 255}));
}

TEST_CASE(
    "each body has its own place pose and skin and a skin not given shows magenta and black") {
    const std::array skins = {test_skin()};
    vx::Result<vx::render::Renderer> renderer = body_renderer(skins);
    INFO((renderer.has_value() ? std::string() : renderer.error().message));
    REQUIRE(renderer.has_value());
    // Skin 1 of one, facing the camera; beside it, 1.5 blocks east, a body
    // in the test's skin with its back to the camera.
    const std::array players = {standing(1), standing(0, 2.0, false)};
    vx::render::Camera front;
    front.position = {1.25, 65.6, 2.6};
    const std::vector<std::uint8_t> seen = draw(renderer.value(), front, players);
    // The second body's head from behind: gray, its back facing south.
    const OnImage back = project(front, {.x = 2.0, .y = 65.64, .z = 0.735});
    INFO("back of the head at ", back.x, ", ", back.y);
    CHECK(near(pixel_at(seen, back.x, back.y),
               {lit(0.2158 * 0.7395), lit(0.2158 * 0.7395), lit(0.2158 * 0.7395), 255}));
    // The face's four squares of 4 texels, lit as a face looking south. Its
    // first texels are on the player's right, the camera's left (-x): the
    // square there at the top is magenta, the one beside it black.
    const Pixel magenta = {lit(0.7395), 0, lit(0.7395), 255};
    const Pixel black = {0, 0, 0, 255};
    const double quarter = 0.234 / 2.0; // a square's middle, from the face's
    const OnImage first = project(front, {.x = 0.5 - quarter, .y = 65.64 + quarter, .z = 0.735});
    const OnImage second = project(front, {.x = 0.5 + quarter, .y = 65.64 + quarter, .z = 0.735});
    INFO("squares at ", first.x, ", ", first.y, " and ", second.x, ", ", second.y);
    CHECK(near(pixel_at(seen, first.x, first.y), magenta));
    CHECK(near(pixel_at(seen, second.x, second.y), black));
}

TEST_CASE("a frame draws at most 64 bodies") {
    vx::Result<vx::render::Renderer> renderer = body_renderer({});
    REQUIRE(renderer.has_value());
    vx::render::Camera camera;
    std::vector<vx::PlayerPose> players(vx::render::Renderer::max_players, standing(0));
    CHECK(renderer.value()
              .render({.sky = sky, .camera = camera, .cubes = {}, .players = players})
              .has_value());
    players.push_back(standing(0));
    const vx::Result<vx::render::Frame> frame =
        renderer.value().render({.sky = sky, .camera = camera, .cubes = {}, .players = players});
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error().message == "a frame draws at most 64 bodies, not 65");
}

TEST_CASE("full fog turns a body the fog color, as 26.2's entity shader fogs it") {
    const std::array skins = {test_skin()};
    vx::Result<vx::render::Renderer> renderer = body_renderer(skins);
    INFO((renderer.has_value() ? std::string() : renderer.error().message));
    REQUIRE(renderer.has_value());
    const std::array players = {standing(0)};
    vx::render::Camera front;
    front.position = {0.5, 65.0, 3.5};
    const vx::render::Fog full{
        .color = {.r = 64, .g = 128, .b = 192},
        .ramps = {.haze_end = 0.01F, .band_start = 100.0F, .band_end = 200.0F}};
    const std::vector<std::uint8_t> seen = draw(renderer.value(), front, players, full);
    for (const OnImage at : {project(front, {.x = 0.5, .y = 65.64, .z = 0.73}),
                             project(front, {.x = 0.5, .y = 65.05, .z = 0.62})}) {
        CHECK(near(pixel_at(seen, at.x, at.y), {64, 128, 192, 255}));
    }
    CHECK(renderer.value().context().instance().validation_counts().errors == 0);
}
