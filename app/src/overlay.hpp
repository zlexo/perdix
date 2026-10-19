#pragma once

#include "world_report.hpp"

#include <vx/render/camera.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain_choice.hpp>
#include <vx/world/life.hpp>
#include <vx/world/lod.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ImDrawData;
struct ImGuiContext;
struct SDL_Window;
union SDL_Event;

namespace vx::app {

// How chunks are meshed, as the overlay switches it (RFC 0003): whether
// faces that look the same merge.
struct ChunkMeshing {
    bool merge = true;
};

// The game's Render Distance option (RFC 0006): the terrain disc's radius,
// 4 to 64 chunks of 32 blocks. Its slider changes `chunks` once let go, and
// the caller hands it to the world thread. A flythrough fixes it: the
// slider shows it, and takes nothing.
struct RenderDistance {
    static constexpr std::int32_t least = 4;
    static constexpr std::int32_t most = 64;
    // RFC 0011: with the horizon on, 16,384 blocks.
    static constexpr std::int32_t horizon_most = 512;
    std::int32_t chunks = 32;
    bool fixed = false;
    std::int32_t limit = most; // the slider's top
};

// The player's movement, as the overlay shows it (RFC 0010).
struct MovementReport {
    bool noclip = false;         // RFC 0001's camera, or walking
    double speed = 0.0;          // horizontally, in blocks a second
    double vertical_speed = 0.0; // up, in blocks a second
    bool on_ground = false;
    bool sprinting = false;
    bool crouching = false;
    bool flying = false;     // in creative flight
    std::uint64_t ticks = 0; // movement ticks run so far
};

// The god panel (RFC 0012), which G opens: Perdix's mode, bringing him to
// you, and the skin each player wears. The caller fills it in, the panel's
// build changes it as it is clicked, and the caller acts on the changes: it
// tells the world thread a new mode, and brings Perdix when `bring_perdix`
// is set, then clears it. A skin chosen shows at once.
struct GodPanel {
    bool perdix = false; // whether the world has him
    LifeMode perdix_mode = LifeMode::life;
    double perdix_distance = 0.0; // from the camera, in blocks
    bool bring_perdix = false;    // set when Bring him here is clicked
    // Every skin's name, by its index. The {} lets designated initializers
    // leave it out on Linux Clang too.
    std::vector<std::string> skins{}; // NOLINT(readability-redundant-member-init)
    std::uint32_t perdix_skin = 0;    // past the last skin: the missing texture
    std::uint32_t your_skin = 0;
};

// RFC 0001's debug overlay, built with Dear ImGui over the window: frame
// time graphs, the GPU and driver, the present mode switch, RFC 0004's
// texture filter switch, RFC 0003's meshing toggles, RFC 0007's chunk
// rendering counters and switches, the camera's position, the player's
// movement (RFC 0010) and the validation count. O shows and hides it. The
// god panel shares its ImGui context and its frame. It owns the context,
// which the renderer it is given to draws with, so it must outlive that
// renderer.
class Overlay {
public:
    // An overlay for `window`, shown from the start or not. `validation` says
    // whether the renderer runs with validation, whose count it shows.
    Overlay(SDL_Window* window, bool validation, bool shown);
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    Overlay(Overlay&&) = delete;
    Overlay& operator=(Overlay&&) = delete;
    ~Overlay();

    [[nodiscard]] ImGuiContext* context() const noexcept { return context_; }
    [[nodiscard]] bool shown() const noexcept { return shown_; }
    // Whether the overlay or the panel is taking text, as into a slider's
    // value: the app's keys then type, and do nothing else.
    [[nodiscard]] bool wants_text() const;
    void toggle() noexcept { shown_ = !shown_; }
    void set_shown(bool shown) noexcept { shown_ = shown; }

    // The god panel, open or not, and what it shows and sets.
    [[nodiscard]] bool panel_open() const noexcept { return panel_open_; }
    void set_panel_open(bool open) noexcept { panel_open_ = open; }
    [[nodiscard]] GodPanel& god_panel() noexcept { return god_; }

    // Passes `event` to ImGui while the overlay or the panel shows and the
    // mouse is free. True when they take it from the app: a click on one of
    // their windows. A mouse button's release reaches ImGui always, hidden
    // or captured: a press it saw that never ends stays held there for good.
    bool take_event(const SDL_Event& event, bool mouse_captured);

    // Records a frame for the graphs, shown or not: `interval`, the time
    // since the frame before, and what the renderer measured of it. The
    // CPU's time is the interval less the renderer's waits.
    void record(std::chrono::nanoseconds interval, const render::FrameTimes& times);

