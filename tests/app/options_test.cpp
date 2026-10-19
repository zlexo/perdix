#include "options.hpp"
#include "overlay.hpp"

#include <vx/core/job_system.hpp>
#include <vx/core/result.hpp>
#include <vx/world/life.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
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
    CHECK(none.value().flythrough == vx::app::Flythrough::none);
    const vx::Result<vx::app::Options> fly = parse({"--flythrough"});
    REQUIRE(fly.has_value());
    CHECK(fly.value().flythrough == vx::app::Flythrough::full);
    const vx::Result<vx::app::Options> quick = parse({"--flythrough=quick"});
    REQUIRE(quick.has_value());
    CHECK(quick.value().flythrough == vx::app::Flythrough::quick);
    const vx::Result<vx::app::Options> endurance = parse({"--flythrough=long"});
    REQUIRE(endurance.has_value());
    CHECK(endurance.value().flythrough == vx::app::Flythrough::endurance);
    // RFC 0005's workers: unset by default, switching their QoS.
    CHECK_FALSE(none.value().workers.has_value());
    CHECK(none.value().worker_qos == vx::WorkerQos::switching);
    for (const auto& [name, qos] : {std::pair{"utility", vx::WorkerQos::utility},
                                    std::pair{"user-initiated", vx::WorkerQos::user_initiated},
                                    std::pair{"switching", vx::WorkerQos::switching}}) {
        const std::string flag = std::string("--worker-qos=") + name;
        const vx::Result<vx::app::Options> parsed = parse({"--workers=3", flag});
        REQUIRE(parsed.has_value());
        CHECK(parsed.value().workers == 3U);
        CHECK(parsed.value().worker_qos == qos);
    }
    // RFC 0006's view distance: unset by default, then any the slider offers.
    CHECK_FALSE(none.value().view_distance.has_value());
    for (const std::int32_t chunks :
         {vx::app::least_view_distance, 16, vx::app::most_view_distance}) {
        const std::string flag = "--view-distance=" + std::to_string(chunks);
        const vx::Result<vx::app::Options> parsed = parse({flag, "--flythrough"});
        REQUIRE(parsed.has_value());
        CHECK(parsed.value().view_distance == chunks);
        CHECK(parsed.value().flythrough == vx::app::Flythrough::full);
    }
}

TEST_CASE("the command line's view distances are the overlay slider's") {
    CHECK(vx::app::least_view_distance == vx::app::RenderDistance::least);
    CHECK(vx::app::most_view_distance == vx::app::RenderDistance::most);
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
    CHECK(error_of({"--flythrough=slow"}).starts_with("unknown option \"--flythrough=slow\""));
    CHECK(error_of({"--workers=0"}) == "--workers takes a whole number from 1 to 64, not \"0\"");
    CHECK(error_of({"--workers=65"}) == "--workers takes a whole number from 1 to 64, not \"65\"");
    CHECK(error_of({"--workers=six"}) ==
          "--workers takes a whole number from 1 to 64, not \"six\"");
    CHECK(error_of({"--worker-qos=fast"}) ==
          "--worker-qos takes switching, utility or user-initiated, not \"fast\"");
    CHECK(error_of({"--view-distance=3"}) ==
          "--view-distance takes a whole number of chunks from 4 to 64, not \"3\"");
    CHECK(error_of({"--view-distance=65"}) ==
          "--view-distance takes a whole number of chunks from 4 to 64, not \"65\"");
    CHECK(error_of({"--view-distance=far"}) ==
          "--view-distance takes a whole number of chunks from 4 to 64, not \"far\"");
    CHECK(error_of({"--view-distance="}) ==
          "--view-distance takes a whole number of chunks from 4 to 64, not \"\"");
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

TEST_CASE("--skin and --perdix-skin name the skins the players wear") {
    const vx::Result<vx::app::Options> none = parse({});
    REQUIRE(none.has_value());
    CHECK_FALSE(none.value().skin.has_value());
    CHECK_FALSE(none.value().perdix_skin.has_value());

    const vx::Result<vx::app::Options> both = parse({"--skin=steve:slim", "--perdix-skin=zlex"});
    REQUIRE(both.has_value());
    CHECK(both.value().skin == "steve:slim");
    CHECK(both.value().perdix_skin == "zlex");
    CHECK(error_of({"--skin="}).starts_with("--skin= needs a skin"));
    CHECK(error_of({"--perdix-skin="}).starts_with("--perdix-skin= needs a skin"));
}

TEST_CASE("--perdix sets what Perdix does, his own life without it") {
    const vx::Result<vx::app::Options> none = parse({});
    REQUIRE(none.has_value());
    CHECK(none.value().perdix == vx::LifeMode::life);
    for (const auto& [arg, mode] : {std::pair{"--perdix=life", vx::LifeMode::life},
                                    std::pair{"--perdix=follow", vx::LifeMode::follow},
                                    std::pair{"--perdix=still", vx::LifeMode::still}}) {
        INFO(arg);
        const vx::Result<vx::app::Options> set = parse({arg});
        REQUIRE(set.has_value());
        CHECK(set.value().perdix == mode);
    }
    CHECK(error_of({"--perdix=sit"}) == "--perdix takes life, follow or still, not \"sit\"");
    CHECK(error_of({"--perdix="}) == "--perdix takes life, follow or still, not \"\"");
}
