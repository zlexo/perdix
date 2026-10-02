#include "window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace vx::app {

Result<Window> Window::create(const char* title, int width, int height,
                              const std::string& vulkan_loader) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return Error{std::string("SDL could not start its video subsystem: ") + SDL_GetError()};
    }
    if (!vulkan_loader.empty()) {
        SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, vulkan_loader.c_str());
    }
    SDL_Window* window =
        SDL_CreateWindow(title, width, height,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        Error error{std::string("SDL could not create the window: ") + SDL_GetError()};
        SDL_Quit();
        return error;
    }
    return Window(window);
}

Window::Window(Window&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)), motion_(std::exchange(other.motion_, {})) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        destroy();
        window_ = std::exchange(other.window_, nullptr);
        motion_ = std::exchange(other.motion_, {});
    }
    return *this;
}

Window::~Window() {
    destroy();
}

bool Window::handle_events() {
    const SDL_WindowID id = SDL_GetWindowID(window_);
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return false; // the whole application, such as Cmd+Q
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == id) {
            return false;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.windowID == id) {
            SDL_SetWindowRelativeMouseMode(window_, true);
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            SDL_SetWindowRelativeMouseMode(window_, false);
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.windowID == id &&
            mouse_captured()) {
            motion_.x += event.motion.xrel;
            motion_.y += event.motion.yrel;
        }
    }
    return true;
}

bool Window::mouse_captured() const {
    return SDL_GetWindowRelativeMouseMode(window_);
}

MouseMotion Window::take_mouse_motion() noexcept {
    return std::exchange(motion_, {});
}

render::SurfaceSource Window::surface_source() const {
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    SDL_Window* window = window_;
    return {
        .instance_extensions = std::span(extensions, count),
        .create = [window](VkInstance instance) -> Result<VkSurfaceKHR> {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
                return Error{std::string("SDL could not create a Vulkan surface: ") +
                             SDL_GetError()};
            }
            return surface;
        },
    };
}

render::Extent Window::framebuffer_size() const {
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0) {
        return {}; // nothing to draw into: rendering pauses (RFC 0001)
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return {.width = static_cast<std::uint32_t>(std::max(width, 0)),
            .height = static_cast<std::uint32_t>(std::max(height, 0))};
}

void Window::destroy() noexcept {
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
    }
}

} // namespace vx::app
