#pragma once

#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/render/block_pack.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace vx::app {

// What the options say about drawing with the player's Minecraft (RFC 0009):
// --no-minecraft turns it off; --minecraft and --minecraft-version fill the
// query, and the app the environment.
struct MinecraftChoice {
    bool use = true;
    minecraft::InstallQuery query{};
};

// The block pack as the app loads it at startup.
struct StartupPack {
    // Its diagnostics are every step's: the texture maps', finding the game's,
    // making its textures', loading the blocks and their textures', and
    // checking the maps against them.
    render::BlockPack pack;
    // The startup report's line on where the textures came from:
    // "Textures: Minecraft 26.2, from <jar>, 11 of 11".
    std::string textures;
    // A Minecraft named on the command line (a folder, a jar or a version)
    // that cannot be used, and why. The pack is loaded with its own textures
    // all the same, so that its problems are listed too; the app prints this
    // after them and stops.
    std::optional<Error> named_game;
};

// The pack under `root` (RFC 0004), drawn with the player's Minecraft where
// it can be (RFC 0009): each namespace's texture map read from the pack, the
// game found unless the choice turns it off or the pack has no map and none
// is named, the textures its jar can make made, the pack loaded with them in
// place of their files, and the maps checked against the textures its blocks
// use. A game that is not found, unless named, is said in the report, and
// the pack's own textures are drawn.
[[nodiscard]] StartupPack load_startup_pack(const std::filesystem::path& root,
                                            const MinecraftChoice& choice);

} // namespace vx::app
