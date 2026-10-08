#include "options.hpp"

#include <vx/core/result.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

vx::Result<vx::app::Options> parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list(args);
    return vx::app::parse_options(list);
}

std::string error_of(std::initializer_list<std::string_view> args) {
    const vx::Result<vx::app::Options> options = parse(args);
    REQUIRE_FALSE(options.has_value());
    return options.error().message;
}

} // namespace

TEST_CASE("every option is read, and none at all is a run with nothing set") {
    const vx::Result<vx::app::Options> none = parse({});
    REQUIRE(none.has_value());
    CHECK(none.value().frames == 0);
    CHECK_FALSE(none.value().overlay);
    CHECK_FALSE(none.value().pack.has_value());
    CHECK_FALSE(none.value().minecraft.has_value());
    CHECK_FALSE(none.value().no_minecraft);

    const vx::Result<vx::app::Options> all =
        parse({"--frames=60", "--overlay", "--pack=packs/x", "--minecraft=games/mc",
               "--minecraft-version=26.2"});
    REQUIRE(all.has_value());
    CHECK(all.value().frames == 60);
    CHECK(all.value().overlay);
    CHECK(all.value().pack == "packs/x");
    CHECK(all.value().minecraft == "games/mc");
    CHECK(all.value().minecraft_version == "26.2");
    const vx::Result<vx::app::Options> off = parse({"--no-minecraft"});
    REQUIRE(off.has_value());
    CHECK(off.value().no_minecraft);
}

TEST_CASE("options that cannot be used say which, and why") {
    CHECK(error_of({"--minecraft="}) == "--minecraft= needs a game folder or a client jar");
    CHECK(error_of({"--minecraft-version="}) ==
          "--minecraft-version= needs a version folder's name");
    CHECK(error_of({"--no-minecraft", "--minecraft=x.jar"}) ==
          "--no-minecraft goes without --minecraft and --minecraft-version");
    CHECK(error_of({"--minecraft-version=26.2", "--no-minecraft"}) ==
          "--no-minecraft goes without --minecraft and --minecraft-version");
    CHECK(error_of({"--frames=many"}) == "--frames takes a whole number, not \"many\"");
    CHECK(error_of({"--fly"}).starts_with("unknown option \"--fly\": try --frames=N"));
}

TEST_CASE("--no-minecraft turns the game off, and the environment fills the search") {
    const vx::Result<vx::app::Options> off = parse({"--no-minecraft"});
    REQUIRE(off.has_value());
    CHECK_FALSE(vx::app::minecraft_choice(off.value(), {}).use);

    const vx::Result<vx::app::Options> on = parse({"--minecraft-version=26.2"});
    REQUIRE(on.has_value());
    const vx::app::MinecraftChoice choice = vx::app::minecraft_choice(
        on.value(), {.home = "/home/player", .appdata = "/home/player/AppData/Roaming"});
    CHECK(choice.use);
    CHECK(choice.query.version == "26.2");
    CHECK_FALSE(choice.query.path.has_value());
    CHECK(choice.query.home == std::filesystem::path("/home/player"));
    CHECK(choice.query.appdata == std::filesystem::path("/home/player/AppData/Roaming"));
}

TEST_CASE("a relative --minecraft path is made absolute, so messages name its files anywhere") {
    const vx::Result<vx::app::Options> options = parse({"--minecraft=games/mc"});
    REQUIRE(options.has_value());
    const vx::app::MinecraftChoice choice = vx::app::minecraft_choice(options.value(), {});
    REQUIRE(choice.query.path.has_value());
    CHECK(choice.query.path->is_absolute());
    CHECK(*choice.query.path == std::filesystem::current_path() / "games" / "mc");
}
