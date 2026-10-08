#include <vx/world/names.hpp>

#include <doctest/doctest.h>

#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

TEST_CASE("a name uses a to z, 0 to 9 and _ alone") {
    for (const std::string_view name : {"stone", "oak_log_top", "x", "_", "26_2"}) {
        INFO(name);
        CHECK(vx::is_name(name));
    }
    for (const std::string_view text :
         {"", "Stone", "oak log", "oak-log", "core:stone", "stone.png", "caf\xC3\xA9"}) {
        INFO(text);
        CHECK_FALSE(vx::is_name(text));
    }
}

TEST_CASE("a near miss is one edit in three letters, at most two, and nearer than the rest") {
    constexpr std::array<std::string_view, 3> keys = {"opacity", "cull_group", "textures"};
    CHECK(vx::near_miss("opacty", keys) == "opacity");
    CHECK(vx::near_miss("textur", keys) == "textures");
    // Two edits are allowed from six letters on, and never more.
    CHECK(vx::near_miss("txtres", keys) == "textures");
    CHECK(vx::near_miss("opcty", keys) == std::nullopt);
    CHECK(vx::near_miss("cxll_gxoxp", keys) == std::nullopt);
    CHECK(vx::near_miss("flavour", keys) == std::nullopt);
    // As near to one as to the other.
    constexpr std::array<std::string_view, 2> axes = {"x", "y"};
    CHECK(vx::near_miss("w", axes) == std::nullopt);
    CHECK(vx::near_miss("w", std::span<const std::string_view>{}) == std::nullopt);
}

TEST_CASE("an unknown key says what it may have meant") {
    constexpr std::array<std::string_view, 2> keys = {"texture", "tint"};
    CHECK(vx::unknown_key("tin", keys) == "unknown key 'tin'; did you mean 'tint'?");
    CHECK(vx::unknown_key("shade", keys) == "unknown key 'shade'");
    CHECK(vx::did_you_mean("textures", keys) == "; did you mean 'texture'?");
    CHECK(vx::did_you_mean("shade", keys).empty());
}
