#include <vx/minecraft/version_description.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace {

// Seconds since 1970 in UTC, worked out apart from the parser, with Python's
// datetime.
constexpr std::int64_t release_26_2 = 1'781'611'413; // 2026-06-16T12:03:33+00:00

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
