#include <vx/render/projection.hpp>

#include <doctest/doctest.h>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cmath>

namespace {

constexpr float aspect = 16.0F / 9.0F;
constexpr float z_near = 0.1F;

glm::mat4 projection() {
    return vx::render::perspective_reversed_infinite(glm::radians(70.0F), aspect, z_near);
}

// Where a point in view space lands in normalized device coordinates.
glm::vec3 ndc(glm::vec3 view) {
    const glm::vec4 clip = projection() * glm::vec4(view, 1.0F);
    return glm::vec3(clip) / clip.w;
}

} // namespace

TEST_CASE("the near plane maps to depth 1, and distance toward 0") {
    CHECK(static_cast<double>(ndc({0.0F, 0.0F, -z_near}).z) == doctest::Approx(1.0));
    // Depth is the near distance over the point's distance.
    CHECK(static_cast<double>(ndc({0.0F, 0.0F, -10.0F}).z) == doctest::Approx(0.01));
    const float far = ndc({0.0F, 0.0F, -1.0e6F}).z;
    CHECK(far > 0.0F);
    CHECK(far < ndc({0.0F, 0.0F, -10.0F}).z);
}

TEST_CASE("a point above the view lands toward the image's top, Vulkan's y being down") {
    CHECK(ndc({0.0F, 1.0F, -10.0F}).y < 0.0F);
    CHECK(ndc({0.0F, -1.0F, -10.0F}).y > 0.0F);
}

TEST_CASE("the field of view and the aspect ratio reach the image's edges") {
    // At distance 10, the top edge is 10 * tan(35 degrees) up, and the right
    // edge `aspect` times as far to the side.
    const float half_height = 10.0F * std::tan(glm::radians(35.0F));
    CHECK(static_cast<double>(ndc({0.0F, half_height, -10.0F}).y) == doctest::Approx(-1.0));
    CHECK(static_cast<double>(ndc({aspect * half_height, 0.0F, -10.0F}).x) == doctest::Approx(1.0));
}
