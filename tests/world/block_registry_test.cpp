#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_physics.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
side = "furnace_side"
north = "furnace_front"
top = "furnace_top"
bottom = "furnace_top"
[[variant]]
when = { lit = true }
textures = { north = "furnace_front_lit" }
[[variant]]
when = { facing = "east" }
rotate_y = 90
[[variant]]
when = { facing = "south" }
rotate_y = 180
[[variant]]
when = { facing = "west" }
rotate_y = 270
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
[[variant]]
when = { axis = "z" }
rotate_x = 90
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

// A layer as "core:oak_log/1": its texture and its clockwise quarter turns.
std::string layer_text(const vx::BlockRegistry& registry, std::uint16_t layer) {
    const vx::TextureRef& texture = registry.textures()[layer];
    return texture.name + '/' + std::to_string(texture.quarter_turns);
}

// A state's six faces, in Face order (+x, -x, +y, -y, +z, -z), as layer_text.
std::vector<std::string> faces_of(const vx::BlockRegistry& registry, std::string_view key) {
    const std::optional<vx::BlockState> found = registry.find(key);
    REQUIRE(found.has_value());
    std::vector<std::string> faces;
    for (const std::uint16_t layer : registry.visuals()[static_cast<std::size_t>(*found)].texture) {
        faces.push_back(layer_text(registry, layer));
    }
    return faces;
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
          "core:furnace[lit=true,]", "core:stone[lit=true]", "core:furnace[lit=truex"}) {
        INFO(key);
        CHECK_FALSE(registry.find(key).has_value());
    }
}

TEST_CASE("files are taken in path order whatever order they come in, and so are diagnostics") {
    const vx::BlockLoad load = vx::BlockRegistry::load(std::vector<vx::SourceFile>{
        {.path = "assets/test/blocks/b.toml", .text = "flavour = 1\n[textures]\nall = \"b\"\n"},
        {.path = "assets/test/blocks/a.toml", .text = "flavour = 1\n[textures]\nall = \"a\"\n"}});
    REQUIRE(load.diagnostics.size() == 2);
    CHECK(load.diagnostics[0].file == "assets/test/blocks/a.toml");
    CHECK(load.diagnostics[1].file == "assets/test/blocks/b.toml");
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

TEST_CASE("one visual per state: air has no faces, and layer 0 is the checker") {
    const vx::BlockRegistry registry = loaded(example_pack());
    REQUIRE(registry.visuals().size() == registry.state_count());
    CHECK(registry.visuals()[0] == vx::BlockVisual{});
    CHECK(registry.textures()[0] == vx::TextureRef{});
    const vx::BlockVisual stone =
        registry.visuals()[static_cast<std::size_t>(*registry.find("core:stone"))];
    CHECK(stone.opacity == vx::Opacity::opaque);
    CHECK(stone.cull_group == 0);
    CHECK(faces_of(registry, "core:stone") == std::vector<std::string>(6, "core:stone/0"));
}

TEST_CASE("a single face beats side, which beats all, and variants name faces over them") {
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/test/blocks/box.toml", .text = R"([properties]
open = [false, true]
[textures]
north = "front"
all = "plain"
side = "side"
[[variant]]
when = { open = true }
textures = { side = "open_side", top = "open_top" }
)"},
    });
    // +x, -x, +y, -y, +z, -z: north is -z.
    CHECK(faces_of(registry, "test:box[open=false]") ==
          std::vector<std::string>{"test:side/0", "test:side/0", "test:plain/0", "test:plain/0",
                                   "test:side/0", "test:front/0"});
    // The variant's side covers the block's own north as well.
    CHECK(faces_of(registry, "test:box[open=true]") ==
          std::vector<std::string>{"test:open_side/0", "test:open_side/0", "test:open_top/0",
                                   "test:plain/0", "test:open_side/0", "test:open_side/0"});
}

TEST_CASE("a log along x has its ends on +x and -x, and its bark turned to run along x") {
    const vx::BlockRegistry registry = loaded(example_pack());
    CHECK(faces_of(registry, "core:oak_log[axis=y]") ==
          std::vector<std::string>{"core:oak_log/0", "core:oak_log/0", "core:oak_log_top/0",
                                   "core:oak_log_top/0", "core:oak_log/0", "core:oak_log/0"});
    // An odd number of quarter turns lays the bark's grain along x on the
    // four faces around the log.
    CHECK(faces_of(registry, "core:oak_log[axis=x]") ==
          std::vector<std::string>{"core:oak_log_top/2", "core:oak_log_top/0", "core:oak_log/1",
                                   "core:oak_log/1", "core:oak_log/1", "core:oak_log/3"});
    // Along z: the ends on +z and -z, and the grain along z, unturned on the
    // top face, where up in the image is north.
    CHECK(faces_of(registry, "core:oak_log[axis=z]") ==
          std::vector<std::string>{"core:oak_log/1", "core:oak_log/3", "core:oak_log/0",
                                   "core:oak_log/2", "core:oak_log_top/0", "core:oak_log_top/2"});
}

