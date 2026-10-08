#include <vx/minecraft/install.hpp>

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/file.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/version_description.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vx::minecraft {
namespace {

namespace fs = std::filesystem;

// A path in UTF-8, for messages. On Windows, path::string() converts through
// the system's code page and throws on a character it cannot map, which
// engine code, built without exceptions, cannot catch.
std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void warn(std::vector<Diagnostic>& diagnostics, const fs::path& file, std::string message) {
    diagnostics.push_back({.severity = Severity::warning,
                           .file = utf8(file),
                           .line = 0,
                           .column = 0,
                           .message = std::move(message)});
}

// "a", "a or b", "a, b or c".
std::string listed(const std::vector<fs::path>& paths) {
    std::string text;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (index > 0) {
            text += index + 1 == paths.size() ? " or " : ", ";
        }
        text += utf8(paths[index]);
    }
    return text;
}

// A version folder's own files are named after it: versions/26.2/26.2.jar.
fs::path own_file(const fs::path& folder, std::string_view extension) {
    fs::path file = folder / folder.filename();
    file += extension;
    return file;
}

// A version folder that holds a release and its jar.
struct Candidate {
    std::string id; // the folder's name
    std::int64_t release_time = 0;
    fs::path jar;
};

// Every release with a jar in a game folder, newest first, and those of one
// time in byte order of their ids. A folder that lacks either file is passed
// over before its description is read.
std::vector<Candidate> releases_with_jars(const fs::path& game,
                                          std::vector<Diagnostic>& diagnostics) {
    const fs::path versions = game / "versions";
    // In byte order of their names, so that warnings come in the same order
    // on every system.
    struct Folder {
        std::string id;
        fs::path path;
    };
    std::vector<Folder> folders;
    std::error_code error;
    for (fs::directory_iterator entry(versions, error), end; !error && entry != end;
         entry.increment(error)) {
        folders.push_back({.id = utf8(entry->path().filename()), .path = entry->path()});
    }
    if (error) {
        if (error != std::errc::no_such_file_or_directory) {
            warn(diagnostics, versions,
                 "cannot be listed (" + error.message() + "); no version in it is used");
        }
        return {};
    }
    std::ranges::sort(folders, {}, &Folder::id);

    std::vector<Candidate> candidates;
    for (const Folder& folder : folders) {
        const fs::path jar = own_file(folder.path, ".jar");
        const fs::path json = own_file(folder.path, ".json");
        if (!fs::is_regular_file(jar, error) || !fs::is_regular_file(json, error)) {
            continue;
        }
        const std::optional<std::string> text = read_file(json);
        if (!text) {
            warn(diagnostics, json, "cannot be read; the version is passed over");
            continue;
        }
        const std::optional<VersionDescription> description =
            read_version_description(utf8(json), *text, diagnostics);
        if (description && description->release) {
            candidates.push_back(
                {.id = folder.id, .release_time = description->release_time, .jar = jar});
        }
    }
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.release_time != b.release_time ? a.release_time > b.release_time : a.id < b.id;
    });
    return candidates;
}

// The archive's message without the jar's path in front, which the
// diagnostic gives as its file.
std::string without_path(const std::string& message, const fs::path& jar) {
    const std::string path = utf8(jar) + ": ";
    return message.starts_with(path) ? message.substr(path.size()) : message;
}

// What a game folder gives: the newest release whose jar opens, and how many
// releases with a jar it holds, whether or not one opened.
struct Choice {
    std::optional<Install> install;
    std::size_t releases = 0;
};

Choice choose_release(const fs::path& game, std::vector<Diagnostic>& diagnostics) {
    const std::vector<Candidate> candidates = releases_with_jars(game, diagnostics);
    Choice choice{.install = std::nullopt, .releases = candidates.size()};
    for (const Candidate& candidate : candidates) {
        Result<Archive> jar = Archive::open(candidate.jar);
        if (jar) {
            choice.install = Install{
                .jar = std::move(jar).value(), .path = candidate.jar, .version = candidate.id};
            break;
        }
        warn(diagnostics, candidate.jar,
             without_path(jar.error().message, candidate.jar) + "; the version is passed over");
    }
    return choice;
}

// Why game folders gave no jar.
Error none_in(const std::vector<fs::path>& games, std::size_t releases) {
    if (releases > 0) {
        return Error{"no Minecraft release in " + listed(games) + " has a jar that opens"};
    }
    return Error{"no Minecraft release with a jar in " + listed(games)};
}

Result<Install> find_in_named(const fs::path& path, std::vector<Diagnostic>& diagnostics) {
    if (path.empty()) {
        return Error{"an empty path names no game folder and no jar"};
    }
    std::error_code error;
    const fs::file_status status = fs::status(path, error);
    if (status.type() == fs::file_type::not_found) {
        return Error{utf8(path) + " does not exist"};
    }
    if (error) {
        return Error{utf8(path) + " cannot be read (" + error.message() + ")"};
    }
    if (!fs::is_directory(status) || !fs::is_directory(path / "versions", error)) {
        return Error{utf8(path) + " is not a game folder: it holds no versions folder"};
    }
    Choice choice = choose_release(path, diagnostics);
    if (choice.install) {
        return std::move(*choice.install);
    }
    return none_in({path}, choice.releases);
}

} // namespace

std::vector<std::filesystem::path>
game_folders(System system, const std::filesystem::path& home,
             const std::optional<std::filesystem::path>& appdata) {
    std::vector<std::filesystem::path> folders;
    switch (system) {
    case System::macos:
        if (!home.empty()) {
            folders.push_back(home / "Library" / "Application Support" / "minecraft");
        }
        break;
    case System::windows:
        if (appdata && !appdata->empty()) {
            folders.push_back(*appdata / ".minecraft");
        } else if (!home.empty()) {
            folders.push_back(home / ".minecraft");
        }
        break;
    case System::linux_os:
        if (!home.empty()) {
            folders.push_back(home / ".minecraft");
            folders.push_back(home / ".var" / "app" / "com.mojang.Minecraft" / ".minecraft");
        }
        break;
    }
    return folders;
}

Result<Install> find_install(const InstallQuery& query, std::vector<Diagnostic>& diagnostics) {
    if (query.path) {
        return find_in_named(*query.path, diagnostics);
    }
    const std::vector<fs::path> folders = game_folders(query.system, query.home, query.appdata);
    if (folders.empty()) {
        return Error{"no home folder to look for Minecraft in"};
    }
    std::vector<fs::path> games;
    std::size_t releases = 0;
    for (const fs::path& folder : folders) {
        std::error_code error;
        if (!fs::is_directory(folder, error)) {
            continue;
        }
        games.push_back(folder);
        Choice choice = choose_release(folder, diagnostics);
        if (choice.install) {
            return std::move(*choice.install);
        }
        releases += choice.releases;
    }
    if (games.empty()) {
        return Error{"no Minecraft game folder at " + listed(folders)};
    }
    return none_in(games, releases);
}

} // namespace vx::minecraft
