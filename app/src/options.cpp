#include "options.hpp"

#include "startup_pack.hpp"

#include <vx/core/result.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace vx::app {
namespace {

std::filesystem::path from_utf8(std::string_view text) {
    return {std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())};
}

// A whole number of `T`, all of `value`.
template <class T> std::optional<T> whole_number(std::string_view value) {
    T number = 0;
    const auto [parsed_to, error] =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || parsed_to != value.data() + value.size()) {
        return std::nullopt;
    }
    return number;
}

std::optional<WorkerQos> worker_qos(std::string_view value) {
    if (value == "switching") {
        return WorkerQos::switching;
    }
    if (value == "utility") {
        return WorkerQos::utility;
    }
    if (value == "user-initiated") {
        return WorkerQos::user_initiated;
    }
    return std::nullopt;
}

// --workers=N: from 1 to 64.
Result<std::uint32_t> worker_count(std::string_view value) {
    const std::optional<std::uint32_t> workers = whole_number<std::uint32_t>(value);
    if (!workers || *workers < 1 || *workers > 64) {
        return Error{"--workers takes a whole number from 1 to 64, not \"" + std::string(value) +
                     "\""};
    }
    return *workers;
}

// --view-distance=N: in chunks, within the overlay slider's range.
Result<std::int32_t> view_distance(std::string_view value) {
    const std::optional<std::int32_t> chunks = whole_number<std::int32_t>(value);
    if (!chunks || *chunks < least_view_distance || *chunks > most_view_distance) {
        return Error{"--view-distance takes a whole number of chunks from " +
                     std::to_string(least_view_distance) + " to " +
                     std::to_string(most_view_distance) + ", not \"" + std::string(value) + "\""};
    }
    return *chunks;
}

// The options that set how a run is measured: the flythroughs, the view
// distance, the worker count and their quality of service, and the frame
// limit. True when `arg` was one of them, an error when its value was not,
// false when it is another option.
Result<bool> parse_run_option(std::string_view arg, Options& options) {
    constexpr std::string_view frames_flag = "--frames=";
    constexpr std::string_view flythrough_flag = "--flythrough";
    constexpr std::string_view quick_flythrough_flag = "--flythrough=quick";
    constexpr std::string_view endurance_flythrough_flag = "--flythrough=long";
    constexpr std::string_view view_distance_flag = "--view-distance=";
    constexpr std::string_view workers_flag = "--workers=";
    constexpr std::string_view worker_qos_flag = "--worker-qos=";
    if (arg == flythrough_flag) {
        options.flythrough = Flythrough::full;
    } else if (arg == quick_flythrough_flag) {
        options.flythrough = Flythrough::quick;
    } else if (arg == endurance_flythrough_flag) {
        options.flythrough = Flythrough::endurance;
    } else if (arg.starts_with(view_distance_flag)) {
        const Result<std::int32_t> chunks = view_distance(arg.substr(view_distance_flag.size()));
        if (!chunks.has_value()) {
            return chunks.error();
        }
        options.view_distance = chunks.value();
    } else if (arg.starts_with(workers_flag)) {
        const Result<std::uint32_t> workers = worker_count(arg.substr(workers_flag.size()));
        if (!workers.has_value()) {
            return workers.error();
        }
        options.workers = workers.value();
    } else if (arg.starts_with(worker_qos_flag)) {
        const std::string_view value = arg.substr(worker_qos_flag.size());
        const std::optional<WorkerQos> qos = worker_qos(value);
        if (!qos) {
            return Error{"--worker-qos takes switching, utility or user-initiated, not \"" +
                         std::string(value) + "\""};
        }
        options.worker_qos = *qos;
    } else if (arg.starts_with(frames_flag)) {
        const std::string_view value = arg.substr(frames_flag.size());
        const std::optional<std::uint64_t> frames = whole_number<std::uint64_t>(value);
        if (!frames) {
            return Error{"--frames takes a whole number, not \"" + std::string(value) + "\""};
        }
        options.frames = *frames;
    } else {
        return false;
    }
    return true;
}

