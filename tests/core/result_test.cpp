#include <vx/core/result.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>

namespace {

vx::Result<int> parse_digit(char c) {
    if (c < '0' || c > '9') {
        return vx::Error{std::string("not a digit: ") + c};
    }
    return c - '0';
}

} // namespace

TEST_CASE("a Result holds the value it was made from") {
    const vx::Result<int> result = parse_digit('7');

    REQUIRE(result.has_value());
    CHECK(static_cast<bool>(result));
    CHECK(result.value() == 7);
}

TEST_CASE("a Result holds the error it was made from") {
    const vx::Result<int> result = parse_digit('x');

    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.error().message == "not a digit: x");
}

TEST_CASE("a Result can carry a move-only value out") {
    vx::Result<std::unique_ptr<int>> result = std::make_unique<int>(42);

    REQUIRE(result.has_value());
    const std::unique_ptr<int> owned = std::move(result).value();
    REQUIRE(owned != nullptr);
    CHECK(*owned == 42);
}
