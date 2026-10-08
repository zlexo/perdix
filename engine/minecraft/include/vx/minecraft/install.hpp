#pragma once

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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

// What find_install looks for. The app fills it from its options and the
// environment, so find_install reads the environment only through it, and
// tests build game folders in a temporary folder. A field left out is empty,
// and the system is the build's.
struct InstallQuery {
    // --minecraft: a game folder, or a client jar.
    std::optional<std::filesystem::path> path = std::nullopt;
    // The player's home folder. The initializer is not redundant, whatever
    // clang-tidy says: without one, Clang warns when a designated
    // initializer leaves the field out.
    std::filesystem::path home{}; // NOLINT(readability-redundant-member-init)
    // %APPDATA%, on Windows.
    std::optional<std::filesystem::path> appdata = std::nullopt;
    // Whose launcher's folders to look in.
    System system = this_system;
};

// A Minecraft client jar, open for reading.
struct Install {
    Archive jar;
    std::filesystem::path path; // of the jar
    // "26.2": the id in the jar's version.json, which jars have had since
    // 1.14, or else the name of the jar's folder, or of the jar itself.
    std::string version;
};

// The jar to read (RFC 0009), or why there is none. In a game folder, the
// version is the newest release, by release time, that has a jar, and the
// first such jar that opens is used. A version folder without its jar is
// passed over before its description is read; a snapshot or a modded profile
// is passed over once its description says so. A jar named as the path skips
// the choice. Without a path, the launcher's folders for the system are looked
// in, in order. The reason there is none is an error when the query named a
// path; otherwise it is a line for the startup report. Versions passed over on
// the way, for a description that cannot be used or a newer jar that does not
// open, are warnings in `diagnostics`.
[[nodiscard]] Result<Install> find_install(const InstallQuery& query,
                                           std::vector<Diagnostic>& diagnostics);

} // namespace vx::minecraft
