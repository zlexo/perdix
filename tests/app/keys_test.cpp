#include "keys.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <doctest/doctest.h>

#include <array>

using vx::app::Controls;
using vx::app::KeyAction;

namespace {

constexpr SDL_WindowID window = 7;

SDL_Event key_down(SDL_Keycode key, bool repeat = false) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = window;
    event.key.key = key;
    event.key.down = true;
    event.key.repeat = repeat;
    return event;
}

SDL_Event click(SDL_WindowID in = window) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = in;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.clicks = 1;
    return event;
}

// One event as Window::handle_events passes it, not taken by the overlay
// or the panel unless `taken`, and with no text being typed unless `typing`.
Controls after(Controls controls, const SDL_Event& event, bool taken = false, bool typing = false) {
    vx::app::control(controls, event, window, taken, typing);
    return controls;
}

bool same(const Controls& a, const Controls& b) {
    return a.overlay == b.overlay && a.panel == b.panel && a.captured == b.captured &&
           a.give_back == b.give_back && a.free_camera == b.free_camera;
}

// Every state the keys can find the app in, flying or not, the overlay and
// panel shown or not.
std::array<Controls, 6> every_state() {
    return {Controls{},
            Controls{.captured = true},
            Controls{.overlay = true},
            Controls{.overlay = true, .captured = true},
            Controls{.panel = true},
            Controls{.overlay = true, .panel = true, .give_back = true, .free_camera = true}};
}

} // namespace

TEST_CASE("O shows the overlay, V switches the free camera and G opens the god panel") {
    CHECK(vx::app::key_action(SDLK_O) == KeyAction::overlay);
    CHECK(vx::app::key_action(SDLK_V) == KeyAction::free_camera);
    CHECK(vx::app::key_action(SDLK_G) == KeyAction::god_panel);
    // The keys that walk and fly do nothing else.
    for (const SDL_Keycode walking :
         {SDLK_W, SDLK_A, SDLK_S, SDLK_D, SDLK_SPACE, SDLK_LSHIFT, SDLK_LCTRL, SDLK_ESCAPE}) {
        CHECK(vx::app::key_action(walking) == KeyAction::none);
    }
}

TEST_CASE("no key on the F row does anything") {
    constexpr std::array<SDL_Keycode, 24> f_row = {
        SDLK_F1,  SDLK_F2,  SDLK_F3,  SDLK_F4,  SDLK_F5,  SDLK_F6,  SDLK_F7,  SDLK_F8,
        SDLK_F9,  SDLK_F10, SDLK_F11, SDLK_F12, SDLK_F13, SDLK_F14, SDLK_F15, SDLK_F16,
        SDLK_F17, SDLK_F18, SDLK_F19, SDLK_F20, SDLK_F21, SDLK_F22, SDLK_F23, SDLK_F24};
    for (const SDL_Keycode key : f_row) {
        INFO("key ", key);
        CHECK(vx::app::key_action(key) == KeyAction::none);
        for (const Controls& state : every_state()) {
            CHECK(same(after(state, key_down(key)), state));
        }
    }
}

TEST_CASE("O, V and G each switch their one thing, and a second press switches it back") {
    const Controls overlay = after({}, key_down(SDLK_O));
    CHECK(same(overlay, {.overlay = true}));
    CHECK(same(after(overlay, key_down(SDLK_O)), {}));
    const Controls free_camera = after({}, key_down(SDLK_V));
    CHECK(same(free_camera, {.free_camera = true}));
    CHECK(same(after(free_camera, key_down(SDLK_V)), {}));
    const Controls panel = after({}, key_down(SDLK_G));
    CHECK(same(panel, {.panel = true}));
    CHECK(same(after(panel, key_down(SDLK_G)), {}));
}

TEST_CASE("the god panel frees the mouse, and a click in the world takes it back") {
    const Controls open = after({.captured = true}, key_down(SDLK_G));
    CHECK(open.panel);
    CHECK_FALSE(open.captured);
    // A click on the panel, which it takes, captures nothing; it stays open.
    const Controls clicked = after(open, click(), true);
    CHECK(clicked.panel);
    CHECK_FALSE(clicked.captured);
    // A click in the world captures the mouse and closes the panel.
    const Controls back = after(clicked, click());
    CHECK(back.captured);
    CHECK_FALSE(back.panel);
    CHECK_FALSE(back.give_back);
    // A click in another window does nothing.
    CHECK(same(after(open, click(window + 1)), open));
}

TEST_CASE("G closing the panel gives the mouse back only when the panel took it") {
    // Flying when it opened: flying again, as the game's inventory does.
    const Controls flying = after(after({.captured = true}, key_down(SDLK_G)), key_down(SDLK_G));
    CHECK(same(flying, {.captured = true}));
    // Never clicked: still free, so a run never clicked never grabs it.
    const Controls free = after(after({}, key_down(SDLK_G)), key_down(SDLK_G));
    CHECK(same(free, {}));
}

TEST_CASE("Escape frees the mouse and closes the panel") {
    CHECK(same(after({.captured = true}, key_down(SDLK_ESCAPE)), {}));
    const Controls open = after({.captured = true}, key_down(SDLK_G));
    const Controls closed = after(open, key_down(SDLK_ESCAPE));
    CHECK(same(closed, {}));
    // Closed that way it gives nothing back on the next G pair.
    CHECK(same(after(after(closed, key_down(SDLK_G)), key_down(SDLK_G)), {}));
    // Even while text is typed, Escape frees the mouse.
    CHECK_FALSE(
        after({.overlay = true, .captured = true}, key_down(SDLK_ESCAPE), false, true).captured);
}

TEST_CASE("while text is typed, and while a key is held, the keys do nothing") {
    for (const SDL_Keycode key : {SDLK_O, SDLK_V, SDLK_G}) {
        INFO("key ", key);
        for (const Controls& state : every_state()) {
            CHECK(same(after(state, key_down(key), false, true), state));
            CHECK(same(after(state, key_down(key, true)), state));
        }
    }
}
