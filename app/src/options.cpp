#include "options.hpp"

#include "startup_pack.hpp"

#include <vx/core/result.hpp>

#include <charconv>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace vx::app {
namespace {

std::filesystem::path from_utf8(std::string_view text) {
    return {std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())};
}

} // namespace

Result<Options> parse_options(std::span<const std::string_view> args) {
    constexpr std::string_view frames_flag = "--frames=";
    constexpr std::string_view overlay_flag = "--overlay";
    constexpr std::string_view pack_flag = "--pack=";
    constexpr std::string_view minecraft_flag = "--minecraft=";
    constexpr std::string_view minecraft_version_flag = "--minecraft-version=";
    constexpr std::string_view no_minecraft_flag = "--no-minecraft";
    constexpr std::string_view flythrough_flag = "--flythrough";
    constexpr std::string_view quick_flythrough_flag = "--flythrough=quick";
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
        } else if (arg == flythrough_flag) {
            options.flythrough = Flythrough::full;
        } else if (arg == quick_flythrough_flag) {
            options.flythrough = Flythrough::quick;
        } else if (arg.starts_with(frames_flag)) {
            const std::string_view value = arg.substr(frames_flag.size());
            const auto [parsed_to, error] =
                std::from_chars(value.data(), value.data() + value.size(), options.frames);
            if (error != std::errc{} || parsed_to != value.data() + value.size()) {
                return Error{"--frames takes a whole number, not \"" + std::string(value) + "\""};
            }
        } else {
            return Error{"unknown option \"" + std::string(arg) +
                         "\": try --frames=N, --overlay, --pack=DIR, --minecraft=PATH, "
                         "--minecraft-version=ID, --no-minecraft or --flythrough[=quick]"};
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
