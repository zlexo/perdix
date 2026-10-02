#include <vx/render/camera_relative.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_int3_sized.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace {

// RFC 0001's test plan: far from the world's origin, where a world position
// in floats would be off by up to a block.
constexpr double far = 10'000'000.0;

} // namespace

TEST_CASE("a position splits into the block it is in and its place in that block") {
    const vx::render::BlockAndFraction split = vx::render::split_position({10.25, -3.75, 0.0});
    CHECK(split.block == glm::i64vec3(10, -4, 0));
    CHECK(static_cast<double>(split.fraction.x) == doctest::Approx(0.25));
    CHECK(static_cast<double>(split.fraction.y) == doctest::Approx(0.25));
    CHECK(split.fraction.z == 0.0F);
}

TEST_CASE("10 million blocks out, a point near the camera keeps float precision") {
    const glm::dvec3 camera = {far + 0.3, 64.5, -far + 0.7};
    const glm::dvec3 corner = {far + 2.0, 65.0, -far + 1.0}; // a block's corner
    const vx::render::BlockAndFraction split = vx::render::split_position(camera);

    // What the vertex shader computes: the corner's block relative to the
    // camera's, less the camera's fraction.
    const glm::vec3 seen =
        vx::render::relative_origin(glm::i64vec3(corner), split.block) - split.fraction;
    const glm::vec3 exact = glm::vec3(corner - camera);
    CHECK(glm::distance(seen, exact) < 1.0e-6F);

    // World positions in floats would be off by up to half a block here.
    const glm::vec3 in_floats = glm::vec3(corner) - glm::vec3(camera);
    CHECK(glm::distance(in_floats, exact) > 0.1F);
}

TEST_CASE("two neighboring chunks give their shared corner the same, exact float") {
    // Past 2^24 blocks from the world's origin, where a float no longer holds
    // every whole block, with the camera a few chunks away.
    const vx::render::BlockAndFraction camera =
        vx::render::split_position({100'000'001.5, 70.0, -100'000'001.5});
    // Chunks are 32 blocks along each side (RFC 0002): the far corner of the
    // first is the near corner of the second.
    const glm::i64vec3 first = {100'000'003, 64, -100'000'003};
    const glm::i64vec3 second = first + glm::i64vec3(32, 0, 0);

    const glm::vec3 from_first =
        vx::render::relative_origin(first, camera.block) + glm::vec3(32.0F, 0.0F, 0.0F);
    const glm::vec3 from_second = vx::render::relative_origin(second, camera.block);
    CHECK(from_first == from_second);
    CHECK(from_second == glm::vec3(34.0F, -6.0F, -1.0F));
}
