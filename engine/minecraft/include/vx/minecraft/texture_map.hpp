#pragma once

#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vx::minecraft {

// A color as a PNG stores it: red, green and blue, sRGB, 0 to 255.
using Color = std::array<std::uint8_t, 3>;

// What a layer is multiplied by (RFC 0009): nothing; the color map of that
// name in the jar, assets/minecraft/textures/colormap/<name>.png, read where
// plains are; or a color of its own, written #rrggbb.
using Tint = std::variant<std::monostate, std::string, Color>;

// One of the game's block textures, assets/minecraft/textures/block/<texture>.png
// in its jar, perhaps tinted.
struct Layer {
    std::string texture; // "grass_block_side_overlay"
    Tint tint;
    SourcePlace place;      // where the texture's name is written, for messages
    SourcePlace tint_place; // where the tint is written, when there is one
};

// What one of a namespace's textures is in Minecraft: one layer or more,
// drawn bottom first.
struct MappedTexture {
    std::vector<Layer> layers;
    SourcePlace place; // of its key
};

// A namespace's assets/<namespace>/minecraft.toml (RFC 0009): what each of
// its textures is made of in Minecraft. Each key is a texture of the
// namespace; its value one of the game's block textures, a table of one and
// its tint, or a list of those, drawn bottom first:
//
//   stone = "stone"
//   grass_top = { texture = "grass_block_top", tint = "grass" }
//   grass_side = ["grass_block_side", { texture = "grass_block_side_overlay", tint = "grass" }]
struct TextureMap {
    std::string path;           // "assets/core/minecraft.toml"
    std::string namespace_name; // "core", from the path
    // "grass_side" and its layers, in name order.
    std::map<std::string, MappedTexture, std::less<>> textures;
};

// A texture map, or why it could not be read.
struct TextureMapLoad {
    std::optional<TextureMap> map;       // empty when any diagnostic is an error
    std::vector<Diagnostic> diagnostics; // in the order of the file
};

// Reads one texture map and checks it against RFC 0009's rules, as
// parse_block_file checks a block file: every problem is a diagnostic,
// placed where it is written. Errors: a path other than
// assets/<namespace>/minecraft.toml, TOML syntax, a value of the wrong type,
// an empty list of layers, a layer without its texture, a key in a layer
// other than texture and tint (with a guess at one misspelt), a tint that is
// neither a name nor #rrggbb, and a name with characters other than a to z,
// 0 to 9 and _.
[[nodiscard]] TextureMapLoad load_texture_map(const SourceFile& file);

} // namespace vx::minecraft
