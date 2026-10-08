#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_registry.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using vx::minecraft::Color;
using vx::minecraft::Layer;
using vx::minecraft::MappedTexture;
using vx::minecraft::TextureMap;

// RFC 0009's map for the core pack, as the RFC writes it.
constexpr const char* rfc_map =
    R"(# assets/core/minecraft.toml: what each core texture is when the player's
# Minecraft is found (RFC 0009). Names are the game's block textures,
# assets/minecraft/textures/block/<name>.png in its jar.
stone = "stone"
cobblestone = "cobblestone"
dirt = "dirt"
sand = "sand"
grass_top = { texture = "grass_block_top", tint = "grass" }
# The side, with its tinted overlay drawn over it.
grass_side = ["grass_block_side", { texture = "grass_block_side_overlay", tint = "grass" }]
oak_log = "oak_log"
oak_log_top = "oak_log_top"
oak_planks = "oak_planks"
oak_leaves = { texture = "oak_leaves", tint = "foliage" }
glass = "glass"
)";

// A map read from the core namespace's place, and what reading it said,
// formatted.
struct Read {
    std::optional<TextureMap> map;
    std::vector<std::string> messages;
};

Read read(const std::string& text, const std::string& path = "assets/core/minecraft.toml") {
    vx::minecraft::TextureMapLoad load =
        vx::minecraft::load_texture_map({.path = path, .text = text});
    Read got{.map = std::move(load.map), .messages = {}};
    for (const vx::Diagnostic& diagnostic : load.diagnostics) {
        got.messages.push_back(vx::format(diagnostic));
    }
    return got;
}

// The layers of one texture in a map that read without a problem. The
// name is a view: GCC warns that a reference returned by a function given a
// temporary std::string may dangle.
const std::vector<Layer>& layers_of(const Read& got, std::string_view name) {
    REQUIRE(got.map.has_value());
    const auto found = got.map->textures.find(name);
    REQUIRE(found != got.map->textures.end());
    return found->second.layers;
}

bool untinted(const Layer& layer) {
    return std::holds_alternative<std::monostate>(layer.tint);
}

// The color map a layer is tinted with, or nothing.
std::optional<std::string> color_map(const Layer& layer) {
    if (const std::string* const name = std::get_if<std::string>(&layer.tint)) {
        return *name;
    }
    return std::nullopt;
}

// A layer of the block texture array, as the block registry lists them.
vx::TextureRef array_layer(const std::string& name, const std::string& first_use,
                           std::uint8_t quarter_turns = 0) {
    return {.name = name,
            .quarter_turns = quarter_turns,
            .used_opaque = true,
            .used_cutout = false,
            .first_use = first_use};
}

// What checking a map against the layers says, formatted.
std::vector<std::string> checked(const TextureMap& map, std::span<const vx::TextureRef> layers) {
    std::vector<vx::Diagnostic> diagnostics;
    vx::minecraft::check_texture_map(map, layers, diagnostics);
    std::vector<std::string> messages;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        CHECK(diagnostic.severity == vx::Severity::warning);
        messages.push_back(vx::format(diagnostic));
    }
    return messages;
}

} // namespace

TEST_CASE("RFC 0009's map for the core pack reads into each texture's layers") {
    const Read got = read(rfc_map);
    REQUIRE(got.messages.empty());
    REQUIRE(got.map.has_value());
    CHECK(got.map->path == "assets/core/minecraft.toml");
    CHECK(got.map->namespace_name == "core");
    CHECK(got.map->textures.size() == 11);
    for (const char* const plain : {"stone", "cobblestone", "dirt", "sand", "oak_log",
                                    "oak_log_top", "oak_planks", "glass"}) {
        INFO(plain);
        const std::vector<Layer>& layers = layers_of(got, plain);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].texture == plain);
        CHECK(untinted(layers[0]));
    }

    const std::vector<Layer>& top = layers_of(got, "grass_top");
    REQUIRE(top.size() == 1);
    CHECK(top[0].texture == "grass_block_top");
    CHECK(color_map(top[0]) == "grass");
    CHECK(color_map(layers_of(got, "oak_leaves").at(0)) == "foliage");

    // The side file, then its overlay over it, tinted.
    const std::vector<Layer>& side = layers_of(got, "grass_side");
    REQUIRE(side.size() == 2);
    CHECK(side[0].texture == "grass_block_side");
    CHECK(untinted(side[0]));
    CHECK(side[1].texture == "grass_block_side_overlay");
    CHECK(color_map(side[1]) == "grass");
}

