#include "keys.hpp"

#include <SDL3/SDL_keycode.h>

namespace vx::app {

KeyAction key_action(SDL_Keycode key) noexcept {
    switch (key) {
    case SDLK_O:
        return KeyAction::overlay;
    case SDLK_V:
        return KeyAction::free_camera;
    default:
        return KeyAction::none;
    }
}

} // namespace vx::app
