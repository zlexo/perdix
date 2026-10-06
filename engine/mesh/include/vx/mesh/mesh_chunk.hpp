#pragma once

#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_visual.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace vx {

// Which pass draws a quad (RFC 0003): opaque faces, then cutout ones such as
// leaves and glass, which need an alpha test and which Apple's GPUs want
// drawn after everything opaque.
enum class Pass : std::uint8_t { opaque, cutout };

// A chunk's quads, by pass and by the way they face.
struct ChunkMesh {
    // ranges[pass][face], indexed by Pass and by Face (+x, -x, +y, -y, +z,
    // -z). The twelve lie back to back in that order: a pass's six are one
    // span (pass()), and all twelve are all().
    std::array<std::array<std::span<const Quad>, 6>, 2> ranges;

    [[nodiscard]] std::span<const Quad> range(Pass pass, Face face) const noexcept {
        return ranges[static_cast<std::size_t>(pass)][static_cast<std::size_t>(face)];
    }
    // A pass's quads, its six faces in order.
    [[nodiscard]] std::span<const Quad> pass(Pass which) const noexcept;
    // Every quad, opaque then cutout.
    [[nodiscard]] std::span<const Quad> all() const noexcept;
};

struct MeshOptions {
    // Faces that look the same, side by side, become one quad. Off for the
    // overlay's toggle, to see and measure what merging saves (RFC 0003).
    bool merge = true;
};

// The mesher's working memory, reused from chunk to chunk: one for each
// thread that meshes (RFC 0005), never shared. Once made, meshing allocates
// nothing. About 1.6 MiB, most of it room for the worst case's 196,608 quads.
class MeshScratch {
public:
    MeshScratch();

    // For each row of a padded chunk along x, at padded (y, z): bit x set
    // where the block is opaque, or cutout.
    using Rows = std::array<std::array<std::uint64_t, padded_size>, padded_size>;
    // For each pass and face, each of the 32 slices a face direction has in
    // a chunk, and each row of a slice: bit u set where a face is visible.
    // Rows and bits run along the face's height and width axes (face_axes).
    using Planes =
        std::array<std::array<std::array<std::array<std::uint32_t, chunk_size>, chunk_size>, 6>, 2>;
    // The material of each visible face of one slice, [row][bit]: its texture
    // and occlusion. Faces merge where their materials are equal.
    using SliceKeys = std::array<std::array<std::uint32_t, chunk_size>, chunk_size>;

private:
    friend ChunkMesh mesh_chunk(const PaddedChunk& blocks, std::span<const BlockVisual> visuals,
                                MeshScratch& scratch, MeshOptions options) noexcept;

    std::unique_ptr<Rows> opaque_;
    std::unique_ptr<Rows> cutout_;
    std::unique_ptr<Planes> planes_;
    std::unique_ptr<SliceKeys> keys_;
    std::unique_ptr<Quad[]> quads_; // NOLINT(modernize-avoid-c-arrays)
};

// The most quads a chunk can need: every block facing all six ways, as in a
// checkerboard of two cutout blocks that do not hide each other.
inline constexpr std::size_t max_chunk_quads = std::size_t{6} * chunk_volume; // 196,608

// The quads that show the chunk inside `blocks` (RFC 0003): a face for
// every place where a block that draws faces meets one that does not hide
// them, darkened at its corners by the opaque blocks around them, and
// merged with its neighbors in a slice where they look the same.
// `visuals` is the block registry's table, indexed by state, and must cover
// every state in `blocks`. The mesh points into `scratch`, so it is valid
// until the scratch's next use.
[[nodiscard]] ChunkMesh mesh_chunk(const PaddedChunk& blocks, std::span<const BlockVisual> visuals,
                                   MeshScratch& scratch, MeshOptions options = {}) noexcept;

} // namespace vx
