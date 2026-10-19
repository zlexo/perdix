#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/draw_lists.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <volk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

// RFC 0007's draw lists without a GPU: runs of face groups, face-direction
// masks, frustum culling, origins and the split of indirect calls.

namespace {

using vx::render::ChunkMeshRecord;
using vx::render::DrawLists;
using vx::render::DrawSwitches;

// The quads each command draws, as arena quad indices.
std::vector<std::uint32_t> drawn_quads(std::span<const VkDrawIndexedIndirectCommand> commands) {
    std::vector<std::uint32_t> quads;
    for (const VkDrawIndexedIndirectCommand& command : commands) {
        REQUIRE(command.indexCount % 6 == 0);
        REQUIRE(command.vertexOffset % 4 == 0);
        const auto first = static_cast<std::uint32_t>(command.vertexOffset / 4);
        for (std::uint32_t at = 0; at < command.indexCount / 6; ++at) {
            quads.push_back(first + at);
        }
    }
    return quads;
}

// Maximal runs of drawn groups, with empty groups between them joining them.
std::size_t runs_by_hand(std::span<const std::uint16_t, 6> counts, std::uint8_t drawn) {
    std::size_t runs = 0;
    bool open = false;
    for (std::size_t group = 0; group < 6; ++group) {
        if (counts[group] == 0) {
            continue;
        }
        const bool draw = (drawn & (1U << group)) != 0;
        if (draw && !open) {
            ++runs;
        }
        open = draw;
    }
    return runs;
}

} // namespace

TEST_CASE("runs draw exactly the visible groups' quads, once each, for every mask and every "
          "pattern of empty groups") {
    std::mt19937 random(0xD4A7);
    std::uniform_int_distribution<int> size(1, 300);
    for (unsigned empty = 0; empty < 64; ++empty) {
        for (unsigned mask = 0; mask < 64; ++mask) {
            std::array<std::uint16_t, 6> counts{};
            for (std::size_t group = 0; group < 6; ++group) {
                counts[group] =
                    (empty & (1U << group)) != 0 ? 0 : static_cast<std::uint16_t>(size(random));
            }
            constexpr std::uint32_t first = 1'000;
            std::vector<std::uint32_t> expected;
            std::uint32_t at = first;
            for (std::size_t group = 0; group < 6; ++group) {
                for (std::uint32_t quad = 0; quad < counts[group]; ++quad) {
                    if ((mask & (1U << group)) != 0) {
                        expected.push_back(at + quad);
                    }
                }
                at += counts[group];
            }
            INFO("mask ", mask, ", empty groups ", empty);
            for (const bool merge : {true, false}) {
                INFO("merging ", merge);
                std::vector<VkDrawIndexedIndirectCommand> commands;
                vx::render::append_runs(counts, static_cast<std::uint8_t>(mask), first, 7, merge,
                                        commands);
                std::vector<std::uint32_t> quads = drawn_quads(commands);
                std::ranges::sort(quads);
                CHECK(quads == expected);
                for (const VkDrawIndexedIndirectCommand& command : commands) {
                    CHECK(command.instanceCount == 1);
                    CHECK(command.firstIndex == 0);
                    CHECK(command.firstInstance == 7);
                    CHECK(command.indexCount > 0);
                }
                // Merged, one command a run; not, one a drawn group.
                const auto drawn_groups = static_cast<std::size_t>(
                    std::ranges::count_if(std::array{0, 1, 2, 3, 4, 5}, [&](int group) {
                        return counts[static_cast<std::size_t>(group)] > 0 &&
                               (mask & (1U << static_cast<unsigned>(group))) != 0;
                    }));
                CHECK(
                    commands.size() ==
                    (merge ? runs_by_hand(counts, static_cast<std::uint8_t>(mask)) : drawn_groups));
            }
        }
    }
}