TEST_CASE("a furnace facing east has its front on +x, and its top turned with it") {
    const vx::BlockRegistry registry = loaded(example_pack());
    CHECK(faces_of(registry, "core:furnace[facing=north]") ==
          std::vector<std::string>{"core:furnace_side/0", "core:furnace_side/0",
                                   "core:furnace_top/0", "core:furnace_top/0",
                                   "core:furnace_side/0", "core:furnace_front/0"});
    CHECK(faces_of(registry, "core:furnace[facing=east]") ==
          std::vector<std::string>{"core:furnace_front/0", "core:furnace_side/0",
                                   "core:furnace_top/1", "core:furnace_top/3",
                                   "core:furnace_side/0", "core:furnace_side/0"});
    CHECK(faces_of(registry, "core:furnace[facing=east,lit=true]")[0] ==
          "core:furnace_front_lit/0");
    CHECK(faces_of(registry, "core:furnace[facing=south]")[4] == "core:furnace_front/0");
    CHECK(faces_of(registry, "core:furnace[facing=west]")[1] == "core:furnace_front/0");
}

TEST_CASE("every combination of rotate_x and rotate_y matches a hand-computed face map") {
    // A die: each face its own texture, e w t b s n for east, west, top,
    // bottom, south and north, so every face shows where it went.
    std::string text = R"([properties]
rx = [0, 90, 180, 270]
ry = [0, 90, 180, 270]
[textures]
east = "e"
west = "w"
top = "t"
bottom = "b"
south = "s"
north = "n"
)";
    for (const char* const angle : {"90", "180", "270"}) {
        text += std::string("[[variant]]\nwhen = { rx = ") + angle + " }\nrotate_x = " + angle +
                "\n[[variant]]\nwhen = { ry = " + angle + " }\nrotate_y = " + angle + "\n";
    }
    const vx::BlockRegistry registry =
        loaded({{.path = "assets/test/blocks/die.toml", .text = text}});

    // Worked out apart from the engine, with rotation matrices and each
    // image's turn measured as an angle, then checked by hand against the
    // log and the furnace above. Each entry is the texture a face shows and
    // its clockwise quarter turns, the faces in the order +x, -x, +y, -y,
    // +z, -z.
    struct Expected {
        int rx;
        int ry;
        std::array<const char*, 6> faces;
    };
    const std::array<Expected, 16> expected = {{
        {.rx = 0, .ry = 0, .faces = {"e0", "w0", "t0", "b0", "s0", "n0"}},
        {.rx = 0, .ry = 90, .faces = {"n0", "s0", "t1", "b3", "e0", "w0"}},
        {.rx = 0, .ry = 180, .faces = {"w0", "e0", "t2", "b2", "n0", "s0"}},
        {.rx = 0, .ry = 270, .faces = {"s0", "n0", "t3", "b1", "w0", "e0"}},
        {.rx = 90, .ry = 0, .faces = {"e1", "w3", "s0", "n2", "b0", "t2"}},
        {.rx = 90, .ry = 90, .faces = {"t2", "b0", "s1", "n1", "e1", "w3"}},
        {.rx = 90, .ry = 180, .faces = {"w3", "e1", "s2", "n0", "t2", "b0"}},
        {.rx = 90, .ry = 270, .faces = {"b0", "t2", "s3", "n3", "w3", "e1"}},
        {.rx = 180, .ry = 0, .faces = {"e2", "w2", "b0", "t0", "n2", "s2"}},
        {.rx = 180, .ry = 90, .faces = {"s2", "n2", "b1", "t3", "e2", "w2"}},
        {.rx = 180, .ry = 180, .faces = {"w2", "e2", "b2", "t2", "s2", "n2"}},
        {.rx = 180, .ry = 270, .faces = {"n2", "s2", "b3", "t1", "w2", "e2"}},
        {.rx = 270, .ry = 0, .faces = {"e3", "w1", "n2", "s0", "t0", "b2"}},
        {.rx = 270, .ry = 90, .faces = {"b2", "t0", "n3", "s3", "e3", "w1"}},
        {.rx = 270, .ry = 180, .faces = {"w1", "e3", "n0", "s2", "b2", "t0"}},
        {.rx = 270, .ry = 270, .faces = {"t0", "b2", "n1", "s1", "w1", "e3"}},
    }};
    for (const Expected& each : expected) {
        const std::string key =
            "test:die[rx=" + std::to_string(each.rx) + ",ry=" + std::to_string(each.ry) + "]";
        INFO(key);
        std::vector<std::string> faces;
        for (const std::string& face : faces_of(registry, key)) {
            // "test:e/1" as "e1".
            faces.push_back(face.substr(5, 1) + face.substr(7));
        }
        CHECK(faces == std::vector<std::string>(each.faces.begin(), each.faces.end()));
    }
}

