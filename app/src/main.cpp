#include "commit.hpp"
#include "fly_controls.hpp"
#include "flythrough.hpp"
#include "flythrough_run.hpp"
#include "footprint.hpp"
#include "options.hpp"
#include "overlay.hpp"
#include "pack.hpp"
#include "startup_figures.hpp"
#include "startup_pack.hpp"
#include "streamed_meshes.hpp"
#include "view.hpp"
#include "walking.hpp"
#include "window.hpp"
#include "world_loop.hpp"
#include "world_thread.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/result.hpp>
#include <vx/core/thread.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/core/version.hpp>
#include <vx/render/block_pack.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>
#include <vx/render/instance.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/movement.hpp>
#include <vx/world/placeholder_generator.hpp>

#include <SDL3/SDL_stdinc.h>
#include <glm/vec3.hpp>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// RFC 0001: validation in debug builds.
#ifdef NDEBUG
constexpr bool validation = false;
#else
constexpr bool validation = true;
#endif

// The home folder and, on Windows, APPDATA, read through SDL, whose getenv
// MSVC does not mark deprecated. Not SDL_GetUserFolder: on macOS it creates
// every folder of the path it returns.
vx::app::Environment read_environment() {
    vx::app::Environment environment;
#ifdef _WIN32
    const char* const home = SDL_getenv("USERPROFILE");
    if (const char* const appdata = SDL_getenv("APPDATA")) {
        environment.appdata = appdata;
    }
#else
    const char* const home = SDL_getenv("HOME");
#endif
    if (home != nullptr) {
        environment.home = home;
    }
    return environment;
}

// The placeholder's ground in the core pack's states (RFC 0006 over RFC
// 0004): grass over dirt over stone.
vx::Result<vx::PlaceholderBlocks> placeholder_blocks(const vx::BlockRegistry& registry) {
    const std::optional<vx::BlockState> grass = registry.find("core:grass");
    const std::optional<vx::BlockState> dirt = registry.find("core:dirt");
    const std::optional<vx::BlockState> stone = registry.find("core:stone");
    if (!grass || !dirt || !stone) {
        return vx::Error{"the block pack has no core:grass, core:dirt or core:stone, which the "
                         "placeholder world is made of"};
    }
    return vx::PlaceholderBlocks{.grass = *grass, .dirt = *dirt, .stone = *stone};
}

// RFC 0007's arena capacity for a view distance: the meshes RFC 0006 draws
// there, about 9,400 at 32 chunks and growing with the square, each as
// large as rough terrain's (14.4 KiB, 1,843 quads), plus a quarter.
std::uint32_t arena_quads(std::int32_t view_distance) {
    const double scale = static_cast<double>(view_distance) / 32.0;
    const double quads = 9'400.0 * scale * scale * 1'843.0 * 1.25;
    return static_cast<std::uint32_t>(std::clamp(
        quads, 1'048'576.0, static_cast<double>(vx::render::ChunkRenderer::max_arena_quads)));
}

// RFC 0005's workers: as many as the options say, or the cores less two,
// one for the main thread and one for the world thread, and at least one.
std::uint32_t worker_count(const vx::app::Options& options) {
    const std::uint32_t cores = std::thread::hardware_concurrency();
    return options.workers.value_or(cores > 3 ? cores - 2 : 1);
}

// "6 workers, switching QoS", for the startup line and the flythrough's
// report.
std::string describe_workers(std::uint32_t count, vx::WorkerQos qos) {
    std::string_view mode = "switching";
    if (qos == vx::WorkerQos::utility) {
        mode = "utility";
    } else if (qos == vx::WorkerQos::user_initiated) {
        mode = "user-initiated";
    }
    return std::to_string(count) + (count == 1 ? " worker, " : " workers, ") + std::string(mode) +
           " QoS";
}

void send(vx::app::WorldLoop& loop, const vx::app::ToWorld& message) {
    loop.to_world().push(vx::app::ToWorld(message));
}

