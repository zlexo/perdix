#include "frame.hpp"

#include "view.hpp"

namespace vx::app {

render::FrameInputs frame_inputs(const Overlay& overlay, const RenderDistance& distance,
                                 render::FrameInputs scene) {
    scene.fog = frame_fog(overlay.fog(), overlay.held_view_distance().value_or(distance.chunks));
    scene.chunk_fade_seconds = overlay.chunk_fade();
    return scene;
}

} // namespace vx::app
