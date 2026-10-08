#include <vx/minecraft/version_description.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vx::minecraft {
namespace {

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// Whether `text` has the shape of `pattern`, in which 0 stands for a digit,
// + for either sign, and any other character for itself.
bool has_shape(std::string_view text, std::string_view pattern) noexcept {
    return std::ranges::equal(text, pattern, [](char c, char p) {
        if (p == '0') {
            return is_digit(c);
        }
        return p == '+' ? c == '+' || c == '-' : c == p;
    });
}

// The `length` digits of `text` from `at`, as a number.
int number(std::string_view text, std::size_t at, std::size_t length) noexcept {
    int value = 0;
    for (const char c : text.substr(at, length)) {
        value = (value * 10) + (c - '0');
    }
    return value;
}

} // namespace

std::optional<std::int64_t> parse_release_time(std::string_view text) {
    // The date and the time of day, then perhaps a fraction of a second, then
    // the offset from UTC.
    constexpr std::string_view date_and_time = "0000-00-00T00:00:00";
    if (!has_shape(text.substr(0, date_and_time.size()), date_and_time)) {
        return std::nullopt;
    }
    std::size_t at = date_and_time.size();
    // No two releases need a fraction of a second to tell them apart.
    if (at < text.size() && text[at] == '.') {
        const std::size_t first = at + 1;
        at = first;
        while (at < text.size() && is_digit(text[at])) {
            ++at;
        }
        if (at == first) {
            return std::nullopt;
        }
    }
    const std::string_view zone = text.substr(at);
    int offset = 0; // minutes east of UTC
    if (has_shape(zone, "+00:00") || has_shape(zone, "+0000")) {
        const int hours = number(zone, 1, 2);
        const int minutes = number(zone, zone.size() - 2, 2);
        if (hours > 23 || minutes > 59) {
            return std::nullopt;
        }
        offset = (zone.front() == '-' ? -1 : 1) * ((hours * 60) + minutes);
    } else if (zone != "Z") {
        return std::nullopt;
    }

    const std::chrono::year_month_day date{
        std::chrono::year{number(text, 0, 4)},
        std::chrono::month{static_cast<unsigned>(number(text, 5, 2))},
        std::chrono::day{static_cast<unsigned>(number(text, 8, 2))}};
    const std::int64_t hour = number(text, 11, 2);
    const std::int64_t minute = number(text, 14, 2);
    const std::int64_t second = number(text, 17, 2);
    // 60 is a leap second.
    if (!date.ok() || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }
    const auto days =
        static_cast<std::int64_t>(std::chrono::sys_days(date).time_since_epoch().count());
    return (days * 86'400) + (hour * 3'600) + (minute * 60) + second - (std::int64_t{offset} * 60);
}

} // namespace vx::minecraft
