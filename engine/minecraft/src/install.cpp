#include <vx/minecraft/install.hpp>

#include "json.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/file.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/version_description.hpp>

#include <yyjson.h>

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

fs::path from_utf8(std::string_view text) {
    return {std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())};
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

// A version's name as the startup report shows it: printable ASCII, as all
// 915 of Mojang's version ids are, and no longer than a name needs to be.
bool is_printable_name(std::string_view name) {
    return !name.empty() && name.size() <= 64 &&
           std::ranges::all_of(name, [](char c) { return c >= ' ' && c <= '~'; });
}

// The id in a jar's version.json, which jars have had since release 1.14:
// nothing for an older jar, and nothing, with a warning, for one whose
// version.json cannot be read.
std::optional<std::string> jar_version(const Archive& jar, const fs::path& path,
                                       const std::string& name,
                                       std::vector<Diagnostic>& diagnostics) {
    if (!jar.contains("version.json")) {
        return std::nullopt;
    }
    const std::string shown = "; the version shown is " + name + ", from its name";
    const Result<std::vector<std::byte>> bytes = jar.read("version.json");
    if (!bytes) {
        warn(diagnostics, path, without_path(bytes.error().message, path) + shown);
        return std::nullopt;
    }
    const JsonRead json =
        read_json({reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()});
    if (json.document == nullptr) {
        warn(diagnostics, path, "its version.json is not JSON: " + json.problem + shown);
        return std::nullopt;
    }
    const std::optional<std::string_view> id =
        string_member(yyjson_doc_get_root(json.document.get()), "id");
    if (!id || id->empty()) {
        warn(diagnostics, path, "its version.json gives no id as a string" + shown);
        return std::nullopt;
    }
    // The report shows it as it is.
    if (!is_printable_name(*id)) {
        warn(diagnostics, path,
             "its version.json gives an id that is not a version's name" + shown);
        return std::nullopt;
    }
    return std::string(*id);
}

// An open jar, shown as the version its version.json says it is, or else by
// `name`, its folder's or its file's.
Install make_install(Archive jar, const fs::path& path, const std::string& name,
                     std::vector<Diagnostic>& diagnostics) {
    std::optional<std::string> version = jar_version(jar, path, name, diagnostics);
    return {.jar = std::move(jar), .path = path, .version = version ? *std::move(version) : name};
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
            choice.install =
                make_install(std::move(jar).value(), candidate.jar, candidate.id, diagnostics);
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

// The newest release, in the first of the game folders that has one whose
// jar opens.
Result<Install> find_release(const std::vector<fs::path>& folders,
                             std::vector<Diagnostic>& diagnostics) {
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

// A jar the query named, by its path or its version: one that does not open
// is an error, not a version passed over.
Result<Install> open_named(const fs::path& jar, const std::string& name,
                           std::vector<Diagnostic>& diagnostics) {
    Result<Archive> archive = Archive::open(jar);
    if (!archive) {
        return archive.error();
    }
    return make_install(std::move(archive).value(), jar, name, diagnostics);
}

// The version folder named `id` in the first of the game folders that has
// one, whatever its type: only its jar is needed.
Result<Install> find_version(const std::vector<fs::path>& folders, const std::string& id,
                             std::vector<Diagnostic>& diagnostics) {
    std::vector<fs::path> games;
    for (const fs::path& game : folders) {
        std::error_code error;
        if (!fs::is_directory(game, error)) {
            continue;
        }
        games.push_back(game);
        const fs::path folder = game / "versions" / from_utf8(id);
        if (!fs::is_directory(folder, error)) {
            continue;
        }
        const fs::path jar = own_file(folder, ".jar");
        if (!fs::is_regular_file(jar, error)) {
            return Error{"version " + id + " in " + utf8(game) + " has no jar"};
        }
        return open_named(jar, id, diagnostics);
    }
    if (games.empty()) {
        return Error{"no Minecraft game folder at " + listed(folders)};
    }
    return Error{"no version " + id + " in " + listed(games)};
}

Result<Install> search(const std::vector<fs::path>& folders,
                       const std::optional<std::string>& version,
                       std::vector<Diagnostic>& diagnostics) {
    return version ? find_version(folders, *version, diagnostics)
                   : find_release(folders, diagnostics);
}

// A path the query names: a client jar, which skips the choice, or a game
// folder, searched like the launcher's.
Result<Install> find_in_named(const fs::path& path, const std::optional<std::string>& version,
                              std::vector<Diagnostic>& diagnostics) {
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
    if (fs::is_regular_file(status)) {
        if (version) {
            return Error{"version " + *version + " is named, but " + utf8(path) +
                         " is a jar, not a game folder"};
        }
        return open_named(path, utf8(path.stem()), diagnostics);
    }
    if (!fs::is_directory(status) || !fs::is_directory(path / "versions", error)) {
        return Error{utf8(path) + " is not a game folder: it holds no versions folder"};
    }
    return search({path}, version, diagnostics);
}

// Whether `name` can only name a folder in versions/, and no other: a
// version's name, with no slash, backslash or colon (D:26.2 would replace the
// whole path on Windows), and not . or ..; printable ASCII also leaves out
// what Windows could not turn into a path.
bool is_folder_name(std::string_view name) {
    return is_printable_name(name) && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string_view::npos;
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
    if (query.version && !is_folder_name(*query.version)) {
        return Error{"'" + *query.version + "' is not the name of a version folder"};
    }
    if (query.path) {
        return find_in_named(*query.path, query.version, diagnostics);
    }
    const std::vector<fs::path> folders = game_folders(query.system, query.home, query.appdata);
    if (folders.empty()) {
        return Error{"no home folder to look for Minecraft in"};
    }
    return search(folders, query.version, diagnostics);
}

} // namespace vx::minecraft
