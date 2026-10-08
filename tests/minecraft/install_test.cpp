#include <vx/minecraft/install.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vx::minecraft::game_folders;
using vx::minecraft::System;

using Folders = std::vector<fs::path>;

const fs::path player_home = fs::path("home") / "player";
const fs::path player_appdata = fs::path("home") / "player" / "AppData" / "Roaming";

} // namespace

TEST_CASE("each system's launcher keeps the game in a folder of its own") {
    CHECK(game_folders(System::macos, player_home, std::nullopt) ==
          Folders{player_home / "Library" / "Application Support" / "minecraft"});
    CHECK(game_folders(System::windows, player_home, player_appdata) ==
          Folders{player_appdata / ".minecraft"});
    // Linux has the launcher's own folder, then Flathub's.
    CHECK(game_folders(System::linux_os, player_home, std::nullopt) ==
          Folders{player_home / ".minecraft",
                  player_home / ".var" / "app" / "com.mojang.Minecraft" / ".minecraft"});
    // APPDATA is only Windows'.
    CHECK(game_folders(System::macos, player_home, player_appdata) ==
          game_folders(System::macos, player_home, {}));
    CHECK(game_folders(System::linux_os, player_home, player_appdata) ==
          game_folders(System::linux_os, player_home, {}));
}

TEST_CASE("Windows without APPDATA looks in the home folder") {
    CHECK(game_folders(System::windows, player_home, std::nullopt) ==
          Folders{player_home / ".minecraft"});
    CHECK(game_folders(System::windows, player_home, fs::path()) ==
          Folders{player_home / ".minecraft"});
}

TEST_CASE("nothing is looked for under an empty home") {
    CHECK(game_folders(System::macos, fs::path(), std::nullopt).empty());
    CHECK(game_folders(System::linux_os, fs::path(), player_appdata).empty());
    CHECK(game_folders(System::windows, fs::path(), std::nullopt).empty());
    CHECK(game_folders(System::windows, fs::path(), player_appdata) ==
          Folders{player_appdata / ".minecraft"});
}

TEST_CASE("the system is the one the build is for") {
#ifdef __APPLE__
    CHECK(vx::minecraft::this_system == System::macos);
#elif defined(_WIN32)
    CHECK(vx::minecraft::this_system == System::windows);
#else
    CHECK(vx::minecraft::this_system == System::linux_os);
#endif
}
