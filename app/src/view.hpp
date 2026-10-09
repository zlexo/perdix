#pragma once

#include "walking.hpp"

#include <vx/render/camera.hpp>

namespace vx::app {

// The camera to draw for the player's `camera` with this frame's `bob`, so
// that the view is the game's: Minecraft 26.2 shifts and turns the scene
// after the camera's own rotation (from its jar). This camera undoes the
// turn through its roll and pitch, and the shift, taken back through the
// turned view, through its position; `camera` itself never moves.
[[nodiscard]] render::Camera bobbed(const render::Camera& camera, const Bob& bob);

} // namespace vx::app
