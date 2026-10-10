#pragma once

#include "startup_pack.hpp"

#include <vx/core/job_system.hpp>
#include <vx/core/result.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vx::app {

// RFC 0006's benchmark flythrough: none, the full one, the few seconds CI
// runs, or RFC 0005's ten minutes on the same path.
enum class Flythrough : std::uint8_t { none, full, quick, endurance };

// The app's command line.
struct Options {
    // Quit after this many frames, for smoke tests; 0 runs until the window closes.
    std::uint64_t frames = 0;
    // Show the overlay from the first frame, not only once O is pressed.
    bool overlay = false;
    // The folder holding the block pack's assets/; unset, it is found from
    // where the executable is.
    std::optional<std::string> pack;
    // RFC 0009: a game folder or a client jar to draw Minecraft's textures
    // from, a version folder's name, or neither, whatever is installed.
    std::optional<std::string> minecraft;
    std::optional<std::string> minecraft_version;
    bool no_minecraft = false;
    // Fly RFC 0006's benchmark path, print its report, and quit.
    Flythrough flythrough = Flythrough::none;
    // RFC 0005's workers: how many, unset for the cores less two, and how
    // they pick their QoS class (the sweep compares them).
    std::optional<std::uint32_t> workers;
    WorkerQos worker_qos = WorkerQos::switching;
    // RFC 0012: the skins you and Perdix wear, by name (perdix, zlex, one of
    // the game's defaults) or a skin file's path, each with :slim or :wide.
    std::optional<std::string> skin;
    std::optional<std::string> perdix_skin;
};

// The options, from the arguments after the program's name, or why they are
// not options: an unknown one, a value that is not a number, out of range or
// missing, or --no-minecraft with either of the other two.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> args);

// What the app reads from the environment to find Minecraft: the home
// folder and, on Windows, APPDATA.
struct Environment {
    std::optional<std::string> home;
    std::optional<std::string> appdata;
};

// What the options and the environment say about the player's Minecraft
// (RFC 0009). A path named on the command line is made absolute, so that
// messages about the files under it name them wherever they are printed.
[[nodiscard]] MinecraftChoice minecraft_choice(const Options& options,
                                               const Environment& environment);

} // namespace vx::app
