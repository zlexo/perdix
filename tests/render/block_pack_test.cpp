#include <vx/core/diagnostic.hpp>
#include <vx/render/block_pack.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

// A pack in a folder of its own under the system's temporary folder,
// removed when the test ends.
class TempPack {
public:
    TempPack() {
        std::random_device random;
        root_ = std::filesystem::temp_directory_path() /
                ("vx-render-pack-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::filesystem::create_directories(root_);
    }
    TempPack(const TempPack&) = delete;
    TempPack& operator=(const TempPack&) = delete;
    TempPack(TempPack&&) = delete;
    TempPack& operator=(TempPack&&) = delete;
    ~TempPack() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    // Writes `text` to the file at `relative`, a path under the root.
    void write(const std::string& relative, const std::string& text) const {
        const std::filesystem::path path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    // Copies one of the texture tests' files to `relative`.
    void copy_texture(const std::string& name, const std::string& relative) const {
        const std::filesystem::path path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::copy_file(std::filesystem::path(VX_BLOCK_TEXTURES_DIR) / "test" /
                                       "textures" / "blocks" / (name + ".png"),
                                   path);
    }

private:
    std::filesystem::path root_;
};

} // namespace

TEST_CASE("a pack loads its blocks and builds every layer of its textures") {
    const TempPack pack;
    pack.write("assets/t/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    pack.copy_texture("solid", "assets/t/textures/blocks/rock.png");
    const vx::render::BlockPack loaded = vx::render::load_block_pack(pack.root());
    std::string problems;
    for (const vx::Diagnostic& diagnostic : loaded.diagnostics) {
        problems += vx::format(diagnostic) + "\n";
    }
    INFO(problems);
    CHECK(loaded.diagnostics.empty());
    REQUIRE(loaded.registry.has_value());
    // The checker and the rock.
    REQUIRE(loaded.registry->textures().size() == 2);
    CHECK(loaded.textures.size() == 2 * vx::render::block_texture_layer_bytes);
}

TEST_CASE("a pack whose texture is missing keeps its registry, has no bytes, and says where") {
    const TempPack pack;
    pack.write("assets/t/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    const vx::render::BlockPack loaded = vx::render::load_block_pack(pack.root());
    CHECK(loaded.registry.has_value());
    CHECK(loaded.textures.empty());
    REQUIRE(loaded.diagnostics.size() == 1);
    CHECK(loaded.diagnostics[0].severity == vx::Severity::error);
    // Relative to the root, as a block file is named.
    CHECK(loaded.diagnostics[0].file == "assets/t/textures/blocks/rock.png");
    CHECK(loaded.diagnostics[0].message.find("no such file") != std::string::npos);
}

TEST_CASE("a pack whose block file has an error builds no textures") {
    const TempPack pack;
    pack.write("assets/t/blocks/rock.toml", "[textures]\nal = \"rock\"\n");
    const vx::render::BlockPack loaded = vx::render::load_block_pack(pack.root());
    CHECK_FALSE(loaded.registry.has_value());
    CHECK(loaded.textures.empty());
    REQUIRE_FALSE(loaded.diagnostics.empty());
    // The block file's problems alone: no texture was looked for.
    for (const vx::Diagnostic& diagnostic : loaded.diagnostics) {
        CHECK(diagnostic.file == "assets/t/blocks/rock.toml");
    }
}

TEST_CASE("the shipped pack loads with no errors and no warnings, and builds every layer") {
    const vx::render::BlockPack pack = vx::render::load_block_pack(VX_SHIPPED_PACK_DIR);
    std::string problems;
    for (const vx::Diagnostic& diagnostic : pack.diagnostics) {
        problems += vx::format(diagnostic) + "\n";
    }
    INFO(problems);
    CHECK(pack.diagnostics.empty());
    REQUIRE(pack.registry.has_value());
    CHECK(pack.registry->state_count() == 12);
    REQUIRE(pack.registry->textures().size() == 16);
    CHECK(pack.textures.size() == 16 * vx::render::block_texture_layer_bytes);
}

TEST_CASE("the shipped pack's blocks show the faces the owner chose") {
    // What each state shows on +x, -x, +y, -y, +z and -z, as texture@quarter
    // turns: grass has its top up and dirt below; a log shows its rings on
    // the two faces along its axis and its bark turned to run along it; leaves
    // and glass are cutouts in groups of their own.
    struct Expected {
        const char* key;
        vx::Opacity opacity;
        std::array<const char*, 6> faces;
    };
    constexpr vx::Opacity opaque = vx::Opacity::opaque;
    constexpr vx::Opacity cutout = vx::Opacity::cutout;
    const std::array<Expected, 11> table{{
        {"core:cobblestone",
         opaque,
         {"cobblestone@0", "cobblestone@0", "cobblestone@0", "cobblestone@0", "cobblestone@0",
          "cobblestone@0"}},
        {"core:dirt", opaque, {"dirt@0", "dirt@0", "dirt@0", "dirt@0", "dirt@0", "dirt@0"}},
        {"core:glass", cutout, {"glass@0", "glass@0", "glass@0", "glass@0", "glass@0", "glass@0"}},
        {"core:grass",
         opaque,
         {"grass_side@0", "grass_side@0", "grass_top@0", "dirt@0", "grass_side@0", "grass_side@0"}},
        {"core:oak_leaves",
         cutout,
         {"oak_leaves@0", "oak_leaves@0", "oak_leaves@0", "oak_leaves@0", "oak_leaves@0",
          "oak_leaves@0"}},
        {"core:oak_log[axis=y]",
         opaque,
         {"oak_log@0", "oak_log@0", "oak_log_top@0", "oak_log_top@0", "oak_log@0", "oak_log@0"}},
        {"core:oak_log[axis=x]",
         opaque,
         {"oak_log_top@2", "oak_log_top@0", "oak_log@1", "oak_log@1", "oak_log@1", "oak_log@3"}},
        {"core:oak_log[axis=z]",
         opaque,
         {"oak_log@1", "oak_log@3", "oak_log@0", "oak_log@2", "oak_log_top@0", "oak_log_top@2"}},
        {"core:oak_planks",
         opaque,
         {"oak_planks@0", "oak_planks@0", "oak_planks@0", "oak_planks@0", "oak_planks@0",
          "oak_planks@0"}},
        {"core:sand", opaque, {"sand@0", "sand@0", "sand@0", "sand@0", "sand@0", "sand@0"}},
        {"core:stone", opaque, {"stone@0", "stone@0", "stone@0", "stone@0", "stone@0", "stone@0"}},
    }};
    const vx::render::BlockPack pack = vx::render::load_block_pack(VX_SHIPPED_PACK_DIR);
    REQUIRE(pack.registry.has_value());
    const vx::BlockRegistry& registry = *pack.registry;
    for (const Expected& expected : table) {
        INFO(expected.key);
        const std::optional<vx::BlockState> state = registry.find(expected.key);
        REQUIRE(state.has_value());
        const vx::BlockVisual& visual = registry.visuals()[static_cast<std::size_t>(*state)];
        CHECK(visual.opacity == expected.opacity);
        for (std::size_t face = 0; face < 6; ++face) {
            const vx::TextureRef& texture = registry.textures()[visual.texture[face]];
            CHECK(texture.name + "@" + std::to_string(texture.quarter_turns) ==
                  std::string("core:") + expected.faces[face]);
        }
    }
    // Every state is in the table, once.
    CHECK(registry.state_count() == table.size() + 1);
    // Leaves hide leaves and glass hides glass, and neither hides the other.
    const auto group = [&](const char* key) {
        return registry.visuals()[static_cast<std::size_t>(*registry.find(key))].cull_group;
    };
    CHECK(group("core:glass") != 0);
    CHECK(group("core:oak_leaves") != 0);
    CHECK(group("core:glass") != group("core:oak_leaves"));
}

TEST_CASE("a pack's diagnostics name their files under its root, from blocks and textures alike") {
    // One warning from the registry, a texture no block uses, and one from
    // the textures, holes in a texture an opaque block uses. Only the
    // textures' come back with paths to make relative, and the registry's
    // must be left as they are.
    const TempPack pack;
    pack.write("assets/t/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    pack.copy_texture("leaves", "assets/t/textures/blocks/rock.png");
    pack.copy_texture("solid", "assets/t/textures/blocks/moss.png");
    const vx::render::BlockPack loaded = vx::render::load_block_pack(pack.root());
    std::vector<std::string> lines;
    for (const vx::Diagnostic& diagnostic : loaded.diagnostics) {
        lines.push_back(vx::format(diagnostic));
    }
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "assets/t/textures/blocks/moss.png: warning: no block uses this texture");
    CHECK(lines[1].starts_with("assets/t/textures/blocks/rock.png: warning: "));
    CHECK(lines[1].find("texels are transparent") != std::string::npos);
}
