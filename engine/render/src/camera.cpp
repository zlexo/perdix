#include <vx/render/camera.hpp>

#include <vx/render/projection.hpp>

#include <glm/ext/matrix_transform.hpp>

#include <cmath>

namespace vx::render {
namespace {

// A twentieth of a block. Reversed-Z keeps depth precise however close the
// near plane sits, so the camera can come right up to a wall.
constexpr float z_near = 0.05F;

} // namespace

glm::vec3 forward(float yaw, float pitch) {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

glm::mat4 view_rotation(float yaw, float pitch) {
    // The camera turns by yaw around +y, then tilts by pitch around its own x
    // axis; the view undoes both, the last first.
    const glm::mat4 undo_yaw = glm::rotate(glm::mat4(1.0F), -yaw, glm::vec3(0.0F, 1.0F, 0.0F));
    return glm::rotate(glm::mat4(1.0F), -pitch, glm::vec3(1.0F, 0.0F, 0.0F)) * undo_yaw;
}

glm::mat4 view_projection(const Camera& camera, float aspect) {
    return perspective_reversed_infinite(camera.fov_y, aspect, z_near) *
           view_rotation(camera.yaw, camera.pitch);
}

} // namespace vx::render
