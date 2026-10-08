#pragma once

#include <vx/minecraft/install.hpp>

#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace vx::test {

// Whether the tests are asked to check against the player's own Minecraft:
// VX_TEST_MINECRAFT set, to anything but 0. Never in CI, which has no game.
inline bool player_minecraft_asked() {
    const char* const asked = std::getenv("VX_TEST_MINECRAFT");
    return asked != nullptr && !std::string_view(asked).empty() && std::string_view(asked) != "0";
}

// What "skipped: " says when they are not asked.
inline constexpr std::string_view not_asked =
    "skipped: VX_TEST_MINECRAFT is not set, so the player's Minecraft is not looked for";

// The search the app runs for the player's Minecraft, from the environment
// as the app reads it: the home folder and, on Windows, APPDATA.
inline vx::minecraft::InstallQuery player_query() {
    vx::minecraft::InstallQuery query;
#ifdef _WIN32
    const char* const home = std::getenv("USERPROFILE");
    if (const char* const appdata = std::getenv("APPDATA")) {
        query.appdata = std::filesystem::path(appdata);
    }
#else
    const char* const home = std::getenv("HOME");
#endif
    if (home != nullptr) {
        query.home = home;
    }
    return query;
}

} // namespace vx::test
