#include "demo_chunk.hpp"
#include "fly_controls.hpp"
#include "options.hpp"
#include "overlay.hpp"
#include "pack.hpp"
#include "startup_pack.hpp"
#include "walking.hpp"
#include "window.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/core/version.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/render/block_pack.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>
#include <vx/render/instance.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <SDL3/SDL_stdinc.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
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

// RFC 0003's milestone: one chunk of the core pack, with air all around it,
// kept padded so that the overlay's merge toggle can mesh it again, and kept
// as blocks for the player to walk on (RFC 0010).
struct DemoChunk {
    vx::ChunkStorage blocks;                  // all air until draw_demo_chunk fills it
    std::span<const vx::BlockVisual> visuals; // the registry's, which outlives the chunk
    std::unique_ptr<vx::PaddedChunk> padded = std::make_unique<vx::PaddedChunk>();
    vx::MeshScratch scratch;
    vx::app::ChunkMeshing meshing; // as the overlay shows it and switches it
    std::optional<bool> merged;    // how the renderer's mesh was made, once it has one
};

// Meshes the demo chunk as its meshing asks, merged or not, and gives it to
// the renderer, unless the renderer has it meshed that way already.
vx::Result<void> mesh_demo_chunk(DemoChunk& chunk, vx::render::Renderer& renderer) {
    if (chunk.merged == chunk.meshing.merge) {
        return {};
    }
    const vx::ChunkMesh mesh =
        vx::mesh_chunk(*chunk.padded, chunk.visuals, chunk.scratch, {.merge = chunk.meshing.merge});
    if (const vx::Result<void> set = renderer.set_chunk({.x = 0, .y = 0, .z = 0}, mesh);
        !set.has_value()) {
        return set.error();
    }
    chunk.merged = chunk.meshing.merge;
    chunk.meshing.quads = mesh.all().size();
    chunk.meshing.cutout = mesh.pass(vx::Pass::cutout).size();
    return {};
}

// Builds the demo chunk, pads it and draws it. The startup report says its
// quads.
vx::Result<void> draw_demo_chunk(const vx::BlockRegistry& registry, vx::render::Renderer& renderer,
                                 DemoChunk& demo) {
    vx::Result<vx::ChunkStorage> chunk = vx::app::demo_chunk(registry);
    if (!chunk.has_value()) {
        return chunk.error();
    }
    demo.visuals = registry.visuals();
    vx::assemble_padded(vx::ChunkNeighborhood(chunk.value()), *demo.padded);
    demo.blocks = std::move(chunk).value();
    if (const vx::Result<void> meshed = mesh_demo_chunk(demo, renderer); !meshed.has_value()) {
        return meshed.error();
    }
    std::cout << "Chunk: " << demo.meshing.quads << " quads, " << demo.meshing.cutout
              << " of them cutout\n";
    return {};
}

// One frame: the overlay built, the chunk meshed again where the overlay
// switched merging, and the frame rendered.
vx::Result<vx::render::Frame> draw_frame(vx::render::Renderer& renderer, vx::app::Overlay& overlay,
                                         const vx::app::Window& window,
                                         const vx::render::Camera& camera, DemoChunk& demo) {
    // A daytime sky blue, #78A7FF in sRGB.
    constexpr vx::render::Color sky = {.r = 0.188F, .g = 0.386F, .b = 1.0F, .a = 1.0F};
    renderer.resize(window.framebuffer_size());
    ImDrawData* const shown =
        overlay.build(renderer, camera, window.mouse_captured(), demo.meshing);
    if (const vx::Result<void> meshed = mesh_demo_chunk(demo, renderer); !meshed.has_value()) {
        return meshed.error();
    }
    return renderer.render({.sky = sky, .camera = camera, .cubes = {}, .overlay = shown});
}

// The player (RFC 0010): walking on the demo chunk, or flying RFC 0001's
// noclip camera, which F4 switches between.
struct Mover {
    vx::app::Walking walking;
    float field_of_view = 0.0F; // the camera's own, which sprinting widens
    bool noclip = false;
};

// Whether a standing player fits with its feet at `feet`, inside no block.
bool stands_free(const vx::BlockBoxes& blocks, vx::Vec3d feet) {
    const vx::Box body = vx::body_box(feet, vx::Pose::standing);
    std::vector<vx::Box> inside;
    blocks.collect(body, inside);
    return !vx::overlaps_any(body, inside);
}

