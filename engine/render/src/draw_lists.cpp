#include <vx/render/draw_lists.hpp>

#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/coordinates.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <volk.h>

#include <algorithm>
#include <chrono>
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

// Quads at `first` in the arena, `quads` of them.
struct Segment {
    std::uint32_t first = 0;
    std::uint32_t quads = 0;
};

// A command for each run of segments, a segment joining the run before it
// when it starts where that run ends and `merge` is on.
void append_segments(std::span<const Segment> segments, std::uint32_t origin, bool merge,
                     std::vector<VkDrawIndexedIndirectCommand>& out) {
    std::uint32_t run_first = 0;
    std::uint32_t run_quads = 0;
    for (const Segment& segment : segments) {
        if (segment.quads == 0) {
            continue;
        }
        if (merge && run_quads > 0 && segment.first == run_first + run_quads) {
            run_quads += segment.quads;
            continue;
        }
        if (run_quads > 0) {
            add_command(run_first, run_quads, origin, out);
        }
        run_first = segment.first;
        run_quads = segment.quads;
    }
    if (run_quads > 0) {
        add_command(run_first, run_quads, origin, out);
    }
}

// RFC 0011: the arena groups (bit g for arena_faces[g]) toward which a
// chunk's own level's neighbor is not drawn.
std::uint8_t open_sides(const ChunkMeshRecord& record, std::uint8_t facing,
                        const DrawnAt& drawn_at) {
    std::uint8_t open = 0;
    for (std::size_t group = 0; group < 6; ++group) {
        if ((facing & (1U << group)) == 0 ||
            (record.open.at(group) == 0 && record.open.at(group + 6) == 0 &&
             record.border.at(group) == 0 && record.border.at(group + 6) == 0)) {
            continue;
        }
        const Face face = arena_faces.at(group);
        const std::size_t axis = static_cast<std::size_t>(face) / 2;
        const std::int32_t step = static_cast<std::size_t>(face) % 2 == 0 ? 1 : -1;
        std::array<std::int32_t, 3> at{record.pos.x, record.pos.y, record.pos.z};
        at.at(axis) += step;
        if (!drawn_at({.x = at[0], .y = at[1], .z = at[2]}, record.level)) {
            open = static_cast<std::uint8_t>(open | (1U << group));
        }
    }
    return open;
}

// One pass of a chunk with sides open (bits of `open`): each group facing the
// camera, the slice toward an open side left out, then those sides' open
// sets. Returns the quads it adds less those it leaves out.
std::int64_t append_open(const ChunkMeshRecord& record, std::size_t pass, std::uint8_t facing,
                         std::uint8_t open, std::uint32_t origin, bool merge,
                         std::vector<VkDrawIndexedIndirectCommand>& out) {
    std::array<Segment, 12> segments{};
    std::size_t used = 0;
    std::int64_t change = 0;
    std::uint32_t at = record.range.first_quad;
    for (std::size_t group = 0; group < 12; ++group) {
        const Segment whole{.first = at, .quads = record.count.at(group)};
        at += whole.quads;
        const std::size_t side = group % 6;
        if (group / 6 != pass || (facing & (1U << side)) == 0) {
            continue;
        }
        Segment segment = whole;
        if ((open & (1U << side)) != 0) {
            const std::uint32_t border = record.border.at(group);
            segment.quads -= border;
            change -= border;
            if (side >= 3) {
                segment.first += border; // a negative face's slice comes first
            }
        }
        segments.at(used++) = segment;
    }
    // The open sets, after the twelve groups, in the same order.
    for (std::size_t set = 0; set < 12; ++set) {
        const std::uint32_t quads = record.open.at(set);
        if (set / 6 == pass && (open & (1U << (set % 6))) != 0) {
            segments.at(used++) = {.first = at, .quads = quads};
            change += quads;
        }
        at += quads;
    }
    append_segments(std::span(segments).first(used), origin, merge, out);
    return change;
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

float faded_in(const ChunkMeshRecord& record, const ChunkFade& fade) noexcept {
    if (!record.appeared || fade.time.count() <= 0.0F) {
        return 1.0F;
    }
    const std::chrono::duration<float> since = fade.now - *record.appeared;
    return std::clamp(since / fade.time, 0.0F, 1.0F);
}

void build_draw_lists(std::span<const ChunkMeshRecord> records, const glm::i64vec3& camera_block,
                      const glm::vec3& camera_fraction, const Frustum& view,
                      const DrawSwitches& switches, DrawLists& lists, const DrawnAt& drawn_at,
                      const ChunkFade& fade) {
    lists.origins.clear();
    lists.opaque.clear();
    lists.cutout.clear();
    lists.stats = {};
    for (const ChunkMeshRecord& record : records) {
        // RFC 0011's swaps hide a level's mesh until the levels around it
        // are ready to change with it.
        if (!record.shown) {
            continue;
        }
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
        // RFC 0011: open toward the sides whose own level's neighbor is not
        // drawn.
        const std::uint8_t open =
            record.level > 0 && drawn_at ? open_sides(record, facing, drawn_at) : 0;
        if (open == 0) {
            append_runs(count.first<6>(), facing, record.range.first_quad, origin, switches.merge,
                        lists.opaque);
            append_runs(count.last<6>(), facing, record.range.first_quad + opaque_quads, origin,
                        switches.merge, lists.cutout);
        } else {
            const std::int64_t change =
                append_open(record, 0, facing, open, origin, switches.merge, lists.opaque) +
                append_open(record, 1, facing, open, origin, switches.merge, lists.cutout);
            lists.stats.quads =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(lists.stats.quads) + change);
        }
        if (lists.opaque.size() + lists.cutout.size() > before) {
            // The fourth component is the level, for the shader's scale,
            // plus half what the chunk has still to fade in.
            lists.origins.emplace_back(static_cast<float>(corner.x), static_cast<float>(corner.y),
                                       static_cast<float>(corner.z),
                                       static_cast<float>(record.level) +
                                           (0.5F * (1.0F - faded_in(record, fade))));
            ++lists.stats.chunks;
        }
    }
}

} // namespace vx::render
