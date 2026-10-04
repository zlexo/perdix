#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_visual.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

// Every diagnostic as format() prints it.
std::vector<std::string> formatted(const std::vector<vx::Diagnostic>& diagnostics) {
    std::vector<std::string> lines;
    lines.reserve(diagnostics.size());
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        lines.push_back(vx::format(diagnostic));
    }
    return lines;
}

struct Parsed {
    std::optional<vx::BlockDefinition> block;
    std::vector<std::string> messages;
};

Parsed parse(const std::string& path, const std::string& text) {
    std::vector<vx::Diagnostic> diagnostics;
    std::optional<vx::BlockDefinition> block =
        vx::parse_block_file({.path = path, .text = text}, diagnostics);
    return {.block = std::move(block), .messages = formatted(diagnostics)};
}

} // namespace

TEST_CASE("a block file parses into its definition, named by its path") {
    // RFC 0004's oak log.
    const Parsed parsed = parse("assets/core/blocks/oak_log.toml", R"(
[properties]
axis = ["y", "x", "z"]

[textures]
side = "oak_log"
top = "oak_log_top"
bottom = "oak_log_top"

[[variant]]
when = { axis = "x" }
rotate_x = 90
rotate_y = 90

[[variant]]
when = { axis = "z" }
rotate_x = 90
)");
    REQUIRE(parsed.messages.empty());
    REQUIRE(parsed.block.has_value());
    const vx::BlockDefinition& block = *parsed.block;
    CHECK(block.name == "core:oak_log");
    CHECK(block.opacity == vx::Opacity::opaque);
    CHECK(block.cull_group.empty());
    REQUIRE(block.properties.size() == 1);
    CHECK(block.properties[0].name == "axis");
    CHECK(block.properties[0].values == std::vector<std::string>{"y", "x", "z"});
    REQUIRE(block.textures.size() == 3);
    CHECK(block.textures[0].face == vx::TextureFace::side);
    CHECK(block.textures[0].texture == "oak_log");
    CHECK(block.textures[1].face == vx::TextureFace::top);
    CHECK(block.textures[2].face == vx::TextureFace::bottom);
    REQUIRE(block.variants.size() == 2);
    REQUIRE(block.variants[0].when.size() == 1);
    CHECK(block.variants[0].when[0].property == "axis");
    CHECK(block.variants[0].when[0].values == std::vector<std::string>{"x"});
    CHECK(block.variants[0].rotate_x == 90);
    CHECK(block.variants[0].rotate_y == 90);
    CHECK(block.variants[1].rotate_x == 90);
    CHECK(block.variants[1].rotate_y == 0);
}

TEST_CASE("a stone is two lines, and a cutout block says so") {
    const Parsed stone = parse("assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
    REQUIRE(stone.block.has_value());
    CHECK(stone.block->name == "core:stone");
    CHECK(stone.block->properties.empty());
    CHECK(stone.block->variants.empty());

    const Parsed leaves =
        parse("assets/core/blocks/oak_leaves.toml",
              "opacity = \"cutout\"\ncull_group = \"leaves\"\n[textures]\nall = \"oak_leaves\"\n");
    REQUIRE(leaves.block.has_value());
    CHECK(leaves.block->opacity == vx::Opacity::cutout);
    CHECK(leaves.block->cull_group == "leaves");
}

TEST_CASE("integer and boolean values become text, and properties come in name order") {
    const Parsed parsed = parse("assets/core/blocks/wheat.toml", R"(
[properties]
lit = [false, true]
age = [0, 1, 2, 3]
[textures]
all = "wheat"
[[variant]]
when = { age = [2, 3] }
textures = { all = "wheat_ripe" }
)");
    REQUIRE(parsed.block.has_value());
    REQUIRE(parsed.block->properties.size() == 2);
    CHECK(parsed.block->properties[0].name == "age");
    CHECK(parsed.block->properties[0].values == std::vector<std::string>{"0", "1", "2", "3"});
    CHECK(parsed.block->properties[1].name == "lit");
    CHECK(parsed.block->properties[1].values == std::vector<std::string>{"false", "true"});
    CHECK(parsed.block->variants[0].when[0].values == std::vector<std::string>{"2", "3"});
}

TEST_CASE("a TOML syntax error is placed in its file") {
    const Parsed parsed = parse("assets/core/blocks/stone.toml", "[textures]\nall = \n");
    CHECK_FALSE(parsed.block.has_value());
    REQUIRE(parsed.messages.size() == 1);
    CHECK(parsed.messages[0].starts_with("assets/core/blocks/stone.toml:2:"));
    CHECK(parsed.messages[0].find(": error: ") != std::string::npos);
}

TEST_CASE("an unknown key is an error, with a guess for a near miss") {
    const Parsed parsed = parse("assets/core/blocks/glass.toml",
                                "opacty = \"cutout\"\nflavour = 3\n[textures]\nal = \"glass\"\n");
    CHECK_FALSE(parsed.block.has_value());
    CHECK(parsed.messages ==
          std::vector<std::string>{
              "assets/core/blocks/glass.toml:1:1: error: unknown key 'opacty'; did you mean "
              "'opacity'?",
              "assets/core/blocks/glass.toml:2:1: error: unknown key 'flavour'",
              "assets/core/blocks/glass.toml:4:1: error: unknown key 'al'; did you mean 'all'?"});
}

TEST_CASE("a value of the wrong type is an error, every one of them reported") {
    const Parsed parsed = parse("assets/core/blocks/furnace.toml", R"(opacity = 3
properties = "facing"
[textures]
all = 7
[[variant]]
when = { lit = 1.5 }
rotate_x = "90"
)");
    CHECK_FALSE(parsed.block.has_value());
    CHECK(
        parsed.messages ==
        std::vector<std::string>{
            R"(assets/core/blocks/furnace.toml:1:11: error: opacity is "opaque" or "cutout")",
            "assets/core/blocks/furnace.toml:2:14: error: properties is a table of property names "
            "and their values",
            R"(assets/core/blocks/furnace.toml:4:7: error: a texture is named in quotes, such as "oak_log")",
            "assets/core/blocks/furnace.toml:6:16: error: a condition names one value or a list of "
            "them",
            "assets/core/blocks/furnace.toml:7:12: error: rotate_x is a whole number of degrees"});
}

TEST_CASE("a block file belongs at assets/<namespace>/blocks/<name>.toml and names its textures") {
    for (const char* const path :
         {"assets/core/stone.toml", "assets/core/blocks/stone.json", "blocks/.toml"}) {
        INFO(path);
        const Parsed parsed = parse(path, "[textures]\nall = \"stone\"\n");
        CHECK_FALSE(parsed.block.has_value());
        CHECK(parsed.messages == std::vector<std::string>{std::string(path) +
                                                          ": error: a block file belongs at "
                                                          "assets/<namespace>/blocks/<name>.toml"});
    }
    const Parsed bare = parse("assets/core/blocks/stone.toml", "opacity = \"opaque\"\n");
    CHECK(bare.messages == std::vector<std::string>{"assets/core/blocks/stone.toml: error: a block "
                                                    "names its textures in a [textures] table"});
}
