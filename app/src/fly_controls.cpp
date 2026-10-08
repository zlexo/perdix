#include "fly_controls.hpp"

#include <SDL3/SDL.h>

namespace vx::app {
namespace {

// About a quarter turn for 600 pixels of mouse motion.
constexpr float radians_per_pixel = 0.0025F;

} // namespace

render::FlyInput read_fly_controls(Window& window) {
    const MouseMotion motion = window.take_mouse_motion();
    if (!window.mouse_captured()) {
        return {};
    }
    // Scancodes are places on the keyboard, not letters, so the keys sit
    // under the same fingers on any layout.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const auto axis = [keys](SDL_Scancode positive, SDL_Scancode negative) {
        return (keys[positive] ? 1.0F : 0.0F) - (keys[negative] ? 1.0F : 0.0F);
    };
    return {
        .move = {axis(SDL_SCANCODE_D, SDL_SCANCODE_A),
                 axis(SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT),
                 axis(SDL_SCANCODE_W, SDL_SCANCODE_S)},
        // Mouse right turns right, a negative yaw; mouse up, a negative y,
        // looks up.
        .yaw_turn = -motion.x * radians_per_pixel,
        .pitch_turn = -motion.y * radians_per_pixel,
        .fast = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL],
    };
}

MovementKeys read_walking_keys(const Window& window) {
    if (!window.mouse_captured()) {
        return {};
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    return {.forward = keys[SDL_SCANCODE_W],
            .back = keys[SDL_SCANCODE_S],
            .left = keys[SDL_SCANCODE_A],
            .right = keys[SDL_SCANCODE_D],
            .jump = keys[SDL_SCANCODE_SPACE],
            .sneak = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT],
            .sprint = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]};
}

} // namespace vx::app
