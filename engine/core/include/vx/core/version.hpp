#pragma once

#include <string_view>

namespace vx {

// The engine version. Set once, in the top-level CMakeLists.txt. Not
// Version, which is the world's for a chunk's copies (RFC 0005).
struct EngineVersion {
    int major;
    int minor;
    int patch;
};

[[nodiscard]] EngineVersion version() noexcept;

// The version as "major.minor.patch".
[[nodiscard]] std::string_view version_string() noexcept;

} // namespace vx
