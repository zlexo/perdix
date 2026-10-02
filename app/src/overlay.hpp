#pragma once

#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <string>

struct ImDrawData;
struct ImGuiContext;
struct SDL_Window;
union SDL_Event;

namespace vx::app {

// RFC 0001's debug overlay, built with Dear ImGui over the window: what the
// renderer measures and reports, and the switches it offers. F3 shows and
// hides it. It owns the ImGui context, which the renderer it is given to
// draws with, so it must outlive that renderer.
class Overlay {
public:
    // An overlay for `window`, shown from the start or not.
    Overlay(SDL_Window* window, bool shown);
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

    // Builds this frame's overlay: its draw lists, or null while it is
    // hidden. A captured mouse flies the camera, and the overlay ignores it.
    [[nodiscard]] ImDrawData* build(const render::Renderer& renderer, bool mouse_captured);

private:
    ImGuiContext* context_;
    bool shown_;
    // The startup report, which the overlay shows too (RFC 0001), formatted
    // once and again only when the swapchain changes.
    std::string device_report_;
    std::string swapchain_report_;
    render::SwapchainChoice reported_choice_;
};

} // namespace vx::app
