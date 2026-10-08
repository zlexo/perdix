#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vx::minecraft {

// When a version came out, from the release time in its description, such as
// 2026-06-16T12:03:33+00:00 (RFC 0009): seconds since 1970 in UTC, or nothing
// when the text is not such a time. Mojang's launcher writes the offset as
// +00:00 and Fabric's installer as +0000; the Modrinth App writes Z, some
// times after a fraction of a second, which is read past and left out.
[[nodiscard]] std::optional<std::int64_t> parse_release_time(std::string_view text);

} // namespace vx::minecraft
