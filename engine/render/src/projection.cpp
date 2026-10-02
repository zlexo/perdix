#include <vx/render/projection.hpp>

#include <cmath>

namespace vx::render {

glm::mat4 perspective_reversed_infinite(float fov_y, float aspect, float z_near) {
    const float f = 1.0F / std::tan(fov_y * 0.5F);
    // glm takes columns: each row here is a column of the matrix.
    return {
        {f / aspect, 0.0F, 0.0F, 0.0F},
        {0.0F, -f, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, -1.0F},
        {0.0F, 0.0F, z_near, 0.0F},
    };
}

} // namespace vx::render
