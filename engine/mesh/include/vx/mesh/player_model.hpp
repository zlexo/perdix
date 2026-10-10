#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/skin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vx {

// Everything that places and poses one body for one frame (RFC 0012),
// already interpolated between its last two ticks.
struct PlayerPose {
    Vec3d feet{};               // the bottom center of the player's box
    double limb_position = 0.0; // the walk swing's phase
    double limb_amount = 0.0;   // 0 to 1
    double age = 0.0;           // ticks, for the idle sway
    float body_yaw = 0.0F;      // radians, the engine's yaw
    float head_yaw = 0.0F;      // radians, the engine's yaw
    float head_pitch = 0.0F;    // radians, the engine's sign: positive looks up
    std::uint32_t skin = 0;     // which of the renderer's skins
    Arms arms = Arms::wide;     // the skin's
    bool crouching = false;
};

// One posed vertex, as the body pipeline pulls it. Plain floats: mesh, like
// world, has no glm.
struct BodyVertex {
    std::array<float, 3> position; // blocks, relative to the body's block
    std::array<float, 2> texel;    // on the skin, 0 to 1
    std::array<float, 3> normal;   // outward, posed
};

// Twelve boxes of six faces of two triangles.
inline constexpr std::size_t player_vertices = 432;

// Poses one body into `out`: positions relative to `origin`, the block the
// body stands in, texels on its skin, and outward normals. The boxes come in
// the order head, hat, body, jacket, right arm, right sleeve, left arm, left
// sleeve, right leg, right pants, left leg, left pants; each box's faces in
// the order top, bottom, right side, front, left side, back; and each face's
// two triangles wind counter-clockwise seen from outside the box.
void pose_player(const PlayerPose& pose, BlockPos origin,
                 std::span<BodyVertex, player_vertices> out) noexcept;

} // namespace vx
