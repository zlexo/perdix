#pragma once

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_video.h>

#include <cstdint>

namespace vx::app {

// What a key does in the app, besides walking and flying. Nothing is on the
// F row (RFC 0012): a 60% keyboard reaches it only through a function layer.
enum class KeyAction : std::uint8_t {
    none,
    overlay,     // O: shows and hides the overlay
    free_camera, // V: switches between walking and the free camera
    god_panel,   // G: opens and closes the god panel
};

// The action of a key pressed.
[[nodiscard]] KeyAction key_action(SDL_Keycode key) noexcept;

// What the app's keys and clicks switch (RFC 0012), apart from SDL's window
// and ImGui, which Window::handle_events reads them from and sets them on.
struct Controls {
    bool overlay = false;     // the overlay shows
    bool panel = false;       // the god panel is open
    bool captured = false;    // the mouse turns the camera
    bool give_back = false;   // the panel took the mouse from the camera
    bool free_camera = false; // V pressed an odd number of times since the caller took it
};

// What `event` does to `controls`, after the overlay and the panel saw it,
// `taken` when they took it, a click on them. O and V switch the overlay
// and the free camera; G opens the god panel and frees the mouse, as the
// game's inventory does, and closes it, giving the mouse back when it took
// it. A click in `window` they did not take captures the mouse, and closes
// the panel. Escape frees the mouse, and closes the panel. While they take
// text (`typing`), as into a slider's value, keys type and do nothing else,
// and a key held down does nothing more.
void control(Controls& controls, const SDL_Event& event, SDL_WindowID window, bool taken,
             bool typing) noexcept;

} // namespace vx::app