TEST_CASE("every texture, layer and tint keeps where it is written") {
    const Read got = read(rfc_map);
    REQUIRE(got.map.has_value());
    const MappedTexture& stone = got.map->textures.at("stone");
    CHECK(stone.place == vx::SourcePlace{.line = 4, .column = 1});
    CHECK(stone.layers.at(0).place == vx::SourcePlace{.line = 4, .column = 9});
    const MappedTexture& side = got.map->textures.at("grass_side");
    CHECK(side.place == vx::SourcePlace{.line = 10, .column = 1});
    CHECK(side.layers.at(0).place == vx::SourcePlace{.line = 10, .column = 15});
    // Where RFC 0009's example warning about the overlay points.
    CHECK(side.layers.at(1).place == vx::SourcePlace{.line = 10, .column = 47});
    CHECK(side.layers.at(1).tint_place == vx::SourcePlace{.line = 10, .column = 82});
    CHECK(side.layers.at(0).tint_place == vx::SourcePlace{});
}

TEST_CASE("a tint of a layer's own is #rrggbb, in either case") {
    const Read got = read(R"(spruce_leaves = { texture = "spruce_leaves", tint = "#619961" }
birch_leaves = { texture = "birch_leaves", tint = "#80a755" }
white = { texture = "white_wool", tint = "#FFFFFF" }
)");
    REQUIRE(got.messages.empty());
    CHECK(layers_of(got, "spruce_leaves").at(0).tint ==
          vx::minecraft::Tint{Color{0x61, 0x99, 0x61}});
    CHECK(layers_of(got, "birch_leaves").at(0).tint ==
          vx::minecraft::Tint{Color{0x80, 0xA7, 0x55}});
    CHECK(layers_of(got, "white").at(0).tint == vx::minecraft::Tint{Color{255, 255, 255}});
}

TEST_CASE("a tint that is neither a color map's name nor #rrggbb is an error") {
    for (const char* const tint : {"#77AB2", "#7G7G7G", "#1 2345", "#+12345", "#0x1234", "#91BD59 ",
                                   "91BD59", "Grass", ""}) {
        INFO(tint);
        const Read got =
            read(std::string(R"(grass_top = { texture = "grass_block_top", tint = ")") + tint +
                 "\" }\n");
        CHECK_FALSE(got.map.has_value());
        CHECK(
            got.messages ==
            std::vector<std::string>{
                "assets/core/minecraft.toml:1:51: error: tint '" + std::string(tint) +
                R"(' is neither a color map's name, such as "grass", nor a color written #rrggbb)"});
    }
}

TEST_CASE("a layer is a table however TOML writes one, with or without a tint") {
    const Read got = read(R"(dirt = { texture = "dirt" }
stone.texture = "stone"

[grass_top]
texture = "grass_block_top"
tint = "grass"

[[grass_side]]
texture = "grass_block_side"

[[grass_side]]
texture = "grass_block_side_overlay"
tint = "grass"
)");
    REQUIRE(got.messages.empty());
    CHECK(untinted(layers_of(got, "dirt").at(0)));
    CHECK(layers_of(got, "stone").at(0).texture == "stone");
    CHECK(color_map(layers_of(got, "grass_top").at(0)) == "grass");
    const std::vector<Layer>& side = layers_of(got, "grass_side");
    REQUIRE(side.size() == 2);
    CHECK(side[0].texture == "grass_block_side");
    CHECK(side[1].texture == "grass_block_side_overlay");
    CHECK(color_map(side[1]) == "grass");
    // A table's texture is placed at its value, as a name's is.
    CHECK(side[1].place == vx::SourcePlace{.line = 12, .column = 11});
}

TEST_CASE("a map with no keys maps nothing") {
    const Read got = read("# Nothing here is drawn from Minecraft.\n");
    CHECK(got.messages.empty());
    REQUIRE(got.map.has_value());
    CHECK(got.map->textures.empty());
}

