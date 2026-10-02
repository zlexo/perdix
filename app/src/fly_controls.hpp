#pragma once

#include "window.hpp"

#include <vx/render/camera.hpp>

namespace vx::app {

// RFC 0001's controls: W, A, S and D to move, Space and Shift for up and
// down, Ctrl to go faster, and the captured mouse to look. With the mouse
// released, the camera stays where it is.
[[nodiscard]] render::FlyInput read_fly_controls(Window& window);

} // namespace vx::app
