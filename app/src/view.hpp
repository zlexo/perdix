#pragma once

#include "walking.hpp"

#include <vx/render/camera.hpp>
#include <vx/render/fog.hpp>

#include <cstdint>
#include <optional>

namespace vx::app {

// The camera to draw for the player's `camera` with this frame's `bob`, so
// that the view is the game's: Minecraft 26.2 shifts and turns the scene
// after the camera's own rotation (from its jar). This camera undoes the
// turn through its roll and pitch, and the shift, taken back through the
// turned view, through its position; `camera` itself never moves.
[[nodiscard]] render::Camera bobbed(const render::Camera& camera, const Bob& bob);

// RFC 0013's fog for a frame, 26.2's noon fog at a view of `held_chunks`,
// the view distance the world holds; none when it is switched off.
[[nodiscard]] std::optional<render::Fog> frame_fog(bool on, std::int32_t held_chunks);

} // namespace vx::app
