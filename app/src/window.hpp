#pragma once

#include <vx/core/result.hpp>

struct SDL_Window;

namespace vx::app {

// The application's one window, through SDL3. Creating it starts SDL's video
// subsystem, and destroying it shuts SDL down.
class Window {
public:
    [[nodiscard]] static Result<Window> create(const char* title, int width, int height);

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    // Handles every pending event. False once the window should close.
    [[nodiscard]] bool handle_events();

    [[nodiscard]] SDL_Window* handle() const noexcept { return window_; }

private:
    explicit Window(SDL_Window* window) noexcept : window_(window) {}
    void destroy() noexcept;

    SDL_Window* window_ = nullptr;
};

} // namespace vx::app
