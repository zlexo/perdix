#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace vx::minecraft {

// The systems whose launchers keep the game in folders of their own. Linux's
// is linux_os, since GNU modes of C++ define `linux` as a macro.
enum class System : std::uint8_t { macos, windows, linux_os };

// The system this build is for. Other systems than these three keep the game
// where Linux does.
#ifdef __APPLE__
inline constexpr System this_system = System::macos;
#elif defined(_WIN32)
inline constexpr System this_system = System::windows;
#else
inline constexpr System this_system = System::linux_os;
#endif

// Where the official launcher keeps the game on `system` (RFC 0009), in the
// order to look: on macOS, ~/Library/Application Support/minecraft; on
// Windows, %APPDATA%\.minecraft, or ~/.minecraft without APPDATA, as Chunky
// looks; on Linux, ~/.minecraft, then ~/.var/app/com.mojang.Minecraft/.minecraft,
// where the Flathub package keeps it. Nothing is looked for under an empty
// home, which would name the working folder.
[[nodiscard]] std::vector<std::filesystem::path>
game_folders(System system, const std::filesystem::path& home,
             const std::optional<std::filesystem::path>& appdata);

} // namespace vx::minecraft
