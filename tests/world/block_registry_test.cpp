#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr vx::BlockState state(int id) {
    return static_cast<vx::BlockState>(static_cast<std::uint16_t>(id));
}

// RFC 0004's example pack: a furnace, an oak log and stone.
std::vector<vx::SourceFile> example_pack() {
    return {
        {.path = "assets/core/blocks/stone.toml", .text = "[textures]\nall = \"stone\"\n"},
        {.path = "assets/core/blocks/furnace.toml", .text = R"([properties]
facing = ["north", "east", "south", "west"]
lit = [false, true]
[textures]
all = "furnace"
[[variant]]
when = { lit = true }
textures = { north = "furnace_front_lit" }
)"},
        {.path = "assets/core/blocks/oak_log.toml", .text = R"([properties]
axis = ["y", "x", "z"]
[textures]
side = "oak_log"
top = "oak_log_top"
bottom = "oak_log_top"
[[variant]]
when = { axis = "x" }
rotate_x = 90
rotate_y = 90
)"},
    };
}

// assets/test/blocks/<name>.toml with properties p0, p1... of the given
// numbers of values, so its states multiply to their product.
vx::SourceFile test_block(const std::string& name, std::initializer_list<int> counts) {
    std::string text = "[properties]\n";
    int property = 0;
    for (const int count : counts) {
        text += "p" + std::to_string(property++) + " = [";
        for (int value = 0; value < count; ++value) {
            text += (value == 0 ? "" : ", ") + std::to_string(value);
        }
        text += "]\n";
    }
    text += "[textures]\nall = \"test\"\n";
    return {.path = "assets/test/blocks/" + name + ".toml", .text = text};
}

vx::BlockRegistry loaded(const std::vector<vx::SourceFile>& files) {
    vx::BlockLoad load = vx::BlockRegistry::load(files);
    for (const vx::Diagnostic& diagnostic : load.diagnostics) {
        INFO(vx::format(diagnostic));
    }
    REQUIRE(load.registry.has_value());
    return *std::move(load.registry);
}

std::vector<std::string> keys_of(const vx::BlockRegistry& registry) {
    std::vector<std::string> keys;
    for (std::size_t id = 0; id < registry.state_count(); ++id) {
        keys.push_back(registry.key(state(static_cast<int>(id))));
    }
    return keys;
}

} // namespace

TEST_CASE("air is 0, then blocks in name order, each state in RFC 0004's mixed-radix order") {
    const vx::BlockRegistry registry = loaded(example_pack());
    CHECK(registry.state_count() == 13);
    CHECK(registry.find("core:air") == state(0));
    CHECK(registry.find("core:furnace[facing=north,lit=false]") == state(1));
    // RFC 0004's worked example: 1 + (1 x 2 + 1).
    CHECK(registry.find("core:furnace[facing=east,lit=true]") == state(4));
    CHECK(registry.find("core:oak_log[axis=y]") == state(9));
    CHECK(registry.find("core:oak_log[axis=z]") == state(11));
    CHECK(registry.find("core:stone") == state(12));
}

TEST_CASE("the golden key table: every state's key, in id order") {
    // A change to the numbering shows up here as a diff.
    CHECK(keys_of(loaded(example_pack())) == std::vector<std::string>{
                                                 "core:air",
                                                 "core:furnace[facing=north,lit=false]",
                                                 "core:furnace[facing=north,lit=true]",
                                                 "core:furnace[facing=east,lit=false]",
                                                 "core:furnace[facing=east,lit=true]",
                                                 "core:furnace[facing=south,lit=false]",
                                                 "core:furnace[facing=south,lit=true]",
                                                 "core:furnace[facing=west,lit=false]",
                                                 "core:furnace[facing=west,lit=true]",
                                                 "core:oak_log[axis=y]",
                                                 "core:oak_log[axis=x]",
                                                 "core:oak_log[axis=z]",
                                                 "core:stone",
                                             });
}

TEST_CASE("find and key round-trip every state, and find fills in the defaults") {
    const vx::BlockRegistry registry = loaded(example_pack());
    for (std::size_t id = 0; id < registry.state_count(); ++id) {
        const vx::BlockState each = state(static_cast<int>(id));
        INFO(registry.key(each));
        CHECK(registry.find(registry.key(each)) == each);
    }
    CHECK(registry.find("core:furnace") == state(1));
    CHECK(registry.find("core:furnace[lit=true]") == state(2));
    CHECK(registry.find("core:furnace[lit=true,facing=west]") == state(8));
}

TEST_CASE("with changes one property and nothing else") {
    const vx::BlockRegistry registry = loaded(example_pack());
    const vx::BlockState lit_west = *registry.find("core:furnace[facing=west,lit=true]");
    CHECK(registry.with(lit_west, "facing", "east") ==
          registry.find("core:furnace[facing=east,lit=true]"));
    CHECK(registry.with(lit_west, "lit", "false") ==
          registry.find("core:furnace[facing=west,lit=false]"));
    CHECK_FALSE(registry.with(lit_west, "axis", "x").has_value());
    CHECK_FALSE(registry.with(lit_west, "facing", "up").has_value());
    CHECK_FALSE(registry.with(*registry.find("core:stone"), "lit", "true").has_value());
}

