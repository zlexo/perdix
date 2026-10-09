#include "view.hpp"

#include "walking.hpp"

#include <vx/render/camera.hpp>

#include <glm/ext/vector_double3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

namespace vx::app {

render::Camera bobbed(const render::Camera& camera, const Bob& bob) {
    render::Camera shown = camera;
    shown.roll = -bob.turn_z;
    shown.pitch -= bob.turn_x;
    const glm::mat4 turned = render::view_rotation(shown.yaw, shown.pitch, shown.roll);
    const glm::vec4 shift =
        glm::transpose(turned) *
        glm::vec4(static_cast<float>(bob.shift_x), static_cast<float>(bob.shift_y), 0.0F, 0.0F);
    shown.position -= glm::dvec3(shift.x, shift.y, shift.z);
    return shown;
}

} // namespace vx::app
