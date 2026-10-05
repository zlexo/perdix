#pragma once

#include <vx/core/result.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace vx::app {

// The folder that holds the block pack's assets/ folder (RFC 0004): `given`,
// when the --pack option names one, or else the nearest folder that holds
// assets/, from the executable's own up, so that a build run from inside the
// repository finds the repository's pack.
[[nodiscard]] Result<std::filesystem::path> find_pack(std::optional<std::string_view> given);

} // namespace vx::app