// The player (RFC 0010): walking on the world thread's ticks, which send
// their last two ticks for the camera to draw between, or flying RFC 0001's
// noclip camera, which V switches between.
struct Mover {
    vx::app::WalkingFrame frame;
    std::chrono::steady_clock::time_point tick_at; // when the frame's tick fell due
    float field_of_view = 0.0F;                    // the camera's own, which sprinting widens
    bool noclip = false;
    bool landing = false; // the world thread was asked to land the player
    bool bobbing = true;  // the game's View Bobbing option, the overlay's switch
};

// How far `now` is from the frame's tick to the next: 0 to 1.
double between_ticks(const Mover& mover, std::chrono::steady_clock::time_point now) {
    return std::clamp(std::chrono::duration<double>(now - mover.tick_at) /
                          std::chrono::duration<double>(vx::app::WorldLoop::tick_length),
                      0.0, 1.0);
}

// One frame of the player. V switches modes: flying leaves the player
// standing where it was; walking again asks the world thread to land it
// where the camera is, which it does unless that is inside a block or a
// chunk still loading, and then V keeps flying. Walking, the mouse turns
// the camera every frame, the keys go to the world thread's next ticks, and
// the camera follows the player between them.
void move_player(Mover& mover, vx::app::Window& window, vx::render::Camera& camera,
                 vx::app::WorldLoop& loop, float seconds,
                 std::chrono::steady_clock::time_point now) {
    if (window.take_noclip_toggle()) {
        if (!mover.noclip) {
            mover.noclip = true;
            vx::app::ToWorld still;
            still.kind = vx::app::ToWorld::Kind::input;
            send(loop, still);
        } else if (!mover.landing) {
            vx::app::ToWorld land;
            land.kind = vx::app::ToWorld::Kind::land;
            land.feet = {.x = camera.position.x,
                         .y = camera.position.y - vx::eye_height(vx::Pose::standing),
                         .z = camera.position.z};
            send(loop, land);
            mover.landing = true;
        }
    }
    const vx::render::FlyInput fly_input = vx::app::read_fly_controls(window);
    if (mover.noclip) {
        vx::render::fly(camera, fly_input, seconds);
        camera.fov_y = mover.field_of_view;
        return;
    }
    vx::render::fly(camera,
                    {.move = glm::vec3(0.0F),
                     .yaw_turn = fly_input.yaw_turn,
                     .pitch_turn = fly_input.pitch_turn,
                     .fast = false},
                    seconds);
    vx::app::ToWorld keys;
    keys.kind = vx::app::ToWorld::Kind::input;
    keys.input = {.held = vx::app::read_walking_keys(window), .yaw = camera.yaw};
    send(loop, keys);
    const double between = between_ticks(mover, now);
    const vx::Vec3d eye = mover.frame.eye_at(between);
    camera.position = {eye.x, eye.y, eye.z};
    camera.fov_y = mover.field_of_view * static_cast<float>(mover.frame.fov_factor_at(between));
}

// The world thread's messages: the player's frames and landings for the
// mover, the meshes and unloads for the uploads, and streaming's reports for
// the overlay.
void take_from_world(vx::app::WorldLoop& loop, vx::app::StreamedMeshes& meshes, Mover& mover,
                     vx::app::Overlay& overlay, std::vector<vx::app::ToMain>& messages) {
    loop.take(messages);
    meshes.receive(messages);
    for (const vx::app::ToMain& message : messages) {
        if (message.kind == vx::app::ToMain::Kind::player) {
            mover.frame = message.player;
            mover.tick_at = message.tick_at;
        } else if (message.kind == vx::app::ToMain::Kind::landed) {
            mover.landing = false;
            mover.noclip = !message.landed;
        } else if (message.kind == vx::app::ToMain::Kind::report) {
            overlay.set_world_report(message.report);
        }
    }
    messages.clear();
}

// Where the camera is and looks, for what streams in first (RFC 0006).
void send_viewer(vx::app::WorldLoop& loop, const vx::render::Camera& camera, float aspect) {
    vx::app::ToWorld message;
    message.kind = vx::app::ToWorld::Kind::camera;
    message.camera = vx::app::camera_viewer(camera, aspect);
    send(loop, message);
}

// The camera drawn: the player's, swaying with the view's bob while it
// walks. The bob never touches the camera the mouse turns.
vx::render::Camera shown_camera(const Mover& mover, const vx::render::Camera& camera,
                                std::chrono::steady_clock::time_point now) {
    if (mover.noclip) {
        return camera;
    }
    return vx::app::bobbed(camera, mover.frame.bob_at(between_ticks(mover, now), mover.bobbing));
}

