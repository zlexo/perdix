#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/world/block_registry.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

TEST_CASE("the shipped pack's texture map reads, and maps every texture its blocks use") {
    // RFC 0009: any warning fails this, so a core texture added without its
    // counterpart in Minecraft, or a key left behind by one taken away,
    // cannot go unnoticed.
    const std::filesystem::path root = VX_SHIPPED_PACK_DIR;
    const vx::BlockLoad blocks = vx::BlockRegistry::load(root);
    std::string block_problems;
    for (const vx::Diagnostic& diagnostic : blocks.diagnostics) {
        block_problems += vx::format(diagnostic) + "\n";
    }
    INFO(block_problems);
    REQUIRE(blocks.registry.has_value());
    // As the app reads it: every namespace's map in the pack.
    vx::minecraft::TextureMapsLoad load = vx::minecraft::load_texture_maps(root);
    for (const vx::minecraft::TextureMap& map : load.maps) {
        vx::minecraft::check_texture_map(map, blocks.registry->textures(), load.diagnostics);
    }
    std::string problems;
    for (const vx::Diagnostic& diagnostic : load.diagnostics) {
        problems += vx::format(diagnostic) + "\n";
    }
    INFO(problems);
    CHECK(load.diagnostics.empty());
    REQUIRE(load.maps.size() == 1);
    CHECK(load.maps[0].path == "assets/core/minecraft.toml");
    CHECK(load.maps[0].textures.size() == 11);
}
