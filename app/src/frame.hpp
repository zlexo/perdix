#pragma once

#include "overlay.hpp"

#include <vx/render/renderer.hpp>

namespace vx::app {

// One app frame for the renderer: `scene` (the sky, the camera, bodies and
// tags, the overlay's draw lists, the chunk changes) with the look the
// overlay chose for it: RFC 0013's fog, at the view distance the world holds
// (the distance chosen until the world reports one), and the chunks' fade.
[[nodiscard]] render::FrameInputs
frame_inputs(const Overlay& overlay, const RenderDistance& distance, render::FrameInputs scene);

} // namespace vx::app
