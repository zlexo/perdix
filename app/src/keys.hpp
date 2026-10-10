#pragma once

#include <SDL3/SDL_keycode.h>

#include <cstdint>

namespace vx::app {

// What a key does in the app, besides walking and flying. Nothing is on the
// F row (RFC 0012): a 60% keyboard reaches it only through a function layer.
enum class KeyAction : std::uint8_t {
    none,
    overlay,     // O: shows and hides the overlay
    free_camera, // V: switches between walking and the free camera
};

// The action of a key pressed.
[[nodiscard]] KeyAction key_action(SDL_Keycode key) noexcept;

} // namespace vx::app
