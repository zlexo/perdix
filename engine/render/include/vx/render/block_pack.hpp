#pragma once

#include <vx/core/diagnostic.hpp>
#include <vx/world/block_registry.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace vx::render {

// A block pack read from disk and made ready for the GPU (RFC 0004): its
// registry, and its texture array's bytes as build_block_textures lays them
// out. Each step runs only when the one before it had no error: a pack whose
// blocks do not load has no textures built.
struct BlockPack {
    std::optional<BlockRegistry> registry; // empty when a block file has an error
    std::vector<std::byte> textures;       // empty after any error
    std::vector<Diagnostic> diagnostics;   // the blocks', then the textures'
};

// The pack under `root`: every assets/<namespace>/blocks/*.toml and the
// textures they name, assets/<namespace>/textures/blocks/<name>.png. Every
// diagnostic names its file relative to `root`, as BlockRegistry::load does,
// except those about the root itself, such as a missing assets folder,
// which name it as given.
[[nodiscard]] BlockPack load_block_pack(const std::filesystem::path& root);

} // namespace vx::render
