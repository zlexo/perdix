#include "window.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <utility>

namespace vx::app {

Result<Window> Window::create(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return Error{std::string("SDL could not start its video subsystem: ") + SDL_GetError()};
    }
    SDL_Window* window = SDL_CreateWindow(title, width, height,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        Error error{std::string("SDL could not create the window: ") + SDL_GetError()};
        SDL_Quit();
        return error;
    }
    return Window(window);
}

Window::Window(Window&& other) noexcept : window_(std::exchange(other.window_, nullptr)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        destroy();
        window_ = std::exchange(other.window_, nullptr);
    }
    return *this;
}

Window::~Window() {
    destroy();
}

bool Window::handle_events() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return false; // the whole application, such as Cmd+Q
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(window_)) {
            return false;
        }
    }
    return true;
}

void Window::destroy() noexcept {
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
    }
}

} // namespace vx::app