TEST_CASE("a texture map belongs at assets/<namespace>/minecraft.toml") {
    for (const char* const path :
         {"assets/minecraft.toml", "assets//minecraft.toml", "assets/core/blocks/minecraft.toml",
          "core/minecraft.toml", "packs/assets/core/minecraft.toml", "assets/core/Minecraft.toml",
          "assets/core/minecraft.json", ""}) {
        INFO(path);
        const Read got = read("stone = \"stone\"\n", path);
        CHECK_FALSE(got.map.has_value());
        CHECK(got.messages ==
              std::vector<std::string>{std::string(path) + ": error: a texture map belongs at "
                                                           "assets/<namespace>/minecraft.toml"});
    }
    const Read other = read("stone = \"stone\"\n", "assets/my_pack_2/minecraft.toml");
    REQUIRE(other.map.has_value());
    CHECK(other.map->namespace_name == "my_pack_2");
}

TEST_CASE("files that toml++ 3.4.0 could not parse safely are errors, not crashes") {
    // The block file tests' cases, each of which once stopped a debug build
    // on one of toml++'s assertions or overflowed the stack.
    std::string deep = "a";
    for (int part = 0; part < 20'000; ++part) {
        deep += ".a";
    }
    for (const std::string& text :
         {std::string("[\n"), std::string("grass_side = [\"grass_block_side\", }\n"),
          std::string("stone = \"stone\"\xC2\xB0\n"), deep + " = \"stone\"\n"}) {
        INFO(text.substr(0, 40));
        const Read got = read(text);
        CHECK_FALSE(got.map.has_value());
        REQUIRE(got.messages.size() == 1);
        CHECK(got.messages[0].starts_with("assets/core/minecraft.toml:"));
        CHECK(got.messages[0].find(": error: ") != std::string::npos);
    }
}

TEST_CASE("every problem in a map is reported, in the order of the file") {
    // toml++ hands keys over in name order: apple before zebra.
    const Read got = read(R"(zebra = 3
apple = []
mango = { texture = "Mango", shade = 1 }
)");
    CHECK_FALSE(got.map.has_value());
    CHECK(
        got.messages ==
        std::vector<std::string>{
            R"(assets/core/minecraft.toml:1:9: error: a texture is a name in quotes, such as "stone", a table such as { texture = "grass_block_top", tint = "grass" }, or a list of them)",
            "assets/core/minecraft.toml:2:9: error: texture 'apple' lists no layers",
            "assets/core/minecraft.toml:3:21: error: Minecraft texture name 'Mango' uses "
            "characters other than a to z, 0 to 9 and _",
            "assets/core/minecraft.toml:3:30: error: unknown key 'shade'"});
}

TEST_CASE("a map with a key for every texture blocks use, and no other, gives no warnings") {
    const Read got = read(rfc_map);
    REQUIRE(got.map.has_value());
    // The core pack's layers: the checker, then each texture and turn.
    const std::vector<vx::TextureRef> layers = {
        array_layer("", ""),
        array_layer("core:cobblestone", "assets/core/blocks/cobblestone.toml:2:7"),
        array_layer("core:dirt", "assets/core/blocks/dirt.toml:2:7"),
        array_layer("core:glass", "assets/core/blocks/glass.toml:3:7"),
        array_layer("core:grass_side", "assets/core/blocks/grass.toml:2:8"),
        array_layer("core:grass_top", "assets/core/blocks/grass.toml:3:7"),
        array_layer("core:oak_leaves", "assets/core/blocks/oak_leaves.toml:4:7"),
        array_layer("core:oak_log", "assets/core/blocks/oak_log.toml:5:8"),
        array_layer("core:oak_log", "assets/core/blocks/oak_log.toml:5:8", 1),
        array_layer("core:oak_log_top", "assets/core/blocks/oak_log.toml:6:7"),
        array_layer("core:oak_log_top", "assets/core/blocks/oak_log.toml:6:7", 1),
        array_layer("core:oak_planks", "assets/core/blocks/oak_planks.toml:2:7"),
        array_layer("core:sand", "assets/core/blocks/sand.toml:2:7"),
        array_layer("core:stone", "assets/core/blocks/stone.toml:2:7"),
    };
    CHECK(checked(*got.map, layers).empty());
}

