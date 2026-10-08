#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/world/block_file.hpp>

#include <doctest/doctest.h>

#include <initializer_list>
#include <optional>
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
