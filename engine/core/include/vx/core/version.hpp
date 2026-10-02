#pragma once

#include <string_view>

namespace vx {

// The engine version. Set once, in the top-level CMakeLists.txt.
struct Version {
    int major;
    int minor;
    int patch;
};

[[nodiscard]] Version version() noexcept;

// The version as "major.minor.patch".
[[nodiscard]] std::string_view version_string() noexcept;

} // namespace vx
