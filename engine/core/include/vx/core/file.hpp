#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace vx {

// All of the file at `path`, byte for byte, or nothing when it cannot be
// opened or a read fails partway, as reading a folder does. It reads with
// C's stdio, which reports a failed read the same way everywhere: through
// an istream, a failed read throws on Linux's standard library, which
// engine code, built without exceptions, cannot catch, and ends the file
// early on the Mac's, so the bytes read so far pass for all of it.
[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path);

} // namespace vx