TEST_CASE("the turns of every matching variant add up, past a whole turn") {
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/test/blocks/die.toml", .text = R"([properties]
a = [false, true]
b = [false, true]
[textures]
east = "e"
west = "w"
top = "t"
bottom = "b"
south = "s"
north = "n"
[[variant]]
when = { a = true }
rotate_x = 270
rotate_y = 90
[[variant]]
when = { b = true }
rotate_x = 180
rotate_y = 180
[[variant]]
when = { a = false, b = false }
rotate_x = 90
rotate_y = 270
)"},
    });
    // 270 + 180 is a quarter past a whole turn: 90 about x, then 270 about y,
    // which is what the third variant gives alone.
    CHECK(faces_of(registry, "test:die[a=true,b=true]") ==
          faces_of(registry, "test:die[a=false,b=false]"));
    CHECK(faces_of(registry, "test:die[a=true,b=true]")[0] == "test:b/0");
}

TEST_CASE("a variant applies where all its conditions hold, and a list matches any of its values") {
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/test/blocks/die.toml", .text = R"([properties]
a = [false, true]
b = [false, true]
[textures]
east = "e"
west = "w"
top = "t"
bottom = "b"
south = "s"
north = "n"
[[variant]]
when = { a = true }
rotate_x = 270
rotate_y = 90
[[variant]]
when = { a = false, b = false }
rotate_x = 90
rotate_y = 270
)"},
        // The die turned as the first variant alone turns it, under one
        // condition, which "all hold" and "any holds" read alike.
        {.path = "assets/test/blocks/die_turned.toml", .text = R"([properties]
turned = [true, false]
[textures]
east = "e"
west = "w"
top = "t"
bottom = "b"
south = "s"
north = "n"
[[variant]]
when = { turned = true }
rotate_x = 270
rotate_y = 90
)"},
        {.path = "assets/test/blocks/log.toml", .text = R"([properties]
axis = ["y", "x", "z"]
[textures]
side = "bark"
top = "rings"
bottom = "rings"
[[variant]]
when = { axis = ["x", "z"] }
rotate_x = 90
)"},
    });
    // b is false but a is not, so only the first variant applies.
    CHECK(faces_of(registry, "test:die[a=true,b=false]") ==
          faces_of(registry, "test:die_turned[turned=true]"));
    // Both listed values turn the log; the value not listed does not.
    CHECK(faces_of(registry, "test:log[axis=z]") == faces_of(registry, "test:log[axis=x]"));
    CHECK(faces_of(registry, "test:log[axis=x]") != faces_of(registry, "test:log[axis=y]"));
}

TEST_CASE("a variant's all covers every face") {
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/test/blocks/sponge.toml", .text = R"([properties]
wet = [false, true]
[textures]
all = "dry"
[[variant]]
when = { wet = true }
textures = { all = "wet" }
)"},
    });
    CHECK(faces_of(registry, "test:sponge[wet=false]") ==
          std::vector<std::string>(6, "test:dry/0"));
    CHECK(faces_of(registry, "test:sponge[wet=true]") == std::vector<std::string>(6, "test:wet/0"));
}

