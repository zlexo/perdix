#include "window.hpp"

#include <vx/core/version.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>

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

    vx::Result<vx::render::Context> context =
        vx::render::Context::create({.extensions = {}, .validation = validation});
    if (!context.has_value()) {
        std::cerr << "error: " << context.error().message << '\n';
        return 1;
    }
    std::cout << vx::render::format_device_report(context.value().device_info(),
                                                  context.value().portability_fallback());

    vx::Result<vx::app::Window> window = vx::app::Window::create("Perdix", 1280, 720);
    if (!window.has_value()) {
        std::cerr << "error: " << window.error().message << '\n';
        return 1;
    }
    const std::uint64_t frames = options.value().frames;
    for (std::uint64_t frame = 0; frames == 0 || frame < frames; ++frame) {
        if (!window.value().handle_events()) {
            break;
        }
        // Until presenting paces the frames (RFC 0001 plan item 4), wait out one at 60 Hz.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return 0;
}
