#pragma once

#include <glm/ext/vector_double3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

namespace vx::render {

// Where the camera is and where it looks (RFC 0001): a position in blocks, in
// doubles so it stays exact far from the world's origin, and a heading
// without roll. With yaw and pitch at 0 it looks down -z, with +y up.
struct Camera {
    glm::dvec3 position{0.0};
    float yaw = 0.0F;                  // radians; positive turns left, seen from above
    float pitch = 0.0F;                // radians; positive looks up
    float fov_y = glm::radians(70.0F); // vertical, RFC 0001's default
};

// The direction the camera looks, a unit vector in world space.
[[nodiscard]] glm::vec3 forward(float yaw, float pitch);

// World directions into view space, where the camera looks down -z: the
// view's rotation alone, since the GPU never sees the camera's position.
[[nodiscard]] glm::mat4 view_rotation(float yaw, float pitch);

// The view's rotation, then the reversed-Z projection for a target of
// `aspect`: what vertex shaders multiply camera-relative positions by.
[[nodiscard]] glm::mat4 view_projection(const Camera& camera, float aspect);

} // namespace vx::render
