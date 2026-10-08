#pragma once

#include <vx/core/result.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/chunk_storage.hpp>

namespace vx::app {

// M2's one chunk (RFC 0003's milestone image), built by hand from the core
// pack: ground of stone, dirt and grass with a sand beach, an oak tree,
// logs lying along x and along z, and a hut of planks and cobblestone with
// a glass window. Every block in the pack, the three ways a log lies, both
// cutouts, and corners for ambient occlusion to darken. An error names a
// block the pack does not have.
[[nodiscard]] Result<ChunkStorage> demo_chunk(const BlockRegistry& registry);

} // namespace vx::app