TEST_CASE("the golden texture table: every layer, in name order, then by turns") {
    const vx::BlockRegistry registry = loaded(example_pack());
    std::vector<std::string> layers;
    for (std::size_t layer = 1; layer < registry.textures().size(); ++layer) {
        layers.push_back(layer_text(registry, static_cast<std::uint16_t>(layer)));
    }
    CHECK(layers == std::vector<std::string>{
                        "core:furnace_front/0",
                        "core:furnace_front_lit/0",
                        "core:furnace_side/0",
                        "core:furnace_top/0",
                        "core:furnace_top/1",
                        "core:furnace_top/2",
                        "core:furnace_top/3",
                        "core:oak_log/0",
                        "core:oak_log/1",
                        "core:oak_log/2",
                        "core:oak_log/3",
                        "core:oak_log_top/0",
                        "core:oak_log_top/2",
                        "core:stone/0",
                    });
    // Where each is first used, in id order, for the texture messages.
    CHECK(registry.textures()[1].first_use == "assets/core/blocks/furnace.toml:6:9");
    CHECK(registry.textures()[2].first_use == "assets/core/blocks/furnace.toml:11:22");
    // furnace_top/0 is on the top face and the bottom one: the top is first.
    CHECK(registry.textures()[4].first_use == "assets/core/blocks/furnace.toml:7:7");
    CHECK(registry.textures()[13].first_use == "assets/core/blocks/oak_log.toml:5:7");
}

TEST_CASE("texture names are in the block's namespace unless written with one") {
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/moss/blocks/stone.toml",
         .text = "[textures]\nall = \"moss_stone\"\ntop = \"core:grass\"\n"},
    });
    CHECK(faces_of(registry, "moss:stone")[2] == "core:grass/0");
    CHECK(faces_of(registry, "moss:stone")[0] == "moss:moss_stone/0");
}

TEST_CASE("each texture knows whether opaque blocks, cutout blocks or both use it") {
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/test/blocks/glass.toml",
         .text = "opacity = \"cutout\"\n[textures]\nall = \"glass\"\ntop = \"shared\"\n"},
        {.path = "assets/test/blocks/stone.toml",
         .text = "[textures]\nall = \"stone\"\ntop = \"shared\"\n"},
    });
    const auto flags = [&](std::string_view name) {
        const auto texture = std::ranges::find(registry.textures(), name, &vx::TextureRef::name);
        REQUIRE(texture != registry.textures().end());
        return std::pair{texture->used_opaque, texture->used_cutout};
    };
    CHECK(flags("test:glass") == std::pair{false, true});
    CHECK(flags("test:stone") == std::pair{true, false});
    CHECK(flags("test:shared") == std::pair{true, true});
}

TEST_CASE("cull groups are numbered in name order, and only cutout blocks have one") {
    const vx::BlockRegistry registry = loaded({
        {.path = "assets/test/blocks/oak_leaves.toml",
         .text = "opacity = \"cutout\"\ncull_group = \"leaves\"\n[textures]\nall = \"x\"\n"},
        {.path = "assets/test/blocks/birch_leaves.toml",
         .text = "opacity = \"cutout\"\ncull_group = \"leaves\"\n[textures]\nall = \"x\"\n"},
        {.path = "assets/test/blocks/glass.toml",
         .text = "opacity = \"cutout\"\ncull_group = \"glass\"\n[textures]\nall = \"x\"\n"},
        {.path = "assets/test/blocks/fence.toml",
         .text = "opacity = \"cutout\"\n[textures]\nall = \"x\"\n"},
    });
    const auto visual = [&](std::string_view key) {
        return registry.visuals()[static_cast<std::size_t>(*registry.find(key))];
    };
    CHECK(visual("test:glass").cull_group == 1);
    CHECK(visual("test:oak_leaves").cull_group == 2);
    CHECK(visual("test:birch_leaves").cull_group == 2);
    CHECK(visual("test:fence").cull_group == 0);
    CHECK(visual("test:fence").opacity == vx::Opacity::cutout);
}

TEST_CASE("the same files in any order give the same visuals and textures") {
    const vx::BlockRegistry first = loaded(example_pack());
    const std::vector<vx::BlockVisual> visuals(first.visuals().begin(), first.visuals().end());
    const std::vector<vx::TextureRef> textures(first.textures().begin(), first.textures().end());
    std::vector<vx::SourceFile> files = example_pack();
    std::mt19937 generator(7);
    for (int round = 0; round < 6; ++round) {
        std::ranges::shuffle(files, generator);
        const vx::BlockRegistry again = loaded(files);
        CHECK(std::ranges::equal(again.visuals(), visuals));
        CHECK(std::ranges::equal(again.textures(), textures));
    }
}

TEST_CASE("more cull groups than a cull group numbers fail the load") {
    // 255 groups fit; the 256th, in name order, is reported where it is named.
    std::vector<vx::SourceFile> files;
    for (int group = 0; group < 256; ++group) {
        const std::string name = "g" + std::to_string(1000 + group);
        files.push_back({.path = "assets/test/blocks/" + name + ".toml",
                         .text = "opacity = \"cutout\"\ncull_group = \"" + name +
                                 "\"\n[textures]\nall = \"x\"\n"});
    }
    const vx::BlockLoad load = vx::BlockRegistry::load(files);
    CHECK_FALSE(load.registry.has_value());
    REQUIRE(load.diagnostics.size() == 1);
    CHECK(vx::format(load.diagnostics[0]) ==
          "assets/test/blocks/g1255.toml:2:14: error: the blocks name 256 cull groups, more than "
          "the 255 a cull group number holds");

    files.pop_back();
    const vx::BlockRegistry fits = loaded(files);
    CHECK(fits.visuals()[static_cast<std::size_t>(*fits.find("test:g1254"))].cull_group == 255);
}

