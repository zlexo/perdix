#include <vx/core/version.hpp>

#include <doctest/doctest.h>

#include <string>

TEST_CASE("version string matches the version numbers") {
    const vx::Version v = vx::version();
    const std::string expected =
        std::to_string(v.major) + '.' + std::to_string(v.minor) + '.' + std::to_string(v.patch);

    CHECK(vx::version_string() == expected);
}

TEST_CASE("version numbers are not negative") {
    const vx::Version v = vx::version();

    CHECK(v.major >= 0);
    CHECK(v.minor >= 0);
    CHECK(v.patch >= 0);
}