// A framebuffer's width over its height, 1 while it has none.
float aspect_of(vx::render::Extent size) {
    return size.height == 0 ? 1.0F
                            : static_cast<float>(size.width) / static_cast<float>(size.height);
}

// What the world thread was last told: how far the uploads are, for its
// backpressure; whether to merge, which the overlay switches and which
// meshes everything again; the render distance, which the overlay's slider
// sets (RFC 0006); and the mesh arena's state, for the memory budget, a few
// times a second and at once when it fills or empties, with a request for
// streaming's report.
struct WorldReports {
    static constexpr std::chrono::milliseconds report_interval{250};

    std::uint64_t uploaded = 0;
    bool merge = vx::app::ChunkMeshing{}.merge;
    std::int32_t view_distance = 0;
    std::uint64_t arena_full_frames = 0;
    bool arena_was_full = false;
    std::optional<std::chrono::steady_clock::time_point> reported;

    void send_changes(vx::app::WorldLoop& loop, const vx::app::StreamedMeshes& meshes,
                      const vx::app::ChunkMeshing& meshing, const vx::app::RenderDistance& distance,
                      const vx::render::ChunkRenderer& chunks,
                      std::chrono::steady_clock::time_point now) {
        const bool due = !reported || now - *reported >= report_interval;
        // At once when the arena fills, otherwise a few times a second.
        const bool filled = chunks.upkeep().full && !arena_was_full;
        if (due || filled) {
            arena_full_frames = chunks.upkeep().full_frames;
            arena_was_full = chunks.upkeep().full;
            vx::app::ToWorld arena;
            arena.kind = vx::app::ToWorld::Kind::arena;
            arena.arena_full_frames = arena_full_frames;
            arena.arena_full = arena_was_full;
            arena.arena_share = static_cast<double>(chunks.arena().space().live()) /
                                static_cast<double>(std::max(chunks.arena_limit(), 1U));
            send(loop, arena);
        }
        if (due) {
            reported = now;
            vx::app::ToWorld ask;
            ask.kind = vx::app::ToWorld::Kind::report;
            send(loop, ask);
        }
        if (meshes.done() != uploaded) {
            uploaded = meshes.done();
            vx::app::ToWorld done;
            done.kind = vx::app::ToWorld::Kind::uploaded;
            done.uploaded = uploaded;
            send(loop, done);
        }
        if (meshing.merge != merge) {
            merge = meshing.merge;
            vx::app::ToWorld options;
            options.kind = vx::app::ToWorld::Kind::meshing;
            options.meshing = {.merge = merge};
            send(loop, options);
        }
        if (distance.chunks != view_distance) {
            view_distance = distance.chunks;
            vx::app::ToWorld region;
            region.kind = vx::app::ToWorld::Kind::view_distance;
            region.view_distance = view_distance;
            send(loop, region);
        }
    }
};

// One frame: the overlay built and the frame rendered, the world thread's
// meshes uploaded within the frame's budget. The overlay switches merging
// and `bobbing`, and sets the render distance.
vx::Result<vx::render::Frame>
draw_frame(vx::render::Renderer& renderer, vx::app::Overlay& overlay, const vx::app::Window& window,
           const vx::render::Camera& camera, vx::app::StreamedMeshes& meshes,
           vx::app::ChunkMeshing& meshing, bool& bobbing, vx::app::RenderDistance& distance) {
    // A daytime sky blue, #78A7FF in sRGB.
    constexpr vx::render::Color sky = {.r = 0.188F, .g = 0.386F, .b = 1.0F, .a = 1.0F};
    renderer.resize(window.framebuffer_size());
    overlay.set_chunk_backlog(meshes.waiting());
    ImDrawData* const shown =
        overlay.build(renderer, camera, window.mouse_captured(), meshing, bobbing, distance);
    return renderer.render(
        {.sky = sky, .camera = camera, .cubes = {}, .overlay = shown, .chunk_changes = &meshes});
}

