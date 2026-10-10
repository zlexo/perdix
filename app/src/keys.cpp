#include "keys.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_video.h>

namespace vx::app {

KeyAction key_action(SDL_Keycode key) noexcept {
    switch (key) {
    case SDLK_O:
        return KeyAction::overlay;
    case SDLK_V:
        return KeyAction::free_camera;
    case SDLK_G:
        return KeyAction::god_panel;
    default:
        return KeyAction::none;
    }
}

void control(Controls& controls, const SDL_Event& event, SDL_WindowID window, bool taken,
             bool typing) noexcept {
    const auto close_panel = [&] {
        controls.panel = false;
        controls.give_back = false;
    };
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !typing) {
        switch (key_action(event.key.key)) {
        case KeyAction::overlay:
            controls.overlay = !controls.overlay;
            break;
        case KeyAction::free_camera:
            controls.free_camera = !controls.free_camera;
            break;
        case KeyAction::god_panel:
            if (controls.panel) {
                controls.captured = controls.captured || controls.give_back;
                close_panel();
            } else {
                controls.panel = true;
                controls.give_back = controls.captured;
                controls.captured = false;
            }
            break;
        case KeyAction::none:
            break;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.windowID == window && !taken) {
        controls.captured = true;
        close_panel();
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        controls.captured = false;
        close_panel();
    }
}

} // namespace vx::app
