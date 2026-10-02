#include "window.hpp"

#include <vx/core/version.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>
#include <vx/render/instance.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain_choice.hpp>

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
};

vx::Result<Options> parse_options(std::span<char* const> args) {
    constexpr std::string_view frames_flag = "--frames=";
    Options options;
    for (const std::string_view arg : args) {
        if (!arg.starts_with(frames_flag)) {
            return vx::Error{"unknown option \"" + std::string(arg) + "\": try --frames=N"};
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

    vx::Result<vx::render::Renderer> renderer =
        vx::render::Renderer::create({.surface = window.value().surface_source(),
                                      .framebuffer = window.value().framebuffer_size(),
                                      .validation = validation,
                                      .present_mode = vx::render::PresentMode::fifo});
    if (!renderer.has_value()) {
        std::cerr << "error: " << renderer.error().message << '\n';
        return 1;
    }
    const vx::render::Context& context = renderer.value().context();
    std::cout << vx::render::format_device_report(context.device_info(),
                                                  context.portability_fallback())
              << vx::render::format_swapchain_report(renderer.value().swapchain().choice());

    // A daytime sky blue, #78A7FF in sRGB.
    constexpr vx::render::Color sky = {.r = 0.188F, .g = 0.386F, .b = 1.0F, .a = 1.0F};
    const std::uint64_t frames = options.value().frames;
    for (std::uint64_t frame = 0; frames == 0 || frame < frames; ++frame) {
        if (!window.value().handle_events()) {
            break;
        }
        renderer.value().resize(window.value().framebuffer_size());
        const vx::Result<vx::render::Frame> rendered = renderer.value().render(sky);
        if (!rendered.has_value()) {
            std::cerr << "error: " << rendered.error().message << '\n';
            return 1;
        }
        if (rendered.value() == vx::render::Frame::skipped) {
            // Presenting paces the loop; with nothing to present, a 60 Hz wait does.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
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