TEST_CASE("every quad that can face the camera is in a group the face mask keeps") {
    std::mt19937 random(0xFACE);
    std::uniform_int_distribution<std::int64_t> chunk(-40, 40);
    std::uniform_int_distribution<std::int32_t> local(0, 31);
    std::uniform_int_distribution<int> face(0, 5);
    std::uniform_real_distribution<float> fraction(0.0F, 1.0F);
    std::size_t facing = 0;
    for (int sample = 0; sample < 200'000; ++sample) {
        const glm::i64vec3 corner(chunk(random), chunk(random), chunk(random));
        const glm::vec3 camera(fraction(random), fraction(random), fraction(random));
        // Arena group g: axis g % 3, positive for g < 3.
        const int group = face(random);
        const auto axis = static_cast<glm::length_t>(group % 3);
        const bool positive = group < 3;
        const std::int64_t block = corner[axis] + local(random);
        // A +x face lies on the block's high side and faces the camera when
        // the camera is beyond it; a -x face on its low side, the other way.
        const bool faces = positive
                               ? static_cast<double>(camera[axis]) > static_cast<double>(block + 1)
                               : static_cast<double>(camera[axis]) < static_cast<double>(block);
        if (!faces) {
            continue;
        }
        ++facing;
        INFO("corner ", corner.x, ", ", corner.y, ", ", corner.z, ", group ", group);
        CHECK((vx::render::facing_mask(corner) & (1U << static_cast<unsigned>(group))) != 0);
    }
    CHECK(facing > 10'000);
    // Far away on +x, only -x faces can face the camera; a chunk around the
    // camera can show all six.
    CHECK(vx::render::facing_mask({64, -5, -5}) == 0b111'110);
    CHECK(vx::render::facing_mask({0, 0, 0}) == 0b111'111);
    CHECK(vx::render::facing_mask({-32, -32, -32}) == 0b000'111);
}

namespace {

// A random view: an orthonormal frame, a field of view and an aspect.
vx::Frustum random_view(std::mt19937& random, vx::Vec3d& forward, vx::Vec3d& right, vx::Vec3d& up,
                        double& tan_y, double& aspect) {
    std::uniform_real_distribution<double> angle(-3.14159, 3.14159);
    const double yaw = angle(random);
    const double pitch = angle(random) / 2.2;
    forward = {.x = std::cos(pitch) * std::sin(yaw),
               .y = std::sin(pitch),
               .z = -std::cos(pitch) * std::cos(yaw)};
    right = {.x = std::cos(yaw), .y = 0.0, .z = std::sin(yaw)};
    up = {.x = (right.y * forward.z) - (right.z * forward.y),
          .y = (right.z * forward.x) - (right.x * forward.z),
          .z = (right.x * forward.y) - (right.y * forward.x)};
    std::uniform_real_distribution<double> fov(0.3, 1.5);
    tan_y = std::tan(fov(random) / 2.0);
    aspect = std::uniform_real_distribution<double>(0.5, 2.5)(random);
    return {forward, right, up, tan_y, aspect, 0.05};
}

double dot(vx::Vec3d a, vx::Vec3d b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

} // namespace

TEST_CASE("a chunk with any point inside the view is never culled") {
    std::mt19937 random(0xC011);
    std::uniform_int_distribution<std::int32_t> chunk(-8, 8);
    std::uniform_real_distribution<double> inside(0.0, 32.0);
    std::uniform_real_distribution<float> fraction(0.0F, 1.0F);
    ChunkMeshRecord record{};
    record.count.fill(1);
    std::size_t seen = 0;
    for (int sample = 0; sample < 3'000; ++sample) {
        vx::Vec3d forward;
        vx::Vec3d right;
        vx::Vec3d up;
        double tan_y = 0.0;
        double aspect = 0.0;
        const vx::Frustum view = random_view(random, forward, right, up, tan_y, aspect);
        record.pos = {.x = chunk(random), .y = chunk(random), .z = chunk(random)};
        const glm::i64vec3 camera_block(5, -3, 7);
        const glm::vec3 camera(fraction(random), fraction(random), fraction(random));
        // Points of the chunk relative to the camera; inside when ahead of
        // the near plane and within both fields of view.
        bool any = false;
        for (int point = 0; point < 32 && !any; ++point) {
            const vx::Vec3d p{.x = static_cast<double>((record.pos.x * 32) - camera_block.x) +
                                   inside(random) - static_cast<double>(camera.x),
                              .y = static_cast<double>((record.pos.y * 32) - camera_block.y) +
                                   inside(random) - static_cast<double>(camera.y),
                              .z = static_cast<double>((record.pos.z * 32) - camera_block.z) +
                                   inside(random) - static_cast<double>(camera.z)};
            const double ahead = dot(p, forward);
            any = ahead >= 0.05 && std::abs(dot(p, right)) <= tan_y * aspect * ahead &&
                  std::abs(dot(p, up)) <= tan_y * ahead;
        }
        if (!any) {
            continue;
        }
        ++seen;
        DrawLists lists;
        vx::render::build_draw_lists(std::span(&record, 1), camera_block, camera, view,
                                     {.frustum = true, .faces = false, .merge = true}, lists);
        INFO("chunk ", record.pos.x, ", ", record.pos.y, ", ", record.pos.z);
        CHECK(lists.stats.culled == 0);
        CHECK(lists.stats.chunks == 1);
    }
    CHECK(seen > 300);
}

TEST_CASE("a limit of 3 commands a call splits them and loses none") {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;
    vx::render::for_each_call(
        10, 3, [&](std::uint32_t first, std::uint32_t count) { calls.emplace_back(first, count); });
    CHECK(calls ==
          std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0, 3}, {3, 3}, {6, 3}, {9, 1}});
    calls.clear();
    vx::render::for_each_call(10, 65'535, [&](std::uint32_t first, std::uint32_t count) {
        calls.emplace_back(first, count);
    });
    CHECK(calls == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0, 10}});
    calls.clear();
    vx::render::for_each_call(
        0, 3, [&](std::uint32_t first, std::uint32_t count) { calls.emplace_back(first, count); });
    CHECK(calls.empty());
}