// One frame of the player. F4 switches modes, landing the player where the
// camera was, unless that is inside a block, where it could not move: there
// F4 keeps flying. Walking, the mouse turns the camera every frame and the
// keys move the player once a tick, the camera following it between ticks.
void move_player(Mover& mover, vx::app::Window& window, vx::render::Camera& camera,
                 const vx::BlockBoxes& blocks, std::chrono::nanoseconds elapsed, float seconds) {
    if (window.take_noclip_toggle()) {
        const vx::Vec3d feet{.x = camera.position.x,
                             .y = camera.position.y - vx::eye_height(vx::Pose::standing),
                             .z = camera.position.z};
        if (!mover.noclip) {
            mover.noclip = true;
        } else if (stands_free(blocks, feet)) {
            mover.noclip = false;
            mover.walking.place(feet);
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
    mover.walking.advance(elapsed, {.held = vx::app::read_walking_keys(window), .yaw = camera.yaw},
                          blocks);
    const vx::Vec3d eye = mover.walking.eye();
    camera.position = {eye.x, eye.y, eye.z};
    camera.fov_y = mover.field_of_view * static_cast<float>(mover.walking.fov_factor());
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

} // namespace

int main(int argc, char* argv[]) {
    // Named for Tracy (RFC 0005's Main, World and Worker threads) and for the
    // OS, so debuggers show it too.
    tracy::SetThreadName("Main");
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
    // A pack comes back only with its registry, which the demo chunk is built
    // from.
    if (!startup || !startup->pack.registry) {
        return 1;
    }
    const vx::render::BlockPack& pack = startup->pack;

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

    // Before the renderer, so it is destroyed after it: the renderer draws
    // with the overlay's ImGui context until then.
    vx::app::Overlay overlay(window.value().handle(), validation, options.value().overlay);
    vx::Result<vx::render::Renderer> renderer =
        vx::render::Renderer::create({.surface = window.value().surface_source(),
                                      .framebuffer = window.value().framebuffer_size(),
                                      .validation = validation,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = overlay.context(),
                                      .block_textures = pack.textures});
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

    DemoChunk demo;
    if (const vx::Result<void> drawn = draw_demo_chunk(*pack.registry, renderer.value(), demo);
        !drawn.has_value()) {
        std::cerr << "error: " << drawn.error().message << '\n';
        return 1;
    }

    // The player stands on the grass in one corner of the chunk, looking
    // across it at the oak (RFC 0010); F4 switches to RFC 0001's noclip
    // camera and back.
    const vx::ChunkBoxes blocks(demo.blocks, {});
    vx::render::Camera camera;
    camera.yaw = 0.785F;   // toward -x and -z, an eighth of a turn left of north
    camera.pitch = -0.15F; // a little down, at the oak
    Mover mover{.walking = vx::app::Walking(vx::app::demo_spawn, 0.0),
                .field_of_view = camera.fov_y,
                .noclip = false};
    std::cout << "Click the window to walk: W A S D, Space to jump, Ctrl with W or W twice to "
                 "sprint, Shift to sneak, Space twice to fly, the mouse to look. F4 switches to a "
                 "camera that flies through everything (W A S D, Space and Shift for up and "
                 "down, Ctrl to go faster) and back. Escape releases the mouse; F3 shows the "
                 "overlay.\n";

    using Clock = std::chrono::steady_clock;
    Clock::time_point last = Clock::now();
    bool last_drawn = false;
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
        // stall, such as dragging the window, does not fling the camera. The
        // walking clock catches up on its own, at most a second.
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last);
        const float seconds = std::min(std::chrono::duration<float>(elapsed).count(), 0.1F);
        last = now;
        move_player(mover, window.value(), camera, blocks, elapsed, seconds);
        overlay.set_movement({.noclip = mover.noclip,
                              .speed = mover.walking.speed(),
                              .vertical_speed = mover.walking.vertical_speed(),
                              .on_ground = mover.walking.player().on_ground,
                              .sprinting = mover.walking.player().sprinting,
                              .crouching = mover.walking.player().pose == vx::Pose::crouching,
                              .flying = mover.walking.player().flying,
                              .ticks = mover.walking.ticks()});

        const vx::Result<vx::render::Frame> rendered =
            draw_frame(renderer.value(), overlay, window.value(), camera, demo);
        if (!rendered.has_value()) {
            std::cerr << "error: " << rendered.error().message << '\n';
            return 1;
        }
        last_drawn = rendered.value() == vx::render::Frame::drawn;
        if (!last_drawn) {
            // Presenting paces the loop; with nothing to present, a 60 Hz wait does.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        FrameMark;
    }

    // With validation on, a run that broke Vulkan's rules fails, so a smoke run
    // with --frames catches it. Messages from shutting down still print.
    const std::uint32_t errors = context.instance().validation_counts().errors;
    if (errors > 0) {
        std::cerr << "error: Vulkan validation counted " << errors
                  << (errors == 1 ? " error\n" : " errors\n");
        return 3;
    }
    return 0;
}
