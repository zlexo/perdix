#include <vx/render/camera_relative.hpp>

#include <glm/common.hpp>

namespace vx::render {

BlockAndFraction split_position(const glm::dvec3& position) {
    const glm::dvec3 block = glm::floor(position);
    // The subtraction is exact in doubles, but for a position just below
    // zero, where it can lose the last bit. Only the step to float rounds, on
    // a value of at most 1, where floats are finest.
    return {.block = glm::i64vec3(block), .fraction = glm::vec3(position - block)};
}

glm::vec3 relative_origin(const glm::i64vec3& origin, const glm::i64vec3& camera_block) {
    return static_cast<glm::vec3>(origin - camera_block);
}

} // namespace vx::render
