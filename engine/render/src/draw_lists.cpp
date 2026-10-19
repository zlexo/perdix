#include <vx/render/draw_lists.hpp>

#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/coordinates.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vx::render {
namespace {

void add_command(std::uint32_t first, std::uint32_t quads, std::uint32_t origin,
                 std::vector<VkDrawIndexedIndirectCommand>& out) {
    // gl_VertexIndex >> 2 is the quad's index in the arena: the run starts
    // at its first quad's first vertex, and RFC 0003's index buffer serves
    // every command from its start.
    out.push_back({.indexCount = 6 * quads,
                   .instanceCount = 1,
                   .firstIndex = 0,
                   .vertexOffset = static_cast<std::int32_t>(4 * first),
                   .firstInstance = origin});
}

} // namespace

std::uint8_t facing_mask(const glm::i64vec3& corner, std::int64_t span) noexcept {
    std::uint8_t mask = 0;
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        if (corner[axis] <= 0) {
            mask = static_cast<std::uint8_t>(mask | (1U << static_cast<unsigned>(axis)));
        }
        if (corner[axis] >= -(span - 1)) {
            mask = static_cast<std::uint8_t>(mask | (1U << static_cast<unsigned>(axis + 3)));
        }
    }
    return mask;
}

void append_runs(std::span<const std::uint16_t, 6> counts, std::uint8_t drawn,
                 std::uint32_t first_quad, std::uint32_t origin, bool merge,
                 std::vector<VkDrawIndexedIndirectCommand>& out) {
    std::uint32_t at = first_quad;
    std::uint32_t run_first = 0;
    std::uint32_t run_quads = 0;
    for (std::size_t group = 0; group < counts.size(); ++group) {
        const std::uint32_t quads = counts[group];
        const bool draw = (drawn & (1U << group)) != 0;
        if (quads == 0) {
            continue; // an empty group neither adds to a run nor breaks it
        }
        if (draw && run_quads > 0 && merge) {
            run_quads += quads;
        } else {
            if (run_quads > 0) {
                add_command(run_first, run_quads, origin, out);
                run_quads = 0;
            }
            if (draw) {
                run_first = at;
                run_quads = quads;
            }
        }
        at += quads;
    }
    if (run_quads > 0) {
        add_command(run_first, run_quads, origin, out);
    }
}

void build_draw_lists(std::span<const ChunkMeshRecord> records, const glm::i64vec3& camera_block,
                      const glm::vec3& camera_fraction, const Frustum& view,
                      const DrawSwitches& switches, DrawLists& lists) {
    lists.origins.clear();
    lists.opaque.clear();
    lists.cutout.clear();
    lists.stats = {};
    for (const ChunkMeshRecord& record : records) {
        // A chunk of RFC 0011's level L spans 32 x 2^L blocks.
        const std::int64_t span = std::int64_t{chunk_size} << record.level;
        const glm::i64vec3 corner =
            (glm::i64vec3(record.pos.x, record.pos.y, record.pos.z) * span) - camera_block;
        // The chunk's cube relative to the camera, in doubles.
        const Vec3d min{.x = static_cast<double>(corner.x) - static_cast<double>(camera_fraction.x),
                        .y = static_cast<double>(corner.y) - static_cast<double>(camera_fraction.y),
                        .z =
                            static_cast<double>(corner.z) - static_cast<double>(camera_fraction.z)};
        const auto size = static_cast<double>(span);
        const Vec3d max{.x = min.x + size, .y = min.y + size, .z = min.z + size};
        if (switches.frustum && !view.may_contain(min, max)) {
            ++lists.stats.culled;
            continue;
        }
        const std::uint8_t facing = switches.faces ? facing_mask(corner, span) : 0x3F;
        const std::span<const std::uint16_t, 12> count(record.count);
        std::uint32_t opaque_quads = 0;
        for (std::size_t group = 0; group < 12; ++group) {
            const std::uint32_t quads = count[group];
            if (group < 6) {
                opaque_quads += quads;
            }
            if ((facing & (1U << (group % 6))) != 0) {
                lists.stats.quads += quads;
            } else {
                lists.stats.quads_facing_away += quads;
            }
        }
        const auto origin = static_cast<std::uint32_t>(lists.origins.size());
        const std::size_t before = lists.opaque.size() + lists.cutout.size();
        append_runs(count.first<6>(), facing, record.range.first_quad, origin, switches.merge,
                    lists.opaque);
        append_runs(count.last<6>(), facing, record.range.first_quad + opaque_quads, origin,
                    switches.merge, lists.cutout);
        if (lists.opaque.size() + lists.cutout.size() > before) {
            // The fourth component is the level, for the shader's scale.
            lists.origins.emplace_back(static_cast<float>(corner.x), static_cast<float>(corner.y),
                                       static_cast<float>(corner.z),
                                       static_cast<float>(record.level));
            ++lists.stats.chunks;
        }
    }
}

} // namespace vx::render
