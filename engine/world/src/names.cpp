#include <vx/world/names.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vx {
namespace {

// Insertions, deletions and substitutions from one key to another.
std::size_t edit_distance(std::string_view from, std::string_view to) {
    std::vector<std::size_t> row(to.size() + 1);
    for (std::size_t column = 0; column < row.size(); ++column) {
        row[column] = column;
    }
    for (std::size_t line = 1; line <= from.size(); ++line) {
        std::size_t diagonal = row[0];
        row[0] = line;
        for (std::size_t column = 1; column <= to.size(); ++column) {
            const std::size_t above = row[column];
            const std::size_t substitution = from[line - 1] == to[column - 1] ? 0U : 1U;
            row[column] = std::min({above + 1, row[column - 1] + 1, diagonal + substitution});
            diagonal = above;
        }
    }
    return row.back();
}

} // namespace

bool is_name(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, [](char letter) {
        return (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') ||
               letter == '_';
    });
}

std::optional<std::string_view> near_miss(std::string_view key,
                                          std::span<const std::string_view> known) {
    const auto distance_to = [key](std::string_view candidate) {
        return edit_distance(key, candidate);
    };
    const auto nearest = std::ranges::min_element(known, {}, distance_to);
    if (nearest == known.end()) {
        return std::nullopt;
    }
    const std::size_t distance = distance_to(*nearest);
    const std::size_t allowed = std::clamp<std::size_t>(key.size() / 3, 1, 2);
    const auto equally_near = std::ranges::count_if(
        known, [&](std::string_view candidate) { return distance_to(candidate) == distance; });
    if (distance > allowed || equally_near > 1) {
        return std::nullopt;
    }
    return *nearest;
}

std::string did_you_mean(std::string_view key, std::span<const std::string_view> known) {
    const std::optional<std::string_view> meant = near_miss(key, known);
    return meant ? "; did you mean '" + std::string(*meant) + "'?" : std::string{};
}

std::string unknown_key(std::string_view key, std::span<const std::string_view> known) {
    return "unknown key '" + std::string(key) + "'" + did_you_mean(key, known);
}

} // namespace vx
