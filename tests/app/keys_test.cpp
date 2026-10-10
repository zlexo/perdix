#include "keys.hpp"

#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>

#include <array>

using vx::app::KeyAction;

TEST_CASE("O shows the overlay and V switches the free camera") {
    CHECK(vx::app::key_action(SDLK_O) == KeyAction::overlay);
    CHECK(vx::app::key_action(SDLK_V) == KeyAction::free_camera);
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
    }
}