TEST_CASE("more texture layers than a face numbers fail the load") {
    // Dice with six textures of their own, whose 16 turns put each texture
    // in all four quarter turns: 2,730 dice make 65,520 layers. Then blocks
    // of one texture each: 15 of them fill every layer with the checker,
    // exactly 65,536, and a 16th goes one over.
    std::string turns;
    for (int turn = 1; turn < 16; ++turn) {
        turns += "[[variant]]\nwhen = { r = " + std::to_string(turn) +
                 " }\nrotate_x = " + std::to_string(90 * (turn / 4)) +
                 "\nrotate_y = " + std::to_string(90 * (turn % 4)) + "\n";
    }
    const auto pack = [&](int blocks) {
        std::vector<vx::SourceFile> files;
        for (int die = 0; die < 2'730; ++die) {
            const std::string d = std::to_string(die);
            files.push_back(
                {.path = "assets/test/blocks/die" + d + ".toml",
                 .text = "[properties]\nr = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, "
                         "15]\n[textures]\neast = \"e" +
                         d + "\"\nwest = \"w" + d + "\"\ntop = \"t" + d + "\"\nbottom = \"b" + d +
                         "\"\nsouth = \"s" + d + "\"\nnorth = \"n" + d + "\"\n" + turns});
        }
        for (int block = 0; block < blocks; ++block) {
            const std::string name = "k" + std::to_string(block);
            files.push_back({.path = "assets/test/blocks/" + name + ".toml",
                             .text = "[textures]\nall = \"" + name + "\"\n"});
        }
        return files;
    };
    const vx::BlockRegistry fits = loaded(pack(15));
    CHECK(fits.textures().size() == 65'536);
    CHECK(fits.textures().back().name == "test:w999");

    // Reported where the first texture past the limit is used, in id order:
    // k9 is the 16th block of one texture by name.
    const vx::BlockLoad load = vx::BlockRegistry::load(pack(16));
    CHECK_FALSE(load.registry.has_value());
    REQUIRE(load.diagnostics.size() == 1);
    CHECK(vx::format(load.diagnostics[0]) ==
          "assets/test/blocks/k9.toml:2:7: error: the blocks need 65,537 texture layers with the "
          "checker, more than the 65,536 a layer number holds");
}

TEST_CASE("each state's physics: air collides with nothing, every other state as its file says") {
    // RFC 0010: ice with two states, a flower and a stone.
    const vx::BlockLoad load = vx::BlockRegistry::load(std::vector<vx::SourceFile>{
        {.path = "assets/core/blocks/ice.toml",
         .text = "slipperiness = 0.98\n[properties]\nsnowy = [false, true]\n[textures]\nall = "
                 "\"ice\"\n"},
        {.path = "assets/core/blocks/poppy.toml",
         .text = "collision = \"none\"\nopacity = \"cutout\"\n[textures]\nall = \"poppy\"\n"},
        {.path = "assets/core/blocks/stone.toml", .text = "[textures]\nall = \"stone\"\n"}});
    REQUIRE(load.registry.has_value());
    const vx::BlockRegistry& registry = *load.registry;
    const std::span<const vx::BlockPhysics> physics = registry.physics();
    REQUIRE(physics.size() == registry.state_count());
    CHECK(physics[0] == vx::BlockPhysics{.collision = vx::Collision::none, .slipperiness = 0.6});
    const vx::BlockPhysics ice{.collision = vx::Collision::full, .slipperiness = 0.98};
    CHECK(physics[static_cast<std::size_t>(*registry.find("core:ice[snowy=false]"))] == ice);
    CHECK(physics[static_cast<std::size_t>(*registry.find("core:ice[snowy=true]"))] == ice);
    CHECK(physics[static_cast<std::size_t>(*registry.find("core:poppy"))].collision ==
          vx::Collision::none);
    CHECK(physics[static_cast<std::size_t>(*registry.find("core:stone"))] ==
          vx::BlockPhysics{.collision = vx::Collision::full, .slipperiness = 0.6});
}