    // Builds this frame's overlay and panel: their draw lists, or null
    // while neither shows. A captured mouse flies the camera, and the overlay ignores it.
    // Switching the present mode asks `renderer` for it. Switching merging
    // changes `meshing.merge`, and the caller meshes the chunks again. The
    // View Bobbing switch changes `bobbing`, and the Render Distance slider
    // `distance.chunks`.
    [[nodiscard]] ImDrawData* build(render::Renderer& renderer, const render::Camera& camera,
                                    bool mouse_captured, ChunkMeshing& meshing, bool& bobbing,
                                    RenderDistance& distance);

    // What the next build shows of the player's movement.
    void set_movement(const MovementReport& movement) noexcept { movement_ = movement; }
    // The meshes and unloads waiting on the main thread for the next frames'
    // uploads (RFC 0007's backlog).
    void set_chunk_backlog(std::size_t waiting) noexcept { chunk_backlog_ = waiting; }
    // Streaming's last report from the world thread (RFC 0006).
    void set_world_report(const WorldReport& report) noexcept {
        world_ = report;
        reported_ = true;
    }
    // The view distance the world holds, as streaming last reported it, in
    // chunks: none before its first report.
    [[nodiscard]] std::optional<std::int32_t> held_view_distance() const noexcept;
    // RFC 0013's fog: on, unless its switch turns it off, as 26.2 can turn
    // its own off for debugging.
    [[nodiscard]] bool fog() const noexcept { return fog_; }
    // RFC 0013's fade, as 26.2's Chunk Fade option: how long a chunk takes
    // to fade in from the fog color once it appears, 0 to 2 seconds.
    [[nodiscard]] float chunk_fade() const noexcept { return chunk_fade_; }

private:
    // A measure's last samples, in milliseconds, for its graph.
    struct History {
        static constexpr std::size_t length = 240; // two seconds at 120 frames a second
        std::array<float, length> samples{};
        std::size_t next = 0;  // where the next sample goes: the oldest, once full
        std::size_t count = 0; // samples so far, up to length

        void add(float milliseconds) noexcept;
        [[nodiscard]] float average() const noexcept;
        [[nodiscard]] float peak() const noexcept;
        // The sample nine in ten are at or below.
        [[nodiscard]] float ninetieth_percentile() const noexcept;
    };

    // A measure's average over its samples, then their graph, the oldest on
    // the left, with RFC 0001's budget as a line across it.
    static void graph(const char* name, const History& history);

    // Formats the startup report: the device's lines once, the swapchain's
    // again whenever it changes.
    void update_reports(const render::Renderer& renderer);
    // The parts of the overlay's window, top to bottom.
    void draw_frame_times() const;
    static void draw_present_mode(render::Renderer& renderer);
    static void draw_block_filter(render::Renderer& renderer);
    void draw_antialiasing(render::Renderer& renderer);
    static void draw_chunk(render::Renderer& renderer, ChunkMeshing& meshing);
    void draw_chunk_rendering(render::Renderer& renderer);
    static void draw_camera(const render::Camera& camera);
    static void draw_movement(const MovementReport& movement);
    void draw_render_distance(RenderDistance& distance);
    void draw_streaming() const;

    void draw_validation(const render::Context& context) const;
    // The overlay's window, and the panel's.
    void draw_overlay(render::Renderer& renderer, const render::Camera& camera,
                      ChunkMeshing& meshing, bool& bobbing, RenderDistance& distance);
    void draw_god_panel();

    ImGuiContext* context_;
    MovementReport movement_;
    std::size_t chunk_backlog_ = 0;
    WorldReport world_;
    bool reported_ = false;
    bool fog_ = true;
    float chunk_fade_ = 0.75F;
    std::string antialiasing_error_; // the MSAA switch's last failure
    // The Render Distance slider's value while it is dragged, 0 otherwise:
    // the distance changes once it is let go.
    std::int32_t dragged_distance_ = 0;
    bool validation_;
    bool shown_;
    bool panel_open_ = false;
    GodPanel god_;
    History frame_;
    History cpu_;
    History gpu_;
    // The startup report, which the overlay shows too (RFC 0001), formatted
    // once and again only when the swapchain changes.
    std::string device_report_;
    std::string swapchain_report_;
    render::SwapchainChoice reported_choice_;
    // The arena's statistics walk its free ranges, so they are sampled a few
    // times a second, not every frame (RFC 0007).
    render::ArenaStats arena_;
    std::chrono::steady_clock::time_point arena_sampled_;
    // RFC 0011's levels, sampled with the arena: meshes held, and shown.
    struct LevelCount {
        std::uint32_t held = 0;
        std::uint32_t shown = 0;
    };
    std::array<LevelCount, lod_levels> levels_{};
};

} // namespace vx::app