// With validation on, a run that broke Vulkan's rules fails, so a smoke run
// with --frames, or a flythrough, catches it.
int validation_result(const vx::render::Context& context) {
    const std::uint32_t errors = context.instance().validation_counts().errors;
    if (errors > 0) {
        std::cerr << "error: Vulkan validation counted " << errors
                  << (errors == 1 ? " error\n" : " errors\n");
        return 3;
    }
    return 0;
}

// The flythrough's plan, when one was asked for.
std::optional<vx::app::FlythroughPlan> flythrough_plan(vx::app::Flythrough which) {
    switch (which) {
    case vx::app::Flythrough::none:
        break;
    case vx::app::Flythrough::full:
        return vx::app::FlythroughPlan::full();
    case vx::app::Flythrough::quick:
        return vx::app::FlythroughPlan::quick();
    case vx::app::Flythrough::endurance:
        return vx::app::FlythroughPlan::endurance();
    }
    return std::nullopt;
}

// The window drawing the plan's size, at whatever density.
void size_for_flythrough(vx::app::Window& window, const vx::app::FlythroughPlan& plan) {
    if (!window.set_framebuffer_size({.width = plan.width, .height = plan.height})) {
        std::cerr << "warning: the flythrough wants " << plan.width << "x" << plan.height
                  << ", and the window draws " << window.framebuffer_size().width << "x"
                  << window.framebuffer_size().height << '\n';
    }
}

// The world thread's sample, with the main thread's own numbers: the holes
// of the meshes sent that wait here, against the viewer the request was
// sent with, the arena's quads, the process's memory, and whether the
// uploads caught up.
vx::app::FlythroughSample flythrough_sample(const vx::app::WorldSample& world,
                                            const vx::render::Renderer& renderer,
                                            const vx::app::StreamedMeshes& meshes,
                                            const vx::Viewer& asked) {
    const bool complete = world.complete && meshes.waiting() == 0;
    if (world.kind == vx::app::SampleKind::completion) {
        return {.id = world.id,
                .full = false,
                .at = world.at,
                .generated = world.generated,
                .uniform = world.uniform,
                .meshed = world.meshed,
                .complete = complete};
    }
    const vx::render::ArenaStats arena = renderer.chunks().arena().space().stats();
    const vx::app::Holes waiting =
        meshes.holes(renderer.chunks().uploads(), asked, vx::app::near_holes_distance);
    return {.id = world.id,
            .full = true,
            .at = world.at,
            .generated = world.generated,
            .uniform = world.uniform,
            .meshed = world.meshed,
            .longest_step = world.longest_step,
            .longest_pass = world.longest_pass,
            .holes = world.holes + waiting.all,
            .near_holes = world.near_holes + waiting.near,
            .chunk_bytes = world.chunk_bytes,
            .mesh_bytes =
                (std::uint64_t{arena.live} + arena.retired) * vx::render::ChunkArena::quad_bytes,
            .footprint_bytes = vx::app::process_footprint(),
            .complete = complete,
            .broken = world.broken};
}

// What the world thread is asked for: a full sample checks RFC 0006's
// invariants when the plan says so.
vx::app::SampleKind sample_kind(const vx::app::FlythroughRun::SampleRequest& request,
                                const vx::app::FlythroughPlan& plan) {
    if (!request.full) {
        return vx::app::SampleKind::completion;
    }
    return plan.check ? vx::app::SampleKind::checked : vx::app::SampleKind::full;
}

// The flythrough's camera and samples. A phase's first request goes before
// the frame's camera, so its answer closes the phase before where it ended;
// the others after it, so they see the frame's. Each answer is read once
// the meshes sent before it reach the uploads and before those after it, so
// the holes the main thread adds are the meshes the world counted as sent
// that do not show yet, against the camera it answered from.
struct FlythroughSamples {
    std::vector<vx::app::ToMain> messages;
    std::optional<vx::Viewer> sent; // the last camera sent
    vx::Viewer asked;               // the camera the request out is answered from

