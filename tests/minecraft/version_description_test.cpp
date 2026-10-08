#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/version_description.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using vx::minecraft::VersionDescription;

// Seconds since 1970 in UTC, worked out apart from the parser, with Python's
// datetime.
constexpr std::int64_t release_26_2 = 1'781'611'413; // 2026-06-16T12:03:33+00:00

// A description read from where the launcher keeps 26.2's, and what reading
// it said, formatted.
struct Read {
    std::optional<VersionDescription> description;
    std::vector<std::string> diagnostics;
};

Read read(std::string_view text) {
    std::vector<vx::Diagnostic> diagnostics;
    Read got{.description = vx::minecraft::read_version_description("versions/26.2/26.2.json", text,
                                                                    diagnostics),
             .diagnostics = {}};
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        got.diagnostics.push_back(vx::format(diagnostic));
    }
    return got;
}

// The one warning a description that cannot be used gives.
std::string passed_over(std::string_view text) {
    const Read got = read(text);
    CHECK_FALSE(got.description.has_value());
    REQUIRE(got.diagnostics.size() == 1);
    return got.diagnostics.front();
}

} // namespace

TEST_CASE("a release time is the seconds since 1970 in UTC, whichever way the offset is written") {
    using vx::minecraft::parse_release_time;
    // Mojang's launcher, Fabric's installer and the Modrinth App.
    CHECK(parse_release_time("2026-06-16T12:03:33+00:00") == release_26_2);
    CHECK(parse_release_time("2026-06-16T12:03:33+0000") == release_26_2);
    CHECK(parse_release_time("2026-06-16T12:03:33Z") == release_26_2);
    CHECK(parse_release_time("2024-08-08T17:08:20.288544446Z") == 1'723'136'900);
    // An offset east of UTC is earlier than the same clock reading in UTC,
    // one west of it later.
    CHECK(parse_release_time("2025-08-02T16:47:02+02:00") == 1'754'146'022);
    CHECK(parse_release_time("2024-12-31T23:30:00-05:30") == 1'735'707'600);
    CHECK(parse_release_time("2026-06-16T14:03:33+02:00") == release_26_2);
    CHECK(parse_release_time("2026-06-16T07:03:33-0500") == release_26_2);
}

TEST_CASE("release times reach back before 1970 and across leap days") {
    using vx::minecraft::parse_release_time;
    CHECK(parse_release_time("1970-01-01T00:00:00Z") == 0);
    CHECK(parse_release_time("1969-12-31T23:59:59Z") == -1);
    CHECK(parse_release_time("2000-02-29T12:00:00Z") == 951'825'600);
    CHECK(parse_release_time("2015-12-03T09:24:39+00:00") == 1'449'134'679);
    CHECK(parse_release_time("0001-01-01T00:00:00Z") == -62'135'596'800);
    CHECK(parse_release_time("9999-12-31T23:59:59Z") == 253'402'300'799);
    // A leap second is a time too.
    CHECK(parse_release_time("2016-12-31T23:59:60Z") == parse_release_time("2017-01-01T00:00:00Z"));
}

TEST_CASE("text that is not a release time is nothing") {
    using vx::minecraft::parse_release_time;
    for (const std::string_view text : {
             "",
             "2026-06-16",
             "2026-06-16T12:03:33",    // no offset
             "2026-06-16T12:03Z",      // no seconds
             "2026-06-16 12:03:33Z",   // a space for the T
             "2026-06-16t12:03:33z",   // lower case
             "26-06-16T12:03:33Z",     // a two-digit year
             "+2026-06-16T12:03:33Z",  // a sign on the year
             "2026-6-16T12:03:33Z",    // a one-digit month
             "2026-13-16T12:03:33Z",   // no month 13
             "2026-00-16T12:03:33Z",   // or 0
             "2026-06-31T12:03:33Z",   // June has 30 days
             "2025-02-29T12:03:33Z",   // 2025 is no leap year
             "2026-06-00T12:03:33Z",   // no day 0
             "2026-06-16T24:00:00Z",   // the hours stop at 23
             "2026-06-16T12:60:33Z",   // the minutes at 59
             "2026-06-16T12:03:61Z",   // the seconds at 60
             "2026-06-16T12:03:33.Z",  // a point with no digits
             "2026-06-16T12:03:33.5",  // a fraction, but no offset
             "2026-06-16T12:03:33+0",  // a short offset
             "2026-06-16T12:03:33+00", // hours alone
             "2026-06-16T12:03:33+24:00",
             "2026-06-16T12:03:33+00:60",
             "2026-06-16T12:03:33+00:00:00",
             "2026-06-16T12:03:33ZZ",
             "2026-06-16T12:03:33Z ", // anything after the offset
             // A no-break space, and the year in full-width digits.
             "2026-06-16T12:03:33\xC2\xA0Z",
             "\xEF\xBC\x92\xEF\xBC\x90\xEF\xBC\x92\xEF\xBC\x96-06-16T12:03:33Z",
         }) {
        CAPTURE(text);
        CHECK(parse_release_time(text) == std::nullopt);
    }
}

TEST_CASE("a release's description gives its release time") {
    // Shaped like the launcher's own, which says much more.
    const Read got = read(R"({
  "arguments": {"game": ["--username", "${auth_player_name}"]},
  "id": "26.2",
  "libraries": [{"name": "com.mojang:blocklist:1.0.10"}],
  "mainClass": "net.minecraft.client.main.Main",
  "releaseTime": "2026-06-16T12:03:33+00:00",
  "time": "2026-06-16T12:03:33+00:00",
  "type": "release"
})");
    REQUIRE(got.description.has_value());
    CHECK(got.description->release);
    CHECK(got.description->release_time == release_26_2);
    CHECK(got.diagnostics.empty());
}

TEST_CASE("snapshots, older kinds and modded profiles are not releases, and need no release time") {
    for (const std::string_view text : {
             R"({"id": "26.3-snapshot-1", "type": "snapshot", "releaseTime": "2026-07-01"})",
             R"({"id": "b1.7.3", "type": "old_beta"})",
             // Fabric's profile says it is a release, made when the profile was.
             R"({"id": "fabric-loader-0.19.2-1.20.1", "inheritsFrom": "1.20.1",
                 "type": "release", "releaseTime": "2026-04-29T06:16:13+0000"})",
             R"({"inheritsFrom": "1.20.1"})",
         }) {
        CAPTURE(text);
        const Read got = read(text);
        REQUIRE(got.description.has_value());
        CHECK_FALSE(got.description->release);
        CHECK(got.diagnostics.empty());
    }
}

TEST_CASE("a description that is not JSON is passed over, with a warning where it goes wrong") {
    // Columns count characters: the bare word starts at the eleventh.
    CHECK(passed_over("{\n  \"id\": \"26.2\",\n  \"type\": release\n}\n") ==
          "versions/26.2/26.2.json:3:11: warning: not JSON: unexpected character, expected a "
          "JSON value; the version is passed over");
    // A character of two bytes before the problem counts once.
    CHECK(passed_over("{\"id\": \"caf\xC3\xA9\", \"type\": release}") ==
          "versions/26.2/26.2.json:1:24: warning: not JSON: unexpected character, expected a "
          "JSON value; the version is passed over");
    CHECK(passed_over(R"({"id": "26.2", "type": "release")") ==
          "versions/26.2/26.2.json:1:33: warning: not JSON: unexpected end of data; the version "
          "is passed over");
    CHECK(passed_over("{\"type\": \"rel\xFF\"}") ==
          "versions/26.2/26.2.json:1:14: warning: not JSON: invalid UTF-8 encoding in string; "
          "the version is passed over");
    CHECK(passed_over("{} {}") == "versions/26.2/26.2.json:1:4: warning: not JSON: unexpected "
                                  "content after document; the version is passed over");
    // Nothing in it is a problem of the whole file.
    for (const std::string_view text : {"", " \n\t "}) {
        CAPTURE(text);
        CHECK(passed_over(text) == "versions/26.2/26.2.json: warning: not JSON: the text is "
                                   "empty; the version is passed over");
    }
}

TEST_CASE("a description without the fields a release needs is passed over, saying which") {
    CHECK(passed_over(R"(["26.2", "release"])") ==
          "versions/26.2/26.2.json: warning: a version's description is a JSON object; the "
          "version is passed over");
    for (const std::string_view text : {R"({"id": "26.2"})", R"({"type": 1})"}) {
        CAPTURE(text);
        CHECK(passed_over(text) == "versions/26.2/26.2.json: warning: a version's description "
                                   "gives its type as a string, such as 'release'; the version "
                                   "is passed over");
    }
    for (const std::string_view text :
         {R"({"type": "release"})", R"({"type": "release", "releaseTime": 1781611413})"}) {
        CAPTURE(text);
        CHECK(passed_over(text) == "versions/26.2/26.2.json: warning: a release's description "
                                   "gives its releaseTime as a string; the version is passed "
                                   "over");
    }
    CHECK(passed_over(R"({"type": "release", "releaseTime": "2026-06-16"})") ==
          "versions/26.2/26.2.json: warning: releaseTime '2026-06-16' is not a time such as "
          "2026-06-16T12:03:33+00:00; the version is passed over");
}

TEST_CASE("a warning joins the diagnostics already given") {
    std::vector<vx::Diagnostic> diagnostics(1);
    CHECK_FALSE(vx::minecraft::read_version_description("a.json", "[]", diagnostics));
    CHECK(diagnostics.size() == 2);
}
