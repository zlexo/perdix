#pragma once

#include <vx/core/frustum.hpp>

#include <glm/ext/vector_double3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

namespace vx::render {

// Where the camera is and where it looks (RFC 0001): a position in blocks, in
// doubles so it stays exact far from the world's origin, and a heading. With
// yaw, pitch and roll at 0 it looks down -z, with +y up. Roll is for effects
// such as view bobbing (RFC 0010); the controls never set it.
struct Camera {
    glm::dvec3 position{0.0};
    float yaw = 0.0F;                  // radians; positive turns left, seen from above
    float pitch = 0.0F;                // radians; positive looks up
    float roll = 0.0F;                 // radians; positive leans left, counterclockwise
    float fov_y = glm::radians(70.0F); // vertical, RFC 0001's default
};

// What a fly camera does in one frame, from RFC 0001's controls.
struct FlyInput {
    // x right, y up, z forward, each from -1 to 1. Right and forward stay
    // level whatever the pitch, and up is the world's.
    glm::vec3 move{0.0F};
    float yaw_turn = 0.0F;   // radians, added to the yaw
    float pitch_turn = 0.0F; // radians, added to the pitch
    bool fast = false;
};

// Blocks a second.
inline constexpr float fly_speed = 10.0F;
inline constexpr float fast_fly_speed = 40.0F;

// Turns and moves the camera for one frame `seconds` long. Diagonals are no
// faster than straight lines, and the pitch stops just short of straight up
// and down, where the yaw would stop meaning anything.
void fly(Camera& camera, const FlyInput& input, float seconds);

// The direction the camera looks, a unit vector in world space.
[[nodiscard]] glm::vec3 forward(float yaw, float pitch);

// World directions into view space, where the camera looks down -z: the
// view's rotation alone, since the GPU never sees the camera's position.
// A roll of 0 leaves the matrix exactly as yaw and pitch make it.
[[nodiscard]] glm::mat4 view_rotation(float yaw, float pitch, float roll = 0.0F);

// The view's rotation, then the reversed-Z projection for a target of
// `aspect`: what vertex shaders multiply camera-relative positions by.
[[nodiscard]] glm::mat4 view_projection(const Camera& camera, float aspect);

// What the camera sees on a target of `aspect`, relative to its position:
// the frustum view_projection draws, for culling (RFC 0007) and for ranking
// what streams in (RFC 0006).
[[nodiscard]] Frustum view_frustum(const Camera& camera, float aspect);

} // namespace vx::render
