#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vx {

// Names in a pack's files (RFC 0004): namespaces, blocks, properties, values
// and textures use a to z, 0 to 9 and _ only, so a pack loads the same on a
// case-insensitive file system as on Linux.
[[nodiscard]] bool is_name(std::string_view text) noexcept;
// What a message says after a name that breaks that rule.
inline constexpr std::string_view not_a_name = " uses characters other than a to z, 0 to 9 and _";

// The known key that "opacty" is a near miss of, "opacity": one edit for
// every three letters of the key, but at least one and at most two, and
// only when one known key is nearer than all the others ("w" is as near to
// "x" as to "y").
[[nodiscard]] std::optional<std::string_view> near_miss(std::string_view key,
                                                        std::span<const std::string_view> known);

// "; did you mean 'opacity'?" for "opacty", or nothing without a near miss.
[[nodiscard]] std::string did_you_mean(std::string_view key,
                                       std::span<const std::string_view> known);

// "unknown key 'opacty'; did you mean 'opacity'?", the guess only for a near
// miss.
[[nodiscard]] std::string unknown_key(std::string_view key,
                                      std::span<const std::string_view> known);

} // namespace vx
