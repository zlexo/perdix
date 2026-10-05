// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.

#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_int3_sized.hpp>
#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr vx::render::Extent image_size = {.width = 64, .height = 32};
constexpr vx::render::Color sky = {.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F};

// An ImGui context for one test. Declared before the renderer, so it is
// destroyed after it: the renderer shuts ImGui's backend down on it.
class OverlayContext {
public:
    OverlayContext() : context_(ImGui::CreateContext()) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize =
            ImVec2(static_cast<float>(image_size.width), static_cast<float>(image_size.height));
        io.DeltaTime = 1.0F / 60.0F;
        io.IniFilename = nullptr; // no imgui.ini from tests
    }
    OverlayContext(const OverlayContext&) = delete;
    OverlayContext& operator=(const OverlayContext&) = delete;
    ~OverlayContext() { ImGui::DestroyContext(context_); }

    [[nodiscard]] ImGuiContext* get() const noexcept { return context_; }

private:
    ImGuiContext* context_;
};

vx::Result<vx::render::Renderer> overlay_renderer(ImGuiContext* overlay) {
    return vx::render::Renderer::create({.surface = std::nullopt,
                                         .framebuffer = image_size,
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo,
                                         .overlay = overlay,
                                         .block_textures = {}});
}

// One cube in the middle of the image, two blocks away.
vx::render::Camera in_front() {
    vx::render::Camera camera;
    camera.position = {0.5, 0.5, 3.0};
    return camera;
}

using Pixel = std::array<std::uint8_t, 4>;

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image_size.width + x) * 4;
    return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
}

// Within one step in every channel, a margin for how drivers round to sRGB.
bool near(const Pixel& pixel, const Pixel& expected) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(int{pixel.at(channel)} - int{expected.at(channel)}) > 1) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("the overlay draws over the frame, which shows around it") {
    const OverlayContext overlay;
    vx::Result<vx::render::Renderer> created = overlay_renderer(overlay.get());
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    // A magenta square in the top left corner, straight on the foreground
    // draw list: no window, and no glyph from the font.
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(4.0F, 4.0F), ImVec2(20.0F, 20.0F),
                                                  IM_COL32(255, 0, 255, 255));
    ImGui::Render();
    const std::array cubes = {glm::i64vec3(0, 0, 0)};
    REQUIRE(
        renderer
            .render(
                {.sky = sky, .camera = in_front(), .cubes = cubes, .overlay = ImGui::GetDrawData()})
            .has_value());
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());

    CHECK(near(pixel_at(pixels.value(), 12, 12), {255, 0, 255, 255}));
    // Around it, the frame the overlay drew over: the sky, and the cube's
    // front face, blue, in the middle.
    CHECK(near(pixel_at(pixels.value(), 60, 2), {0, 255, 0, 255}));
    const Pixel middle = pixel_at(pixels.value(), 32, 16);
    CHECK(middle.at(0) <= 1);
    CHECK(middle.at(1) <= 1);
    CHECK(middle.at(2) >= 140);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("the overlay's windows and text draw, the font uploaded under validation") {
    const OverlayContext overlay;
    vx::Result<vx::render::Renderer> created = overlay_renderer(overlay.get());
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    // Two frames: the first uploads the font, the second draws with it.
    const std::array cubes = {glm::i64vec3(0, 0, 0)};
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(64.0F, 32.0F));
        ImGui::Begin("Overlay");
        ImGui::TextUnformatted("Perdix");
        ImGui::End();
        ImGui::Render();
        REQUIRE(renderer
                    .render({.sky = sky,
                             .camera = in_front(),
                             .cubes = cubes,
                             .overlay = ImGui::GetDrawData()})
                    .has_value());
    }
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());

    // The window covers the image, so not one pixel is left of the green sky.
    std::size_t sky_pixels = 0;
    for (std::uint32_t y = 0; y < image_size.height; ++y) {
        for (std::uint32_t x = 0; x < image_size.width; ++x) {
            if (near(pixel_at(pixels.value(), x, y), {0, 255, 0, 255})) {
                ++sky_pixels;
            }
        }
    }
    CHECK(sky_pixels == 0);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("an overlay for a renderer made without one is an error, not a draw") {
    vx::Result<vx::render::Renderer> created = overlay_renderer(nullptr);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());

    // Empty draw lists: the renderer turns the frame down before reading them.
    ImDrawData empty;
    const vx::Result<vx::render::Frame> drawn =
        created.value().render({.sky = sky, .camera = in_front(), .cubes = {}, .overlay = &empty});
    REQUIRE_FALSE(drawn.has_value());
    CHECK(drawn.error().message ==
          "a frame's overlay needs a renderer made with an overlay context");
}
