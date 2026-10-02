// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest and
// skipped by the Linux presets until RFC 0001's lavapipe job (pull request
// plan item 6). On macOS, source the Vulkan SDK's setup-env.sh first.

#include <vx/render/instance.hpp>

#include <doctest/doctest.h>

#include <string>
#include <utility>

TEST_CASE("an instance with validation creates cleanly") {
    vx::Result<vx::render::Instance> created =
        vx::render::Instance::create({.extensions = {}, .validation = true, .portability = false});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());

    const vx::render::Instance& instance = created.value();
    CHECK(instance.handle() != VK_NULL_HANDLE);
    CHECK(instance.loader_version() >= VK_API_VERSION_1_3);
    CHECK_FALSE(instance.lists_portability_drivers());
    CHECK(instance.validation_counts().errors == 0);
}

TEST_CASE("an instance can opt in to portability drivers") {
    vx::Result<vx::render::Instance> created =
        vx::render::Instance::create({.extensions = {}, .validation = true, .portability = true});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());

    CHECK(created.value().lists_portability_drivers());
    CHECK(created.value().validation_counts().errors == 0);
}

TEST_CASE("moving an instance moves ownership of its handle") {
    vx::Result<vx::render::Instance> created =
        vx::render::Instance::create({.extensions = {}, .validation = false, .portability = false});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Instance first = std::move(created).value();
    const VkInstance handle = first.handle();

    vx::render::Instance second = std::move(first);

    CHECK(second.handle() == handle);
    CHECK(first.handle() == VK_NULL_HANDLE); // a moved-from instance owns nothing
}