    void send_camera(vx::app::WorldLoop& loop, vx::app::FlythroughRun& run,
                     const vx::app::StreamedMeshes& meshes, const vx::Viewer& viewer) {
        const std::optional<vx::app::FlythroughRun::SampleRequest> request =
            run.sample_request(meshes.waiting() > 0);
        if (request && request->before_camera) {
            ask(loop, run.plan(), *request, sent.value_or(viewer));
        }
        vx::app::ToWorld camera;
        camera.kind = vx::app::ToWorld::Kind::camera;
        camera.camera = viewer;
        send(loop, camera);
        sent = viewer;
        if (request && !request->before_camera) {
            ask(loop, run.plan(), *request, viewer);
        }
    }

    void ask(vx::app::WorldLoop& loop, const vx::app::FlythroughPlan& plan,
             const vx::app::FlythroughRun::SampleRequest& request, const vx::Viewer& viewer) {
        vx::app::ToWorld message;
        message.kind = vx::app::ToWorld::Kind::sample;
        message.sample_id = request.id;
        message.sample_kind = sample_kind(request, plan);
        send(loop, message);
        asked = viewer;
    }

    void take(vx::app::WorldLoop& loop, vx::app::StreamedMeshes& meshes,
              vx::app::FlythroughRun& run, const vx::render::Renderer& renderer,
              vx::app::Overlay& overlay) {
        loop.take(messages);
        meshes.receive(messages, [&](const vx::app::WorldSample& world) {
            run.add(flythrough_sample(world, renderer, meshes, asked));
        });
        for (const vx::app::ToMain& message : messages) {
            if (message.kind == vx::app::ToMain::Kind::report) {
                overlay.set_world_report(message.report);
            }
        }
        messages.clear();
    }
};

// What the flythrough's report opens with.
vx::app::FlythroughHeader flythrough_header(const vx::render::Renderer& renderer,
                                            const vx::render::Extent& size,
                                            std::int32_t view_distance,
                                            const std::string& workers) {
    const vx::render::DeviceInfo& device = renderer.context().device_info();
    const vx::render::Swapchain* swapchain = renderer.swapchain();
    const bool immediate =
        swapchain != nullptr && swapchain->choice().present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR;
#ifdef NDEBUG
    constexpr std::string_view build = "release";
#else
    constexpr std::string_view build = "debug";
#endif
#ifdef TRACY_ENABLE
    constexpr std::string_view profiling = ", profiling";
#else
    constexpr std::string_view profiling;
#endif
    return {.engine = std::string(vx::version_string()) + " (" + std::string(vx::app::commit) + ")",
            .build = std::string(build) + std::string(profiling),
            .device = device.name,
            .driver = device.driver_name + " " + device.driver_info,
            .settings = std::to_string(size.width) + "x" + std::to_string(size.height) +
                        ", view distance " + std::to_string(view_distance) + ", " + workers + ", " +
                        (immediate ? "immediate" : "FIFO") + " present"};
}

// A checked flythrough's result: what it found wrong, each said, then the
// validation's (RFC 0006's CI run).
int checked(std::span<const vx::app::PhaseResult> results, const vx::render::Context& context) {
    const std::vector<std::string> problems = vx::app::flythrough_problems(results);
    for (const std::string& problem : problems) {
        std::cerr << "error: " << problem << '\n';
    }
    return problems.empty() ? validation_result(context) : 4;
}

