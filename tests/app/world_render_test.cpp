// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// The whole way the world takes to the screen, as the app runs it (RFC 0005,
// 0006, 0007, 0010): the world loop streams the placeholder's hills in
// around the player, the main thread follows the player with the camera and
// hands the meshes to the renderer's uploads, and the frame shows the ground
// under the player's eye.

#include "streamed_meshes.hpp"
#include "walking.hpp"
#include "world_loop.hpp"

#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/result.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/lod.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using vx::app::ToMain;
using vx::app::ToWorld;
using vx::app::WorldLoop;

constexpr vx::render::Extent image_size = {.width = 256, .height = 144};
constexpr float aspect = 16.0F / 9.0F;
// No face is this cyan: the checker is black and magenta.
constexpr vx::render::Color sky = {.r = 0.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F};

constexpr vx::PlaceholderBlocks blocks{.grass = static_cast<vx::BlockState>(3),
                                       .dirt = static_cast<vx::BlockState>(2),
                                       .stone = static_cast<vx::BlockState>(1)};

std::vector<vx::BlockVisual> visuals() {
    const auto opaque = [](std::uint16_t texture) {
        return vx::BlockVisual{.opacity = vx::Opacity::opaque,
                               .cull_group = 0,
                               .texture = {texture, texture, texture, texture, texture, texture}};
    };
    // Every state on the checker, layer 0: what matters is where faces are.
    return {vx::BlockVisual{}, opaque(0), opaque(0), opaque(0)};
}

// Pinholes: sky pixels whose four neighbors are all something drawn, as a
// crack between two chunks shows.
std::size_t pinholes(const std::vector<std::uint8_t>& pixels) {
    const auto is_sky = [&pixels](std::size_t x, std::size_t y) {
        const std::size_t at = ((y * image_size.width) + x) * 4;
        return pixels[at] == 0 && pixels[at + 1] == 255 && pixels[at + 2] == 255;
    };
    std::size_t count = 0;
    for (std::size_t y = 1; y + 1 < image_size.height; ++y) {
        for (std::size_t x = 1; x + 1 < image_size.width; ++x) {
            if (is_sky(x, y) && !is_sky(x - 1, y) && !is_sky(x + 1, y) && !is_sky(x, y - 1) &&
                !is_sky(x, y + 1)) {
                ++count;
            }
        }
    }
    return count;
}

// The sky's pixels from row `from_row` down.
std::size_t sky_pixels(const std::vector<std::uint8_t>& pixels, std::size_t from_row) {
    std::size_t count = 0;
    for (std::size_t at = from_row * image_size.width * 4; at + 3 < pixels.size(); at += 4) {
        if (pixels[at] == 0 && pixels[at + 1] == 255 && pixels[at + 2] == 255) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("the frame shows the ground under the player's eye, streamed in after it started") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({}); // no workers: the jobs run when the loop settles
    vx::MeshResultPool pool;
    const std::int32_t ground = generator.height(24, 24);
    vx::app::WorldLoopSettings settings{
        .region = {.view_distance = 4, .near_distance = 2, .near_height = 2, .hysteresis = 2}};
    // On the grass beside the spire, as the app starts.
    settings.player = vx::app::WorldLoopSettings::PlayerStart{
        .spawn = {.x = 24.5, .y = static_cast<double>(ground), .z = 24.5}, .lowest = -64.0};
    WorldLoop loop(generator, table, jobs, pool, settings);
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = image_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = nullptr,
                                      .block_textures = {},
                                      .chunk_arena_quads = std::uint32_t{1} << 20U,
                                      .stage_chunk_uploads = false,
                                      .draw_call_limit = 0});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    // Each frame as the app's: the world thread's messages taken, the camera
    // at the player's eye, looking down at the ground ahead, and the world
    // told where it is, the frame drawn with what its budget uploads, and
    // the uploads reported back.
    vx::app::StreamedMeshes meshes;
    vx::app::WalkingFrame player;
    vx::render::Camera camera;
    camera.yaw = 0.785F;
    camera.pitch = -0.7F;
    std::vector<ToMain> messages;
    std::uint64_t reported = 0;
    const auto frame = [&] {
        loop.take(messages);
        meshes.receive(messages);
        for (const ToMain& message : messages) {
            if (message.kind == ToMain::Kind::player) {
                player = message.player;
            }
        }
        const vx::Vec3d eye = player.eye_at(1.0);
        camera.position = {eye.x, eye.y, eye.z};
        ToWorld viewer;
        viewer.kind = ToWorld::Kind::camera;
        viewer.camera = vx::app::camera_viewer(camera, aspect);
        loop.to_world().push(std::move(viewer));
        REQUIRE(renderer
                    .render({.sky = sky,
                             .camera = camera,
                             .cubes = {},
                             .overlay = nullptr,
                             .chunk_changes = &meshes})
                    .has_value());
        if (meshes.done() != reported) {
            reported = meshes.done();
            ToWorld done;
            done.kind = ToWorld::Kind::uploaded;
            done.uploaded = reported;
            loop.to_world().push(std::move(done));
        }
    };

    // The player starts before the world around it has loaded, as on the
    // app's world thread, whose ticks begin with the workers': two seconds
    // of ticks with no job run.
    std::chrono::steady_clock::time_point now{};
    for (int tick = 0; tick <= 40; ++tick) {
        now += WorldLoop::tick_length;
        loop.step(now);
        frame();
    }
    // Then the world streams in, and the frames upload it.
    for (int round = 0; round < 1'000; ++round) {
        loop.settle();
        now += WorldLoop::tick_length;
        loop.step(now);
        frame();
        if (meshes.waiting() == 0 && loop.unuploaded() == 0 && loop.meshes_in_flight() == 0) {
            break;
        }
    }
    REQUIRE(meshes.waiting() == 0);
    // It waited where it started, on the grass.
    CHECK(player.on_ground);
    CHECK(player.position.y == doctest::Approx(ground));

    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());
    MESSAGE("eye at y ", camera.position.y, ", ", renderer.chunks().uploads().records().size(),
            " meshes held, ", renderer.chunks().draw_lists().stats.chunks, " drawn");
    // Looking down from a standing eye, the lower half of the frame is
    // ground.
    CHECK(sky_pixels(pixels.value(), image_size.height / 2) == 0);
}

