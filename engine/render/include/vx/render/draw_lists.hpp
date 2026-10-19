#pragma once

#include <vx/core/frustum.hpp>
#include <vx/render/chunk_uploads.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <volk.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace vx::render {

// RFC 0007's culling and merging, each with an overlay switch.
struct DrawSwitches {
    bool frustum = true; // chunks outside the view are not drawn
    bool faces = true;   // face groups that cannot face the camera are not drawn
    bool merge = true;   // adjacent groups drawn together make one command
};

// Which of a chunk's six face directions can face the camera: a bit for each
// arena group, bit 0 for +x, then +y, +z, -x, -y, -z (arena_faces). With
// `corner` the chunk's lowest block relative to the camera's block, faces
// along +x need corner.x <= 0 and faces along -x corner.x >= -(span - 1),
// -31 at level 0, and the same for y and z: integer tests, conservative by
// one block (RFC 0007).
// `span` is the chunk's size in blocks: 32 at level 0, 32 x 2^L at RFC 0011's
// level L.
[[nodiscard]] std::uint8_t facing_mask(const glm::i64vec3& corner,
                                       std::int64_t span = chunk_size) noexcept;

// Appends a command for each run of one pass's six groups, in arena order,
// that is drawn: groups whose bit is set in `drawn`, merged with their
// neighbors and the empty groups between them when `merge` is on.
// `first_quad` is the pass's first quad in the arena; `origin` the chunk's
// slot among the frame's origins, the commands' first instance.
void append_runs(std::span<const std::uint16_t, 6> counts, std::uint8_t drawn,
                 std::uint32_t first_quad, std::uint32_t origin, bool merge,
                 std::vector<VkDrawIndexedIndirectCommand>& out);

// What the culling did, for the overlay.
struct DrawStats {
    std::uint32_t chunks = 0; // chunks drawn
    std::uint32_t culled = 0; // chunks outside the view
    std::uint64_t quads = 0;  // quads drawn
    std::uint64_t quads_facing_away = 0;
};

// A frame's draw lists (RFC 0007), built afresh on the main thread from the
// arena's records.
struct DrawLists {
    // A chunk's lowest block relative to the camera's block, one for each
    // chunk drawn: a whole number, exact as a float within 2^24 blocks. A
    // command's first instance picks it.
    std::vector<glm::vec4> origins;
    std::vector<VkDrawIndexedIndirectCommand> opaque;
    std::vector<VkDrawIndexedIndirectCommand> cutout;
    DrawStats stats;
};

// Fills `lists` for a camera at `camera_block` plus `camera_fraction`, which
// sees through `view`, a frustum relative to the camera.
void build_draw_lists(std::span<const ChunkMeshRecord> records, const glm::i64vec3& camera_block,
                      const glm::vec3& camera_fraction, const Frustum& view,
                      const DrawSwitches& switches, DrawLists& lists);

// Calls `draw(first, count)` for consecutive slices of `commands` commands,
// at most `limit` a slice: a device's maxDrawIndirectCount bounds one call
// (65,535 on KosmicKrisp).
template <class F> void for_each_call(std::uint32_t commands, std::uint32_t limit, F&& draw) {
    assert(limit > 0);
    std::uint32_t first = 0;
    while (first < commands) {
        const std::uint32_t count = std::min(limit, commands - first);
        draw(first, count);
        first += count;
    }
}

} // namespace vx::render