// RFC 0006's benchmark flythrough: the path flies the camera, the world
// streams in around it, and the report goes to the standard output once the
// last phase is over. Closing the window stops it with no report.
int fly(const vx::app::FlythroughPlan& plan, vx::app::Window& window,
        vx::render::Renderer& renderer, vx::app::Overlay& overlay, vx::app::WorldLoop& loop,
        vx::app::StreamedMeshes& meshes, const std::string& workers) {
    using Clock = std::chrono::steady_clock;
    vx::app::FlythroughRun run(plan, Clock::now());
    vx::render::Camera camera;
    vx::app::ChunkMeshing meshing;
    bool bobbing = false;
    // Fixed: the slider shows the plan's distance and takes nothing.
    vx::app::RenderDistance distance{.chunks = plan.view_distance, .fixed = true};
    WorldReports reports;
    reports.view_distance = plan.view_distance;
    FlythroughSamples samples;
    std::optional<Clock::time_point> last;
    while (window.handle_events(overlay)) {
        const Clock::time_point now = Clock::now();
        if (last) {
            run.frame_time(now - *last);
        }
        last = now;
        const std::optional<vx::app::FlythroughPose> pose = run.frame(now);
        if (!pose) {
            std::cout << '\n'
                      << vx::app::flythrough_report(flythrough_header(renderer,
                                                                      window.framebuffer_size(),
                                                                      plan.view_distance, workers),
                                                    run.results());
            return plan.check ? checked(run.results(), renderer.context())
                              : validation_result(renderer.context());
        }
        camera.position = {pose->eye.x, pose->eye.y, pose->eye.z};
        camera.yaw = pose->yaw;
        camera.pitch = pose->pitch;
        samples.send_camera(loop, run, meshes,
                            vx::app::camera_viewer(camera, aspect_of(window.framebuffer_size())));
        samples.take(loop, meshes, run, renderer, overlay);
        const vx::Result<vx::render::Frame> rendered =
            draw_frame(renderer, overlay, window, camera, meshes, meshing, bobbing, distance);
        if (!rendered.has_value()) {
            std::cerr << "error: " << rendered.error().message << '\n';
            return 1;
        }
        reports.send_changes(loop, meshes, meshing, distance, renderer.chunks(), now);
        FrameMark;
    }
    std::cerr << "error: the window closed before the flythrough's end\n";
    return 1;
}

// The block pack, found and loaded with the player's Minecraft where it can
// be: a bad block file, texture or texture map, or a Minecraft named on the
// command line that cannot be used, stops startup with every problem listed
// by file, line and column, and nothing comes back.
std::optional<vx::app::StartupPack> load_pack(const vx::app::Options& options) {
    const vx::Result<std::filesystem::path> root = vx::app::find_pack(options.pack);
    if (!root.has_value()) {
        std::cerr << "error: " << root.error().message << '\n';
        return std::nullopt;
    }
    vx::app::StartupPack startup = vx::app::load_startup_pack(
        root.value(), vx::app::minecraft_choice(options, read_environment()));
    const vx::render::BlockPack& pack = startup.pack;
    for (vx::Diagnostic diagnostic : pack.diagnostics) {
        // Named under the root, so a terminal or an editor finds the file
        // from whatever folder the program was started in.
        if (const std::filesystem::path file(diagnostic.file); file.is_relative()) {
            diagnostic.file = (root.value() / file).generic_string();
        }
        std::cerr << vx::format(diagnostic) << '\n';
    }
    // A Minecraft named on the command line that cannot be used, after every
    // other problem.
    if (startup.named_game) {
        std::cerr << "error: " << startup.named_game->message << '\n';
    }
    // Any error leaves the pack without a registry or without textures.
    const bool loaded = !vx::has_errors(pack.diagnostics) && pack.registry;
    if (!loaded) {
        std::cerr << "error: the block pack in " << root.value().string() << " did not load\n";
    }
    if (!loaded || startup.named_game) {
        return std::nullopt;
    }
    std::cout << "Block pack: " << root.value().string() << ", " << pack.registry->state_count()
              << " states\n"
              << startup.textures << '\n';
    return startup;
}

// The skins and the font players and their name tags are drawn with (RFC
// 0012): the pack's own skins, and the game's skins and font from its jar,
// each skin chosen on the command line, with what could not be honored and
// the startup report's lines printed.
vx::app::StartupFigures load_figures(const vx::app::StartupPack& startup,
                                     const vx::app::Options& options) {
    vx::app::StartupFigures figures = vx::app::load_startup_figures(
        startup.root, startup.game, vx::app::parse_skin_choice(options.skin.value_or("zlex")),
        vx::app::parse_skin_choice(options.perdix_skin.value_or("perdix")));
    for (const vx::Diagnostic& diagnostic : figures.diagnostics) {
        std::cerr << vx::format(diagnostic) << '\n';
    }
    std::cout << figures.skins_line << '\n' << figures.font_line << '\n';
    return figures;
}

} // namespace