TEST_CASE("with RFC 0011's horizon on, the arena draws exactly the chunks the world loop picks, "
          "on several levels, none with a chunk around it") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    vx::app::WorldLoopSettings settings{
        .region = {.view_distance = 16, .near_distance = 2, .near_height = 2, .hysteresis = 2}};
    // The rings for this frame's 144 rows: level 1 from 113 blocks out.
    settings.lod = vx::LodRings{};
    settings.lod->framebuffer_height = image_size.height;
    settings.unuploaded_limit = 1'000'000;
    WorldLoop loop(generator, table, jobs, pool, settings);
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = image_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = nullptr,
                                      .block_textures = {},
                                      .chunk_arena_quads = std::uint32_t{1} << 22U,
                                      .stage_chunk_uploads = false,
                                      .draw_call_limit = 0});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    renderer.chunks().uploads().use_swaps();
    renderer.chunks().uploads().set_budget(std::chrono::hours(1));

    // Over the hills, looking out across them.
    vx::app::StreamedMeshes meshes;
    vx::render::Camera camera;
    camera.position = {24.5, 140.0, 24.5};
    camera.yaw = 0.785F;
    camera.pitch = -0.25F;
    std::vector<ToMain> messages;
    std::uint64_t reported = 0;
    const auto frame = [&] {
        loop.take(messages);
        meshes.receive(messages);
        ToWorld viewer;
        viewer.kind = ToWorld::Kind::camera;
        viewer.camera = vx::app::camera_viewer(camera, aspect);
        loop.to_world().push(std::move(viewer));
        REQUIRE(renderer
                    .render({.sky = sky,
                             .camera = camera,
                             .cubes = {},
                             .overlay = nullptr,
                             .chunk_changes = &meshes})
                    .has_value());
        if (meshes.done() != reported) {
            reported = meshes.done();
            ToWorld done;
            done.kind = ToWorld::Kind::uploaded;
            done.uploaded = reported;
            loop.to_world().push(std::move(done));
        }
    };
    std::chrono::steady_clock::time_point now{};
    for (int round = 0; round < 1'000; ++round) {
        loop.settle();
        now += WorldLoop::tick_length;
        loop.step(now);
        frame();
        if (round > 2 && meshes.waiting() == 0 && loop.unuploaded() == 0 &&
            loop.meshes_in_flight() == 0) {
            break;
        }
    }
    REQUIRE(meshes.waiting() == 0);

    using Chunk = std::tuple<int, std::int32_t, std::int32_t, std::int32_t>;
    std::set<Chunk> picked;
    for (const vx::LodPos chunk : loop.drawn()) {
        picked.insert({chunk.level, chunk.x, chunk.y, chunk.z});
    }
    std::set<Chunk> shown;
    std::set<int> levels;
    for (const vx::render::ChunkMeshRecord& record : renderer.chunks().uploads().records()) {
        if (record.shown) {
            shown.insert({record.level, record.pos.x, record.pos.y, record.pos.z});
            levels.insert(record.level);
        }
    }
    CHECK(shown == picked);
    CHECK(levels.size() >= 3);
    // The meshes of every level stream in, most of them hidden.
    CHECK(renderer.chunks().uploads().records().size() > shown.size());
    std::size_t overlaps = 0;
    for (const vx::LodPos chunk : loop.drawn()) {
        for (vx::LodPos up = chunk; up.level < vx::max_lod_level;) {
            up = vx::parent(up);
            overlaps += shown.contains({up.level, up.x, up.y, up.z}) ? 1 : 0;
        }
    }
    CHECK(overlaps == 0);
    const std::uint32_t drawn = renderer.chunks().draw_lists().stats.chunks;
    std::size_t opened = 0; // levels' sides drawn open: their neighbor is drawn finer
    for (const vx::render::ChunkMeshRecord& record : renderer.chunks().uploads().records()) {
        if (record.shown && record.level > 0) {
            for (const std::uint16_t count : record.open) {
                opened += count;
            }
        }
    }
    CHECK(opened > 0);
    CHECK(drawn > 0);
    CHECK(drawn <= shown.size());

    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());
    // No crack where two levels meet: the coarse chunk's open sides close
    // them (3 pinholes here without).
    CHECK(pinholes(pixels.value()) == 0);
    MESSAGE(renderer.chunks().uploads().records().size(), " meshes held, ", shown.size(),
            " shown on ", levels.size(), " levels, ", drawn,
            " drawn; sky in the lower half: ", sky_pixels(pixels.value(), image_size.height / 2),
            " of ", image_size.width * image_size.height / 2, "; pinholes ",
            pinholes(pixels.value()));
}