TEST_CASE("draw lists: each drawn chunk's origin, its commands in both passes, and the counts") {
    // Two chunks: one around the camera, one far east of it.
    std::array<ChunkMeshRecord, 2> records{};
    records[0].pos = {.x = 0, .y = 0, .z = 0};
    records[0].range.first_quad = 100;
    records[0].count = {1, 2, 3, 4, 5, 6, 10, 0, 0, 0, 0, 20};
    records[1].pos = {.x = 3, .y = 0, .z = 0};
    records[1].range.first_quad = 500;
    records[1].count = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    const glm::i64vec3 camera_block(10, 10, 10);
    const glm::vec3 camera(0.5F, 0.5F, 0.5F);
    DrawLists lists;
    // Everything in view: the default frustum holds all.
    vx::render::build_draw_lists(records, camera_block, camera, {}, {}, lists);
    REQUIRE(lists.origins.size() == 2);
    CHECK(lists.origins[0] == glm::vec4(-10.0F, -10.0F, -10.0F, 0.0F));
    CHECK(lists.origins[1] == glm::vec4(86.0F, -10.0F, -10.0F, 0.0F));
    // Around the camera every group faces it: one run of all 21 opaque
    // quads, and one of the cutout +x and -z groups across the empty ones.
    REQUIRE(lists.opaque.size() >= 1);
    REQUIRE(lists.cutout.size() == 1);
    CHECK(lists.cutout[0].vertexOffset == 4 * 121);
    CHECK(lists.cutout[0].indexCount == 6 * 30);
    CHECK(lists.opaque[0].vertexOffset == 4 * 100);
    CHECK(lists.opaque[0].indexCount == 6 * 21);
    CHECK(lists.opaque[0].firstInstance == 0);
    // East of the camera: only its -x faces (arena group 3) and +y, +z,
    // -y, -z, since the camera is inside its y and z span.
    const std::vector<std::uint32_t> east = drawn_quads(std::span(lists.opaque).subspan(1));
    CHECK(east == std::vector<std::uint32_t>{501, 502, 503, 504, 505});
    CHECK(lists.stats.quads_facing_away == 1);
    CHECK(lists.stats.chunks == 2);
    // Looking west, the chunk east is behind the camera: culled, with no
    // origin. The camera's own chunk holds the camera, so it stays.
    const vx::Frustum looking_west({.x = -1.0, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 0.0, .z = -1.0},
                                   {.x = 0.0, .y = 1.0, .z = 0.0}, 0.7, 1.6, 0.05);
    vx::render::build_draw_lists(records, camera_block, camera, looking_west, {}, lists);
    CHECK(lists.stats.culled == 1);
    CHECK(lists.origins.size() == 1);
    CHECK(lists.stats.chunks == 1);
    // Switches off: every group of every chunk, one command a group.
    vx::render::build_draw_lists(records, camera_block, camera, looking_west,
                                 {.frustum = false, .faces = false, .merge = false}, lists);
    CHECK(lists.origins.size() == 2);
    CHECK(lists.opaque.size() == 12);
    CHECK(lists.cutout.size() == 2);
    CHECK(lists.stats.quads == 21 + 30 + 6);
}