int main(int argc, char* argv[]) {
    // Named for Tracy (RFC 0005's Main, World and Worker threads) and for the
    // OS, so debuggers show it too.
    vx::set_thread_name("Main");
    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    const std::span<char* const> after_name = args.empty() ? args : args.subspan(1);
    const std::vector<std::string_view> arguments(after_name.begin(), after_name.end());
    const vx::Result<vx::app::Options> options = vx::app::parse_options(arguments);
    if (!options.has_value()) {
        std::cerr << "error: " << options.error().message << '\n';
        return 2;
    }

    std::cout << "Perdix " << vx::version_string() << '\n';

    // The block pack (RFC 0004), before anything opens, drawn with the
    // player's Minecraft where it can be (RFC 0009).
    const std::optional<vx::app::StartupPack> startup = load_pack(options.value());
    // A pack comes back only with its registry, which the world is built
    // from.
    if (!startup || !startup->pack.registry) {
        return 1;
    }
    const vx::render::BlockPack& pack = startup->pack;

    const vx::app::StartupFigures figures = load_figures(*startup, options.value());

    // SDL loads a Vulkan library for the window's surface, and it must be the
    // engine's loader: a driver's library does not understand the loader's
    // instances (see Window::create).
    const vx::Result<std::string> loader = vx::render::vulkan_loader_path();
    if (!loader.has_value()) {
        std::cerr << "error: " << loader.error().message << '\n';
        return 1;
    }
    vx::Result<vx::app::Window> window =
        vx::app::Window::create("Perdix", 1280, 720, loader.value());
    if (!window.has_value()) {
        std::cerr << "error: " << window.error().message << '\n';
        return 1;
    }
    const std::optional<vx::app::FlythroughPlan> plan = flythrough_plan(options.value().flythrough);
    if (plan) {
        size_for_flythrough(window.value(), *plan);
    }

    // The world (RFC 0006): the placeholder's hills, streamed in around the
    // camera at RFC 0006's view distance by the world thread and its
    // workers (RFC 0005).
    const vx::BlockRegistry& registry = *pack.registry;
    const vx::Result<vx::PlaceholderBlocks> ground = placeholder_blocks(registry);
    if (!ground.has_value()) {
        std::cerr << "error: " << ground.error().message << '\n';
        return 1;
    }
    constexpr std::uint64_t world_seed = 0x5EED;
    const vx::PlaceholderGenerator generator(world_seed, ground.value());
    vx::app::WorldLoopSettings settings{.region = {}};
    // The player starts beside the spire at the world's origin, on the grass;
    // a flythrough flies the camera alone.
    const vx::Vec3d spawn{.x = 24.5, .y = static_cast<double>(generator.height(24, 24)), .z = 24.5};
    if (plan) {
        settings.region.view_distance = plan->view_distance;
    } else {
        settings.player = vx::app::WorldLoopSettings::PlayerStart{.spawn = spawn, .lowest = -64.0};
    }
    settings.physics = registry.physics();

    // Before the renderer, so it is destroyed after it: the renderer draws
    // with the overlay's ImGui context until then.
    vx::app::Overlay overlay(window.value().handle(), validation, options.value().overlay);
    vx::Result<vx::render::Renderer> renderer = vx::render::Renderer::create(
        {.surface = window.value().surface_source(),
         .framebuffer = window.value().framebuffer_size(),
         .validation = validation,
         // A flythrough measures frames without waiting on the display.
         .present_mode = plan ? vx::render::PresentMode::immediate : vx::render::PresentMode::fifo,
         .overlay = overlay.context(),
         .block_textures = pack.textures,
         .skins = figures.skins,
         .font = figures.font.has_value() ? &figures.font.value() : nullptr,
         .chunk_arena_quads = arena_quads(settings.region.view_distance),
         .stage_chunk_uploads = false,
         .draw_call_limit = 0});
    if (!renderer.has_value()) {
        std::cerr << "error: " << renderer.error().message << '\n';
        return 1;
    }
    // Counted on the GPU, so the report shows what the renderer holds.
    std::cout << "Block texture array: " << renderer.value().block_texture_layers() << " layers\n";
    const vx::render::Context& context = renderer.value().context();
    std::cout << vx::render::format_device_report(context.device_info(),
                                                  context.portability_fallback());
    if (const vx::render::Swapchain* swapchain = renderer.value().swapchain()) {
        std::cout << vx::render::format_swapchain_report(swapchain->choice());
    }

    // The world thread runs from here; the main thread hands its meshes to
    // the renderer and the camera to it. Destroyed in reverse: the meshes
    // still waiting go back to the pool before the thread stops and joins,
    // then the loop waits for its jobs.
    vx::JobSystem jobs(
        {.workers = worker_count(options.value()), .qos = options.value().worker_qos});
    vx::MeshResultPool pool;
    vx::app::WorldLoop loop(generator, registry.visuals(), jobs, pool, settings);
    const vx::app::WorldThread world(loop);
    vx::app::StreamedMeshes meshes;
    const std::string workers = describe_workers(jobs.worker_count(), options.value().worker_qos);
    std::cout << "World: placeholder hills, seed 0x5EED, view distance "
              << settings.region.view_distance << " chunks, " << workers << "\n";
    if (plan) {
        return fly(*plan, window.value(), renderer.value(), overlay, loop, meshes, workers);
    }

    // The player stands on the grass beside the spire, looking at it (RFC
    // 0010); V switches to RFC 0001's noclip camera and back.
    vx::render::Camera camera;
    camera.position = {spawn.x, spawn.y + vx::eye_height(vx::Pose::standing), spawn.z};
    camera.yaw = 0.785F;  // toward -x and -z, an eighth of a turn left of north
    camera.pitch = 0.25F; // a little up, at the spire
    Mover mover{.frame = {},
                .tick_at = {},
                .field_of_view = camera.fov_y,
                .noclip = false,
                .landing = false,
                .bobbing = true};
    mover.frame.previous = spawn;
    mover.frame.position = spawn;
    std::cout << "Click the window to walk: W A S D, Space to jump, Ctrl with W or W twice to "
                 "sprint, Shift to sneak, Space twice to fly, the mouse to look. V switches to a "
                 "camera that flies through everything (W A S D, Space and Shift for up and "
                 "down, Ctrl to go faster) and back. Escape releases the mouse; O shows the "
                 "overlay.\n";

    using Clock = std::chrono::steady_clock;
    Clock::time_point last = Clock::now();
    bool last_drawn = false;
    vx::app::ChunkMeshing meshing;
    vx::app::RenderDistance distance{.chunks = settings.region.view_distance, .fixed = false};
    WorldReports reports;
    reports.view_distance = settings.region.view_distance;
    std::vector<vx::app::ToMain> messages;
    const std::uint64_t frames = options.value().frames;
    for (std::uint64_t frame = 0; frames == 0 || frame < frames; ++frame) {
        if (!window.value().handle_events(overlay)) {
            break;
        }
        const Clock::time_point now = Clock::now();
        // The last frame's times, unless it was skipped, and its interval
        // holds the wait for the next.
        if (last_drawn) {
            overlay.record(now - last, renderer.value().frame_times());
        }
        // The time since the last frame, at most a tenth of a second, so a
        // stall, such as dragging the window, does not fling the camera.
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last);
        const float seconds = std::min(std::chrono::duration<float>(elapsed).count(), 0.1F);
        last = now;
        take_from_world(loop, meshes, mover, overlay, messages);
        move_player(mover, window.value(), camera, loop, seconds, now);
        send_viewer(loop, camera, aspect_of(window.value().framebuffer_size()));
        overlay.set_movement({.noclip = mover.noclip,
                              .speed = mover.frame.speed(),
                              .vertical_speed = mover.frame.vertical_speed(),
                              .on_ground = mover.frame.on_ground,
                              .sprinting = mover.frame.sprinting,
                              .crouching = mover.frame.crouching,
                              .flying = mover.frame.flying,
                              .ticks = mover.frame.ticks});

        const vx::render::Camera shown = shown_camera(mover, camera, now);
        const vx::Result<vx::render::Frame> rendered =
            draw_frame(renderer.value(), overlay, window.value(), shown, meshes, meshing,
                       mover.bobbing, distance);
        if (!rendered.has_value()) {
            std::cerr << "error: " << rendered.error().message << '\n';
            return 1;
        }
        reports.send_changes(loop, meshes, meshing, distance, renderer.value().chunks(), now);
        last_drawn = rendered.value() == vx::render::Frame::drawn;
        if (!last_drawn) {
            // Presenting paces the loop; with nothing to present, a 60 Hz wait does.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        FrameMark;
    }

    return validation_result(context);
}
