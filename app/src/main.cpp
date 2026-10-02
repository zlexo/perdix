#include "fly_controls.hpp"
#include "overlay.hpp"
#include "window.hpp"

#include <vx/core/version.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>
#include <vx/render/instance.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

// RFC 0001: validation in debug builds.
#ifdef NDEBUG
constexpr bool validation = false;
#else
constexpr bool validation = true;
#endif

struct Options {
    // Quit after this many frames, for smoke tests; 0 runs until the window closes.
    std::uint64_t frames = 0;
    // Show the overlay from the first frame, not only once F3 is pressed.
    bool overlay = false;
};

vx::Result<Options> parse_options(std::span<char* const> args) {
    constexpr std::string_view frames_flag = "--frames=";
    constexpr std::string_view overlay_flag = "--overlay";
    Options options;
    for (const std::string_view arg : args) {
        if (arg == overlay_flag) {
            options.overlay = true;
            continue;
        }
        if (!arg.starts_with(frames_flag)) {
            return vx::Error{"unknown option \"" + std::string(arg) +
                             "\": try --frames=N or --overlay"};
        }
        const std::string_view value = arg.substr(frames_flag.size());
        const auto [parsed_to, error] =
            std::from_chars(value.data(), value.data() + value.size(), options.frames);
        if (error != std::errc{} || parsed_to != value.data() + value.size()) {
            return vx::Error{"--frames takes a whole number, not \"" + std::string(value) + "\""};
        }
    }
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    // Named for Tracy (RFC 0005's Main, World and Worker threads) and for the
    // OS, so debuggers show it too.
    tracy::SetThreadName("Main");
    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    const vx::Result<Options> options = parse_options(args.subspan(1));
    if (!options.has_value()) {
        std::cerr << "error: " << options.error().message << '\n';
        return 2;
    }

    std::cout << "Perdix " << vx::version_string() << '\n';

    // SDL loads a Vulkan library for the window's surface, and it must be the
    // engine's loader: a driver's library does not understand the loader's
    // instances (see Window::create).
    const vx::Result<std::string> loader = vx::render::vulkan_loader_path();
    if (!loader.has_value()) {
        std::cerr << "error: " << loader.error().message << '\n';
        return 1;
    }
    vx::Result<vx::app::Window> window =
        vx::app::Window::create("Perdix", 1280, 720, loader.value());
    if (!window.has_value()) {
        std::cerr << "error: " << window.error().message << '\n';
        return 1;
    }

    // Before the renderer, so it is destroyed after it: the renderer draws
    // with the overlay's ImGui context until then.
    vx::app::Overlay overlay(window.value().handle(), validation, options.value().overlay);
    vx::Result<vx::render::Renderer> renderer =
        vx::render::Renderer::create({.surface = window.value().surface_source(),
                                      .framebuffer = window.value().framebuffer_size(),
                                      .validation = validation,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = overlay.context()});
    if (!renderer.has_value()) {
        std::cerr << "error: " << renderer.error().message << '\n';
        return 1;
    }
    const vx::render::Context& context = renderer.value().context();
    std::cout << vx::render::format_device_report(context.device_info(),
                                                  context.portability_fallback());
    if (const vx::render::Swapchain* swapchain = renderer.value().swapchain()) {
        std::cout << vx::render::format_swapchain_report(swapchain->choice());
    }

    // A daytime sky blue, #78A7FF in sRGB.
    constexpr vx::render::Color sky = {.r = 0.188F, .g = 0.386F, .b = 1.0F, .a = 1.0F};
    // One cube to fly around, seen first from a little above and in front.
    const std::array cubes = {glm::i64vec3(0, 0, 0)};
    vx::render::Camera camera;
    camera.position = {0.5, 1.6, 3.0};
    camera.pitch = -0.4F;
    std::cout << "Click the window to fly: W A S D, Space and Shift for up and down, Ctrl "
                 "to go faster, the mouse to look; Escape releases the mouse. F3 shows the "
                 "overlay.\n";

    using Clock = std::chrono::steady_clock;
    Clock::time_point last = Clock::now();
    bool last_drawn = false;
    const std::uint64_t frames = options.value().frames;
    for (std::uint64_t frame = 0; frames == 0 || frame < frames; ++frame) {
        if (!window.value().handle_events(overlay)) {
            break;
        }
        const Clock::time_point now = Clock::now();
        // The last frame's times, unless it was skipped, and its interval
        // holds the wait for the next.
        if (last_drawn) {
            overlay.record(now - last, renderer.value().frame_times());
        }
        // The time since the last frame, at most a tenth of a second, so a
        // stall, such as dragging the window, does not fling the camera.
        const float seconds = std::min(std::chrono::duration<float>(now - last).count(), 0.1F);
        last = now;
        vx::render::fly(camera, vx::app::read_fly_controls(window.value()), seconds);

        renderer.value().resize(window.value().framebuffer_size());
        ImDrawData* const shown =
            overlay.build(renderer.value(), camera, window.value().mouse_captured());
        const vx::Result<vx::render::Frame> rendered = renderer.value().render(
            {.sky = sky, .camera = camera, .cubes = cubes, .overlay = shown});
        if (!rendered.has_value()) {
            std::cerr << "error: " << rendered.error().message << '\n';
            return 1;
        }
        last_drawn = rendered.value() == vx::render::Frame::drawn;
        if (!last_drawn) {
            // Presenting paces the loop; with nothing to present, a 60 Hz wait does.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        FrameMark;
    }

    // With validation on, a run that broke Vulkan's rules fails, so a smoke run
    // with --frames catches it. Messages from shutting down still print.
    const std::uint32_t errors = context.instance().validation_counts().errors;
    if (errors > 0) {
        std::cerr << "error: Vulkan validation counted " << errors
                  << (errors == 1 ? " error\n" : " errors\n");
        return 3;
    }
    return 0;
}
