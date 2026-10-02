#include <vx/render/camera.hpp>

#include <vx/render/projection.hpp>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vx::render {
namespace {

// A twentieth of a block. Reversed-Z keeps depth precise however close the
// near plane sits, so the camera can come right up to a wall.
constexpr float z_near = 0.05F;

// Just short of a quarter turn.
constexpr float max_pitch = (std::numbers::pi_v<float> / 2.0F) - 0.001F;

} // namespace

void fly(Camera& camera, const FlyInput& input, float seconds) {
    // Kept within a turn either way, so a long session cannot grow it.
    camera.yaw = std::remainder(camera.yaw + input.yaw_turn, 2.0F * std::numbers::pi_v<float>);
    camera.pitch = std::clamp(camera.pitch + input.pitch_turn, -max_pitch, max_pitch);

    const float sin_yaw = std::sin(camera.yaw);
    const float cos_yaw = std::cos(camera.yaw);
    const glm::vec3 forward_level = {-sin_yaw, 0.0F, -cos_yaw};
    const glm::vec3 right = {cos_yaw, 0.0F, -sin_yaw};
    const glm::vec3 direction = (input.move.x * right) + (input.move.z * forward_level) +
                                glm::vec3(0.0F, input.move.y, 0.0F);
    if (glm::length(direction) == 0.0F) {
        return;
    }
    const float speed = input.fast ? fast_fly_speed : fly_speed;
    camera.position += glm::dvec3(glm::normalize(direction) * (speed * seconds));
}

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
