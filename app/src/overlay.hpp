#pragma once

#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

struct ImDrawData;
struct ImGuiContext;
struct SDL_Window;
union SDL_Event;

namespace vx::app {

// RFC 0001's debug overlay, built with Dear ImGui over the window: frame
// time graphs, the GPU and driver, the present mode switch, the camera's
// position and the validation count. F3 shows and hides it. It owns the ImGui
// context, which the renderer it is given to draws with, so it must outlive
// that renderer.
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
    void toggle() noexcept { shown_ = !shown_; }

    // Passes `event` to ImGui while the overlay shows. True when the overlay
    // takes it from the app: a click on the overlay's window.
    bool take_event(const SDL_Event& event);

    // Records a frame for the graphs, shown or not: `interval`, the time
    // since the frame before, and what the renderer measured of it. The
    // CPU's time is the interval less the renderer's waits.
    void record(std::chrono::nanoseconds interval, const render::FrameTimes& times);

    // Builds this frame's overlay: its draw lists, or null while it is
    // hidden. A captured mouse flies the camera, and the overlay ignores it.
    // Switching the present mode asks `renderer` for it.
    [[nodiscard]] ImDrawData* build(render::Renderer& renderer, const render::Camera& camera,
                                    bool mouse_captured);

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
    static void draw_camera(const render::Camera& camera);
    void draw_validation(const render::Context& context) const;

    ImGuiContext* context_;
    bool validation_;
    bool shown_;
    History frame_;
    History cpu_;
    History gpu_;
    // The startup report, which the overlay shows too (RFC 0001), formatted
    // once and again only when the swapchain changes.
    std::string device_report_;
    std::string swapchain_report_;
    render::SwapchainChoice reported_choice_;
};

} // namespace vx::app