TEST_CASE("a texture blocks use without a key is drawn from its own file, and says so") {
    const Read got = read("stone = \"stone\"\n");
    REQUIRE(got.map.has_value());
    // Turned twice: one warning, at the least turned layer's first use.
    const std::vector<vx::TextureRef> layers = {
        array_layer("", ""),
        array_layer("core:oak_log", "assets/core/blocks/oak_log.toml:5:8"),
        array_layer("core:oak_log", "assets/core/blocks/oak_beam.toml:3:7", 1),
        array_layer("core:stone", "assets/core/blocks/stone.toml:2:7"),
    };
    CHECK(checked(*got.map, layers) ==
          std::vector<std::string>{
              "assets/core/minecraft.toml: warning: core:oak_log has no key, and is drawn from "
              "its own file (used at assets/core/blocks/oak_log.toml:5:8)"});
}

TEST_CASE("a key no block uses is a warning at the key, in the order of the file") {
    const Read got = read("zinc = \"zinc\"\nstone = \"stone\"\napple = \"apple\"\n");
    REQUIRE(got.map.has_value());
    const std::vector<vx::TextureRef> layers = {
        array_layer("", ""),
        array_layer("core:stone", "assets/core/blocks/stone.toml:2:7"),
    };
    CHECK(checked(*got.map, layers) ==
          std::vector<std::string>{
              "assets/core/minecraft.toml:1:1: warning: no block uses the texture core:zinc",
              "assets/core/minecraft.toml:3:1: warning: no block uses the texture core:apple"});
}

TEST_CASE("a map answers for its own namespace's textures, whichever blocks use them") {
    const Read got = read("stone = \"stone\"\n", "assets/core/minecraft.toml");
    REQUIRE(got.map.has_value());
    // core:stone used by a block of another namespace is used; other:dirt
    // and corelike:sand belong to maps of their own.
    const std::vector<vx::TextureRef> layers = {
        array_layer("", ""),
        array_layer("core:stone", "assets/other/blocks/wall.toml:2:7"),
        array_layer("corelike:sand", "assets/corelike/blocks/sand.toml:2:7"),
        array_layer("other:dirt", "assets/other/blocks/dirt.toml:2:7"),
    };
    CHECK(checked(*got.map, layers).empty());
    // And one whose keys name only another namespace's textures uses none.
    const Read other = read("dirt = \"dirt\"\n", "assets/core/minecraft.toml");
    REQUIRE(other.map.has_value());
    CHECK(checked(*other.map, layers) ==
          std::vector<std::string>{
              "assets/core/minecraft.toml: warning: core:stone has no key, and is drawn from its "
              "own file (used at assets/other/blocks/wall.toml:2:7)",
              "assets/core/minecraft.toml:1:1: warning: no block uses the texture core:dirt"});
}

TEST_CASE("every file in the corpus of bad maps gives exactly the messages it expects") {
    // RFC 0009's test plan: a bad file for each error and each warning, so
    // every check is shown to fire. Each file ends in comments that give the
    // path it stands for, the textures its namespace's blocks use (each a
    // layer, with where it is first used), and every message it must give,
    // in order. A map is checked against those textures once it reads.
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(VX_BAD_MAPS_DIR)) {
        if (entry.path().extension() == ".toml") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    REQUIRE_FALSE(files.empty());
    for (const std::filesystem::path& file : files) {
        INFO(file.filename().string());
        std::ifstream stream(file);
        std::stringstream text;
        text << stream.rdbuf();
        std::string path;
        std::vector<vx::TextureRef> layers = {array_layer("", "")};
        std::vector<std::string> expected;
        std::istringstream lines(text.str());
        for (std::string line; std::getline(lines, line);) {
            if (line.starts_with("# path: ")) {
                path = line.substr(8);
            } else if (line.starts_with("# uses: ")) {
                const std::size_t at = line.find(" at ");
                REQUIRE(at != std::string::npos);
                layers.push_back(array_layer(line.substr(8, at - 8), line.substr(at + 4)));
            } else if (line.starts_with("# expect: ")) {
                expected.push_back(line.substr(10));
            }
        }
        Read got = read(text.str(), path);
        if (got.map) {
            const std::vector<std::string> warnings = checked(*got.map, layers);
            got.messages.insert(got.messages.end(), warnings.begin(), warnings.end());
        }
        // A map comes back unless the file has an error.
        CHECK(got.map.has_value() == std::ranges::none_of(expected, [](const std::string& message) {
                  return message.find(": error: ") != std::string::npos;
              }));
        CHECK_FALSE(expected.empty());
        CHECK(got.messages == expected);
    }
}
