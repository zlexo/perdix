#include "startup_pack.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/minecraft/made_textures.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/render/block_pack.hpp>
#include <vx/world/block_registry.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::app {
namespace {

// A path in UTF-8, for the report. On Windows, path::string() converts
// through the system's code page and throws on a character it cannot map.
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// "11 of 11": how many of the textures the pack's blocks use were made.
std::string made_of_used(const BlockRegistry& registry, const std::vector<MadeTexture>& made) {
    std::set<std::string_view> used;
    for (const TextureRef& texture : registry.textures()) {
        if (!texture.name.empty()) { // layer 0, the checker
            used.insert(texture.name);
        }
    }
    const auto from_game = std::ranges::count_if(used, [&](std::string_view name) {
        return std::ranges::find(made, name, &MadeTexture::name) != made.end();
    });
    return std::to_string(from_game) + " of " + std::to_string(used.size());
}

// What the player's Minecraft gives the pack: the textures its jar can make
// from each map, and the version and jar the report names. Nothing of the jar
// stays open after.
struct FromGame {
    std::vector<MadeTexture> made;
    std::string version; // empty when no game is used
    std::filesystem::path jar;
};

FromGame textures_from_game(const minecraft::Install& install,
                            std::span<const minecraft::TextureMap> maps,
                            std::vector<Diagnostic>& diagnostics) {
    FromGame game{.made = {}, .version = install.version, .jar = install.path};
    for (const minecraft::TextureMap& map : maps) {
        std::vector<MadeTexture> made =
            minecraft::make_textures(install.jar, install.version, map, diagnostics);
        game.made.insert(game.made.end(), std::make_move_iterator(made.begin()),
                         std::make_move_iterator(made.end()));
    }
    return game;
}

} // namespace

StartupPack load_startup_pack(const std::filesystem::path& root, const MinecraftChoice& choice) {
    minecraft::TextureMapsLoad maps = minecraft::load_texture_maps(root);
    StartupPack startup;
    std::vector<Diagnostic> diagnostics = std::move(maps.diagnostics);
    const bool named = choice.query.path.has_value() || choice.query.version.has_value();
    FromGame game;
    if (!choice.use) {
        startup.textures = "Textures: the pack's own (--no-minecraft)";
    } else if (maps.maps.empty() && !named) {
        startup.textures = "Textures: the pack's own; it has no minecraft.toml";
    } else if (const Result<minecraft::Install> install =
                   minecraft::find_install(choice.query, diagnostics);
               !install) {
        // Named on the command line, it must be there; looked for, it may not.
        if (named) {
            startup.named_game = install.error();
        } else {
            startup.textures = "Textures: the pack's own; " + install.error().message +
                               " (--minecraft=PATH names one)";
        }
    } else {
        game = textures_from_game(install.value(), maps.maps, diagnostics);
    }
    startup.pack = render::load_block_pack(root, game.made);
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(startup.pack.diagnostics.begin()),
                       std::make_move_iterator(startup.pack.diagnostics.end()));
    if (startup.pack.registry) {
        for (const minecraft::TextureMap& map : maps.maps) {
            minecraft::check_texture_map(map, startup.pack.registry->textures(), diagnostics);
        }
        if (!game.version.empty()) {
            startup.textures = "Textures: Minecraft " + game.version + ", from " + utf8(game.jar) +
                               ", " + made_of_used(*startup.pack.registry, game.made);
        }
    }
    startup.pack.diagnostics = std::move(diagnostics);
    return startup;
}

} // namespace vx::app