// The options for the players (RFC 0012): --skin=, --perdix-skin= and
// --perdix=.
// True when `arg` was one of them, an error when its value is empty, false
// when it is another option.
Result<bool> parse_figure_option(std::string_view arg, Options& options) {
    constexpr std::string_view skin_flag = "--skin=";
    constexpr std::string_view perdix_skin_flag = "--perdix-skin=";
    constexpr std::string_view perdix_flag = "--perdix=";
    if (arg.starts_with(perdix_flag)) {
        const std::string_view mode = arg.substr(perdix_flag.size());
        if (mode == "life") {
            options.perdix = LifeMode::life;
        } else if (mode == "follow") {
            options.perdix = LifeMode::follow;
        } else if (mode == "still") {
            options.perdix = LifeMode::still;
        } else {
            return Error{"--perdix takes life, follow or still, not \"" + std::string(mode) + "\""};
        }
        return true;
    }
    for (const auto& [flag, value] :
         {std::pair{skin_flag, &options.skin}, std::pair{perdix_skin_flag, &options.perdix_skin}}) {
        if (arg.starts_with(flag)) {
            if (arg.size() == flag.size()) {
                return Error{std::string(flag) +
                             " needs a skin: perdix, zlex, one of the game's, or a file"};
            }
            *value = std::string(arg.substr(flag.size()));
            return true;
        }
    }
    return false;
}

} // namespace

Result<Options> parse_options(std::span<const std::string_view> args) {
    constexpr std::string_view overlay_flag = "--overlay";
    constexpr std::string_view pack_flag = "--pack=";
    constexpr std::string_view minecraft_flag = "--minecraft=";
    constexpr std::string_view minecraft_version_flag = "--minecraft-version=";
    constexpr std::string_view no_minecraft_flag = "--no-minecraft";
    constexpr std::string_view horizon_flag = "--horizon";
    Options options;
    for (const std::string_view arg : args) {
        if (arg == overlay_flag) {
            options.overlay = true;
        } else if (arg.starts_with(pack_flag)) {
            options.pack = std::string(arg.substr(pack_flag.size()));
        } else if (arg.starts_with(minecraft_flag)) {
            if (arg.size() == minecraft_flag.size()) {
                return Error{"--minecraft= needs a game folder or a client jar"};
            }
            options.minecraft = std::string(arg.substr(minecraft_flag.size()));
        } else if (arg.starts_with(minecraft_version_flag)) {
            if (arg.size() == minecraft_version_flag.size()) {
                return Error{"--minecraft-version= needs a version folder's name"};
            }
            options.minecraft_version = std::string(arg.substr(minecraft_version_flag.size()));
        } else if (arg == no_minecraft_flag) {
            options.no_minecraft = true;
        } else if (arg == horizon_flag) {
            options.horizon = true;
        } else if (const Result<bool> run = parse_run_option(arg, options); !run.has_value()) {
            return run.error();
        } else if (run.value()) {
            continue;
        } else if (const Result<bool> figure = parse_figure_option(arg, options);
                   !figure.has_value()) {
            return figure.error();
        } else if (!figure.value()) {
            return Error{"unknown option \"" + std::string(arg) +
                         "\": try --frames=N, --overlay, --pack=DIR, --minecraft=PATH, "
                         "--minecraft-version=ID, --no-minecraft, --flythrough[=quick|long], "
                         "--view-distance=N, --horizon, --workers=N, "
                         "--worker-qos=switching|utility|user-initiated, "
                         "--skin=NAME[:slim], --perdix-skin=NAME[:slim] or "
                         "--perdix=life|follow|still"};
        }
    }
    if (options.no_minecraft && (options.minecraft || options.minecraft_version)) {
        return Error{"--no-minecraft goes without --minecraft and --minecraft-version"};
    }
    return options;
}

MinecraftChoice minecraft_choice(const Options& options, const Environment& environment) {
    MinecraftChoice choice{.use = !options.no_minecraft, .query = {}};
    if (options.minecraft) {
        const std::filesystem::path given = from_utf8(*options.minecraft);
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(given, error);
        choice.query.path = error ? given : absolute;
    }
    choice.query.version = options.minecraft_version;
    if (environment.home) {
        choice.query.home = from_utf8(*environment.home);
    }
    if (environment.appdata) {
        choice.query.appdata = from_utf8(*environment.appdata);
    }
    return choice;
}

} // namespace vx::app
