#pragma once

#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>

namespace vx::render {

// A position split for camera-relative rendering (RFC 0001): the block it is
// in, as 64-bit integers, and its place within that block. The GPU only ever
// sees positions relative to the camera's block, so precision does not fade
// with distance from the world's origin.
struct BlockAndFraction {
    glm::i64vec3 block; // each axis rounded down
    glm::vec3 fraction; // each axis from 0 to 1
};

// Splits a position kept in doubles on the CPU, such as the camera's.
[[nodiscard]] BlockAndFraction split_position(const glm::dvec3& position);

// `origin`, a whole block, as seen from the camera's block. A whole number is
// exact as a float up to 2^24, so the result is exact while the two are
// within 16,777,216 blocks of each other, far beyond any view distance.
[[nodiscard]] glm::vec3 relative_origin(const glm::i64vec3& origin,
                                        const glm::i64vec3& camera_block);

} // namespace vx::render