TEST_CASE("each state knows its block, and each block its default state") {
    const vx::BlockRegistry registry = loaded(example_pack());
    CHECK(registry.type_of(state(0)) == vx::BlockType::air);
    const vx::BlockType furnace = registry.type_of(state(4));
    CHECK(registry.type_of(state(1)) == furnace);
    CHECK(registry.type_of(state(8)) == furnace);
    CHECK(registry.type_of(state(9)) != furnace);
    CHECK(registry.default_state(furnace) == state(1));
    CHECK(registry.default_state(registry.type_of(state(11))) == state(9));
    CHECK(registry.default_state(vx::BlockType::air) == state(0));
}

TEST_CASE("the same files in any order give the same ids") {
    const std::vector<std::string> expected = keys_of(loaded(example_pack()));
    std::vector<vx::SourceFile> files = example_pack();
    std::mt19937 generator(5);
    for (int round = 0; round < 6; ++round) {
        std::ranges::shuffle(files, generator);
        CHECK(keys_of(loaded(files)) == expected);
    }
}

TEST_CASE("names number the blocks, not the paths they come from") {
    // In byte order, "a0:x" comes before "a:x" (':' sorts after '0'), while
    // their paths come the other way round ('/' sorts before '0').
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/a/blocks/x.toml", .text = "[textures]\nall = \"x\"\n"},
        {.path = "assets/a0/blocks/x.toml", .text = "[textures]\nall = \"x\"\n"},
    });
    CHECK(registry.find("a0:x") == state(1));
    CHECK(registry.find("a:x") == state(2));
}

TEST_CASE("find refuses keys that name no state") {
    const vx::BlockRegistry registry = loaded(example_pack());
    for (const char* const key :
         {"core:gold", "stone", "core:stone[]", "core:furnace[", "core:furnace[lit]",
          "core:furnace[lit=maybe]", "core:furnace[heat=1]", "core:furnace[lit=true,lit=false]",
          "core:furnace[lit=true,]", "core:stone[lit=true]"}) {
        INFO(key);
        CHECK_FALSE(registry.find(key).has_value());
    }
}

TEST_CASE("a pack with more states than ids fails, naming its largest blocks") {
    // Four blocks of 4 x 16 x 16 x 16 = 16,384 states make 65,537 with air:
    // one more than a 16-bit id numbers.
    std::vector<vx::SourceFile> files;
    for (const char* const name : {"a", "b", "c", "d"}) {
        files.push_back(test_block(name, {4, 16, 16, 16}));
    }
    const vx::BlockLoad load = vx::BlockRegistry::load(files);
    CHECK_FALSE(load.registry.has_value());
    REQUIRE(load.diagnostics.size() == 1);
    CHECK(vx::format(load.diagnostics[0]) ==
          "assets/test/blocks/a.toml: error: the blocks have 65,537 states, more than the 65,536 a "
          "state id "
          "numbers; the largest: test:a (16,384), test:b (16,384), test:c (16,384)");

    // Blocks of different sizes, far over: the true total and counts, the
    // largest first, on the largest block's file.
    const vx::BlockLoad far = vx::BlockRegistry::load(
        std::vector<vx::SourceFile>{test_block("a", {2}), test_block("b", {300, 300}),
                                    test_block("c", {16, 16}), test_block("d", {100, 1'000})});
    CHECK_FALSE(far.registry.has_value());
    REQUIRE(far.diagnostics.size() == 1);
    CHECK(vx::format(far.diagnostics[0]) ==
          "assets/test/blocks/d.toml: error: the blocks have 190,259 states, more than the 65,536 "
          "a state id numbers; the largest: test:d (100,000), test:b (90,000), test:c (256)");

    // With 3 x 43 x 127 = 16,383 states in the last block, they fill every
    // id exactly.
    files.back() = test_block("d", {3, 43, 127});
    const vx::BlockLoad fits = vx::BlockRegistry::load(files);
    REQUIRE(fits.registry.has_value());
    CHECK(fits.registry->state_count() == 65'536);
    const vx::BlockState last = state(65'535);
    CHECK(fits.registry->key(last) == "test:d[p0=2,p1=42,p2=126]");
    CHECK(fits.registry->find("test:d[p0=2,p1=42,p2=126]") == last);
}

TEST_CASE("a block defined twice is an error, and so is any bad file, every problem reported") {
    std::vector<vx::SourceFile> files = example_pack();
    files.push_back(
        {.path = "assets/core/blocks/stone.toml", .text = "[textures]\nall = \"stone2\"\n"});
    files.push_back({.path = "assets/core/blocks/glass.toml",
                     .text = "opacty = \"cutout\"\n[textures]\nall = \"glass\"\n"});
    const vx::BlockLoad load = vx::BlockRegistry::load(files);
    CHECK_FALSE(load.registry.has_value());
    std::vector<std::string> messages;
    for (const vx::Diagnostic& diagnostic : load.diagnostics) {
        messages.push_back(vx::format(diagnostic));
    }
    CHECK(messages ==
          std::vector<std::string>{
              "assets/core/blocks/glass.toml:1:1: error: unknown key 'opacty'; did you mean "
              "'opacity'?",
              "assets/core/blocks/stone.toml: error: core:stone is defined twice, also in "
              "assets/core/blocks/stone.toml",
          });
}
