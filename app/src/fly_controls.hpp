#pragma once

#include "window.hpp"

#include <vx/render/camera.hpp>
#include <vx/world/movement.hpp>

namespace vx::app {

// RFC 0001's controls: W, A, S and D to move, Space and Shift for up and
// down, Ctrl to go faster, and the captured mouse to look. With the mouse
// released, the camera stays where it is.
[[nodiscard]] render::FlyInput read_fly_controls(Window& window);

// RFC 0010's keys, as the game has them, held now while the mouse is
// captured: W, A, S and D, Space to jump, Shift to crouch, Ctrl to sprint.
// None while the mouse is free.
[[nodiscard]] MovementKeys read_walking_keys(const Window& window);

} // namespace vx::app
