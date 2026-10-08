#pragma once

#include <vx/core/diagnostic.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace vx::minecraft {

// When a version came out, from the release time in its description, such as
// 2026-06-16T12:03:33+00:00 (RFC 0009): seconds since 1970 in UTC, or nothing
// when the text is not such a time. Mojang's launcher writes the offset as
// +00:00 and Fabric's installer as +0000; the Modrinth App writes Z, some
// times after a fraction of a second, which is read past and left out.
[[nodiscard]] std::optional<std::int64_t> parse_release_time(std::string_view text);

// What a launcher's description of a version, versions/<id>/<id>.json, says
// as far as choosing a version needs (RFC 0009).
struct VersionDescription {
    // A release of the game itself: type "release", and no inheritsFrom. A
    // modded profile, such as Fabric's, names the version it builds on in
    // inheritsFrom, and its release time is when the profile was made.
    bool release = false;
    std::int64_t release_time = 0; // of a release, as parse_release_time gives it
};

// The description in `text`, read from the file `path` names, or nothing when
// it cannot be used, with a warning on that file saying why. Only a release
// needs a release time.
[[nodiscard]] std::optional<VersionDescription>
read_version_description(std::string_view path, std::string_view text,
                         std::vector<Diagnostic>& diagnostics);

} // namespace vx::minecraft
