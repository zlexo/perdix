#pragma once

#include <glm/mat4x4.hpp>

namespace vx::render {

// RFC 0001's projection: reversed-Z with an infinite far plane, in Vulkan's
// clip space, where y points down and depth runs from 0 to 1. The near plane
// maps to depth 1 and infinity to 0, so a 32-bit float depth keeps its
// densest precision far away, where depth needs it most, and the projection
// puts no limit on view distance. The view looks down -z; `fov_y` is the
// vertical field of view in radians.
[[nodiscard]] glm::mat4 perspective_reversed_infinite(float fov_y, float aspect, float z_near);

} // namespace vx::render
