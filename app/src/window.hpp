#pragma once

#include <vx/core/result.hpp>
#include <vx/render/context.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <string>

struct SDL_Window;

namespace vx::app {

class Overlay;

// Relative mouse motion, in pixels: x to the right, y down.
struct MouseMotion {
    float x = 0.0F;
    float y = 0.0F;
};

// The application's one window, through SDL3. Creating it starts SDL's video
// subsystem, and destroying it shuts SDL down.
class Window {
public:
    // A window the renderer can draw into with Vulkan. SDL loads a Vulkan
    // library for that itself, and searching on its own it can take MoltenVK's
    // in place of the loader: with the Vulkan SDK's environment set, its first
    // candidate, an app bundle's MoltenVK, resolves through DYLD_LIBRARY_PATH
    // to the SDK's. `vulkan_loader` names the loader file the engine uses, so
    // SDL loads that one; empty lets SDL search.
    [[nodiscard]] static Result<Window> create(const char* title, int width, int height,
                                               const std::string& vulkan_loader);

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    // Handles every pending event. False once the window should close. A
    // click into the window captures the mouse and Escape releases it, so a
    // run that is never clicked, such as a smoke run, never grabs it. F3
    // shows and hides the overlay, which has the events while it shows and
    // the mouse is free; a click it takes captures nothing. F4 is kept for
    // take_noclip_toggle.
    [[nodiscard]] bool handle_events(Overlay& overlay);

    [[nodiscard]] bool mouse_captured() const;
    // The captured mouse's motion since the last call.
    [[nodiscard]] MouseMotion take_mouse_motion() noexcept;
    // Whether F4, which switches between walking and the noclip camera, was
    // pressed an odd number of times since the last call: two presses switch
    // there and back.
    [[nodiscard]] bool take_noclip_toggle() noexcept;

    // How the renderer makes this window's Vulkan surface. Valid while the
    // window lives.
    [[nodiscard]] render::SurfaceSource surface_source() const;

    // The drawable size in pixels, which on a high-density display is larger
    // than the window's size in points; zero while the window is minimized.
    [[nodiscard]] render::Extent framebuffer_size() const;

    [[nodiscard]] SDL_Window* handle() const noexcept { return window_; }

private:
    explicit Window(SDL_Window* window) noexcept : window_(window) {}
    void destroy() noexcept;

    SDL_Window* window_ = nullptr;
    MouseMotion motion_; // gathered while the mouse is captured
    bool noclip_toggle_ = false;
};

} // namespace vx::app
