#include <vx/render/camera.hpp>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <numbers>

namespace {

constexpr float quarter_turn = std::numbers::pi_v<float> / 2.0F;

bool close(const glm::vec3& a, const glm::vec3& b) {
    return glm::distance(a, b) < 1.0e-5F;
}

} // namespace

TEST_CASE("with no yaw or pitch, the camera looks down -z") {
    CHECK(close(vx::render::forward(0.0F, 0.0F), {0.0F, 0.0F, -1.0F}));
    CHECK(vx::render::view_rotation(0.0F, 0.0F) == glm::mat4(1.0F));
}

TEST_CASE("yaw turns the camera left, and pitch tilts it up") {
    CHECK(close(vx::render::forward(quarter_turn, 0.0F), {-1.0F, 0.0F, 0.0F}));
    CHECK(close(vx::render::forward(0.0F, quarter_turn / 2.0F),
                {0.0F, std::numbers::sqrt2_v<float> / 2.0F, -std::numbers::sqrt2_v<float> / 2.0F}));
}

TEST_CASE("the view turns where the camera looks into -z, and keeps up as up") {
    for (const auto [yaw, pitch] :
         std::array{std::array{0.3F, 0.2F}, std::array{-2.0F, -1.1F}, std::array{3.0F, 1.4F}}) {
        INFO("yaw ", yaw, ", pitch ", pitch);
        const glm::mat4 view = vx::render::view_rotation(yaw, pitch);
        const glm::vec4 ahead = view * glm::vec4(vx::render::forward(yaw, pitch), 0.0F);
        CHECK(close(glm::vec3(ahead), {0.0F, 0.0F, -1.0F}));
        const glm::vec4 up =
            vx::render::view_rotation(yaw, 0.0F) * glm::vec4(0.0F, 1.0F, 0.0F, 0.0F);
        CHECK(close(glm::vec3(up), {0.0F, 1.0F, 0.0F}));
    }
}

TEST_CASE("a point straight ahead lands in the image's middle, in front of the camera") {
    vx::render::Camera camera;
    camera.yaw = 0.3F;
    camera.pitch = 0.2F;
    const glm::vec3 ahead = 10.0F * vx::render::forward(camera.yaw, camera.pitch);
    const glm::vec4 clip =
        vx::render::view_projection(camera, 16.0F / 9.0F) * glm::vec4(ahead, 1.0F);

    CHECK(clip.w > 0.0F);
    CHECK(static_cast<double>(clip.x / clip.w) == doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(static_cast<double>(clip.y / clip.w) == doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(static_cast<double>(clip.z / clip.w) ==
          doctest::Approx(0.005)); // the near distance, 0.05, over 10
}

TEST_CASE("flying forward follows the yaw and stays level, whatever the pitch") {
    vx::render::Camera camera;
    camera.yaw = quarter_turn; // facing -x
    camera.pitch = 0.8F;
    vx::render::fly(camera, {.move = {0.0F, 0.0F, 1.0F}}, 0.5F);

    // Ten blocks a second for half a second.
    CHECK(close(glm::vec3(camera.position), {-5.0F, 0.0F, 0.0F}));
}

TEST_CASE("up is the world's up, and a diagonal is no faster than a straight line") {
    vx::render::Camera rising;
    rising.pitch = -0.5F;
    vx::render::fly(rising, {.move = {0.0F, 1.0F, 0.0F}}, 1.0F);
    CHECK(close(glm::vec3(rising.position), {0.0F, 10.0F, 0.0F}));

    vx::render::Camera diagonal;
    vx::render::fly(diagonal, {.move = {1.0F, 0.0F, 1.0F}}, 1.0F);
    CHECK(static_cast<double>(glm::length(glm::vec3(diagonal.position))) == doctest::Approx(10.0));
}

TEST_CASE("Ctrl flies four times as fast") {
    vx::render::Camera camera;
    vx::render::fly(camera, {.move = {0.0F, 0.0F, 1.0F}, .fast = true}, 1.0F);
    CHECK(close(glm::vec3(camera.position), {0.0F, 0.0F, -40.0F}));
}

TEST_CASE("the pitch stops short of straight up and down, and the yaw wraps") {
    vx::render::Camera camera;
    vx::render::fly(camera, {.yaw_turn = 7.0F, .pitch_turn = 10.0F}, 0.0F);
    CHECK(camera.pitch < quarter_turn);
    CHECK(camera.pitch > quarter_turn - 0.01F);
    // 7 radians is a full turn and about 0.717 more.
    CHECK(static_cast<double>(camera.yaw) == doctest::Approx(7.0 - 2.0 * std::numbers::pi));

    vx::render::fly(camera, {.pitch_turn = -20.0F}, 0.0F);
    CHECK(camera.pitch > -quarter_turn);
}
