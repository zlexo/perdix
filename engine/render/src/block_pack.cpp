#include <vx/render/block_pack.hpp>

#include <vx/core/diagnostic.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/world/block_registry.hpp>

#include <cstddef>
#include <filesystem>
#include <utility>

namespace vx::render {

BlockPack load_block_pack(const std::filesystem::path& root) {
    BlockLoad load = BlockRegistry::load(root);
    BlockPack pack{.registry = std::move(load.registry),
                   .textures = {},
                   .diagnostics = std::move(load.diagnostics)};
    if (!pack.registry) {
        return pack;
    }
    const std::size_t first = pack.diagnostics.size();
    pack.textures =
        build_block_textures(pack.registry->textures(), root / "assets", pack.diagnostics);
    // The textures are named by their paths under the root it was given;
    // relative to it, as the block files are.
    for (std::size_t index = first; index < pack.diagnostics.size(); ++index) {
        Diagnostic& diagnostic = pack.diagnostics[index];
        diagnostic.file =
            std::filesystem::path(diagnostic.file).lexically_relative(root).generic_string();
    }
    return pack;
}

} // namespace vx::render