TEST_CASE(
    "a chunk of RFC 0011's level L spans 32 x 2^L blocks: its origin, its box and its faces") {
    // A level-2 chunk, x from -128 to 0, and the level-0 chunk at its far
    // end, x from -128 to -96; the camera at x = -60, inside the coarse
    // chunk's span on every axis, looking east.
    std::array<ChunkMeshRecord, 2> records{};
    records[0].pos = {.x = -1, .y = 0, .z = 0};
    records[0].level = 2;
    records[0].range.first_quad = 100;
    records[0].count = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    records[1].pos = {.x = -4, .y = 0, .z = 0};
    records[1].range.first_quad = 200;
    records[1].count = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    const glm::i64vec3 camera_block(-60, 10, 10);
    const glm::vec3 camera(0.5F, 0.5F, 0.5F);
    DrawLists lists;
    vx::render::build_draw_lists(records, camera_block, camera, {}, {}, lists);
    REQUIRE(lists.origins.size() == 2);
    // The level rides in the origin's fourth component, for the shader.
    CHECK(lists.origins[0] == glm::vec4(-68.0F, -10.0F, -10.0F, 2.0F));
    CHECK(lists.origins[1] == glm::vec4(-68.0F, -10.0F, -10.0F, 0.0F));
    // Its -x faces can face the camera 68 blocks into it; a level-0
    // chunk's that far behind cannot.
    CHECK(vx::render::facing_mask({-68, -10, -10}, 128) == 0b111'111);
    CHECK(vx::render::facing_mask({-68, -10, -10}) == 0b110'111);
    CHECK(drawn_quads(lists.opaque) ==
          std::vector<std::uint32_t>{100, 101, 102, 103, 104, 105, 200, 201, 202, 204, 205});
    // Looking east, the level-0 chunk lies behind the camera and is culled;
    // the coarse one reaches 60 blocks ahead of it and stays.
    const vx::Frustum looking_east({.x = 1.0, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 0.0, .z = 1.0},
                                   {.x = 0.0, .y = 1.0, .z = 0.0}, 0.7, 1.6, 0.05);
    vx::render::build_draw_lists(records, camera_block, camera, looking_east, {}, lists);
    CHECK(lists.stats.culled == 1);
    REQUIRE(lists.origins.size() == 1);
    CHECK(lists.origins[0].w == 2.0F);
}

TEST_CASE("a mesh RFC 0011's swaps hide is not drawn, nor counted, nor culled") {
    std::array<ChunkMeshRecord, 2> records{};
    records[0].pos = {.x = 0, .y = 0, .z = 0};
    records[0].range.first_quad = 100;
    records[0].count = {1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0};
    records[1] = records[0];
    records[1].range.first_quad = 300;
    records[1].level = 1;
    records[1].shown = false;
    DrawLists lists;
    vx::render::build_draw_lists(records, glm::i64vec3(10, 10, 10), glm::vec3(0.5F), {}, {}, lists);
    CHECK(lists.origins.size() == 1);
    CHECK(lists.stats.chunks == 1);
    CHECK(lists.stats.culled == 0);
    CHECK(lists.stats.quads == 21);
    for (const auto& command : lists.opaque) {
        CHECK(command.vertexOffset < 4U * 300U);
    }
}
