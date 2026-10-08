// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.
//
// A swapchain needs a surface, and a surface usually needs a window, which CI
// does not have. VK_EXT_headless_surface makes one with no display behind it:
// its swapchain acquires and presents as a window's does, to nowhere, so the
// renderer's window path runs here under validation. Where a driver's
// headless surface cannot take the renderer's swapchain, the tests say why
// and CTest reports them skipped.

#include <vx/render/camera.hpp>
#include <vx/render/context.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_int3_sized.hpp>
#include <volk.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace {

constexpr vx::render::Extent surface_size = {.width = 64, .height = 32};
constexpr vx::render::Color sky = {.r = 0.0F, .g = 0.0F, .b = 1.0F, .a = 1.0F};

vx::render::SurfaceSource headless_surface() {
    static constexpr std::array<const char*, 2> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    return {
        .instance_extensions = extensions,
        .create = [](VkInstance instance) -> vx::Result<VkSurfaceKHR> {
            const VkHeadlessSurfaceCreateInfoEXT info = {
                .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
                .pNext = nullptr,
                .flags = 0,
            };
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (vkCreateHeadlessSurfaceEXT(instance, &info, nullptr, &surface) != VK_SUCCESS) {
                return vx::Error{"vkCreateHeadlessSurfaceEXT failed"};
            }
            return surface;
        },
    };
}

// Why this driver's headless surfaces cannot take the renderer's swapchain,
// if they cannot: Mesa's headless surfaces, lavapipe's among them, offer UNORM
// formats only, and the renderer draws to an sRGB one.
std::optional<std::string> surface_refuses_swapchain() {
    const vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = headless_surface(), .validation = true});
    INFO((context.has_value() ? std::string() : context.error().message));
    REQUIRE(context.has_value());
    const vx::render::SurfaceSupport support = vx::render::query_surface_support(
        context.value().device().physical_device(), context.value().surface());
    const vx::Result<vx::render::SwapchainChoice> choice =
        vx::render::choose_swapchain(support, surface_size, vx::render::PresentMode::fifo);
    if (choice.has_value()) {
        return std::nullopt;
    }
    return choice.error().message;
}

vx::Result<vx::render::Renderer> windowed_renderer() {
    return vx::render::Renderer::create({.surface = headless_surface(),
                                         .framebuffer = surface_size,
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo,
                                         .overlay = nullptr,
                                         .block_textures = {}});
}

// Draws `count` frames of one cube, failing the test at the first error.
void draw_frames(vx::render::Renderer& renderer, int count) {
    const std::array cubes = {glm::i64vec3(0, 0, 0)};
    vx::render::Camera camera;
    camera.position = {0.5, 0.5, 3.0};
    for (int frame = 0; frame < count; ++frame) {
        const vx::Result<vx::render::Frame> drawn =
            renderer.render({.sky = sky, .camera = camera, .cubes = cubes});
        INFO((drawn.has_value() ? std::string() : drawn.error().message));
        REQUIRE(drawn.has_value());
        CHECK(drawn.value() == vx::render::Frame::drawn);
    }
}

} // namespace

TEST_CASE("a window's frames acquire, draw and present, on a surface with no display") {
    if (const std::optional<std::string> reason = surface_refuses_swapchain()) {
        MESSAGE("skipped: ", *reason);
        return;
    }
    vx::Result<vx::render::Renderer> created = windowed_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    REQUIRE(renderer.swapchain() != nullptr);

    // A frame's GPU time arrives when its slot comes round again, though on
    // KosmicKrisp, now and then, after its semaphore says it is done: about
    // one frame in 200 here reads as not in yet. One of six frames will do.
    draw_frames(renderer, 2);
    bool timed = false;
    for (int frame = 0; frame < 6; ++frame) {
        draw_frames(renderer, 1);
        timed = timed || renderer.frame_times().gpu.has_value();
    }
    CHECK(timed);
    const vx::render::SwapchainChoice& choice = renderer.swapchain()->choice();
    MESSAGE(vx::render::format_swapchain_report(choice));
    CHECK(choice.present_mode == VK_PRESENT_MODE_FIFO_KHR);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("a present mode switch takes a new swapchain, on the next frame") {
    if (const std::optional<std::string> reason = surface_refuses_swapchain()) {
        MESSAGE("skipped: ", *reason);
        return;
    }
    vx::Result<vx::render::Renderer> created = windowed_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    draw_frames(renderer, 2);
    const VkSwapchainKHR fifo = renderer.swapchain()->handle();

    renderer.set_present_mode(vx::render::PresentMode::immediate);
    CHECK(renderer.present_mode() == vx::render::PresentMode::immediate);
    CHECK(renderer.swapchain()->handle() == fifo); // until the next frame
    draw_frames(renderer, 2);
    CHECK(renderer.swapchain()->handle() != fifo);
    // Immediate where the surface offers it; otherwise FIFO, which every
    // surface does.
    const vx::render::SurfaceSupport support = vx::render::query_surface_support(
        renderer.context().device().physical_device(), renderer.context().surface());
    const bool offers_immediate =
        std::ranges::find(support.present_modes, VK_PRESENT_MODE_IMMEDIATE_KHR) !=
        support.present_modes.end();
    MESSAGE("the surface ", std::string(offers_immediate ? "offers" : "does not offer"),
            " immediate");
    CHECK(renderer.swapchain()->choice().present_mode ==
          (offers_immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR));

    const VkSwapchainKHR immediate = renderer.swapchain()->handle();
    renderer.set_present_mode(vx::render::PresentMode::fifo);
    draw_frames(renderer, 2);
    CHECK(renderer.swapchain()->handle() != immediate);
    CHECK(renderer.swapchain()->choice().present_mode == VK_PRESENT_MODE_FIFO_KHR);

    // Asking for the mode it has already replaces nothing.
    const VkSwapchainKHR settled = renderer.swapchain()->handle();
    renderer.set_present_mode(vx::render::PresentMode::fifo);
    draw_frames(renderer, 1);
    CHECK(renderer.swapchain()->handle() == settled);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}
