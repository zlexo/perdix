#include "../core/zip_bytes.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vx::minecraft::game_folders;
using vx::minecraft::System;

using Folders = std::vector<fs::path>;

const fs::path player_home = fs::path("home") / "player";
const fs::path player_appdata = fs::path("home") / "player" / "AppData" / "Roaming";

// A path as messages give it.
std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// A folder of its own in the system's temporary folder, removed when the
// test ends, even after a failed check.
class TemporaryFolder {
public:
    TemporaryFolder() {
        std::random_device random;
        path_ = fs::temp_directory_path() /
                ("vx-minecraft-" + std::to_string(random()) + "-" + std::to_string(random()));
        fs::create_directories(path_);
    }
    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;
    TemporaryFolder(TemporaryFolder&&) = delete;
    TemporaryFolder& operator=(TemporaryFolder&&) = delete;
    ~TemporaryFolder() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

// A jar that opens: an archive with nothing in it.
std::string empty_jar() {
    const vx::test::Zip zip = vx::test::make_zip({});
    return {reinterpret_cast<const char*>(zip.bytes.data()), zip.bytes.size()};
}

std::string release(std::string_view time) {
    return R"({"type": "release", "releaseTime": ")" + std::string(time) + R"("})";
}

// A version folder in `game`, as the launcher writes one: its description,
// and its jar unless there is none.
void add_version(const fs::path& game, std::string_view id, std::string_view description,
                 const std::optional<std::string>& jar = empty_jar()) {
    const fs::path folder = game / "versions" / id;
    write(folder / (std::string(id) + ".json"), description);
    if (jar) {
        write(folder / (std::string(id) + ".jar"), *jar);
    }
}

fs::path jar_of(const fs::path& game, std::string_view id) {
    return game / "versions" / id / (std::string(id) + ".jar");
}

vx::minecraft::InstallQuery named(const fs::path& game) {
    return {.path = game};
}

// What find_install gave, and its diagnostics, formatted.
struct Found {
    vx::Result<vx::minecraft::Install> install;
    std::vector<std::string> diagnostics;
};

Found find(const vx::minecraft::InstallQuery& query) {
    std::vector<vx::Diagnostic> diagnostics;
    vx::Result<vx::minecraft::Install> install = vx::minecraft::find_install(query, diagnostics);
    std::vector<std::string> formatted;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        formatted.push_back(vx::format(diagnostic));
    }
    return {.install = std::move(install), .diagnostics = std::move(formatted)};
}

// What was found, or nothing, with a failed check saying why. None of these
// stops the test case: inside a check, a stop is caught, and the next line
// would read a jar that is not there.
std::string version_of(const Found& found) {
    if (!found.install.has_value()) {
        FAIL_CHECK(found.install.error().message);
        return {};
    }
    return found.install.value().version;
}

fs::path path_of(const Found& found) {
    if (!found.install.has_value()) {
        FAIL_CHECK(found.install.error().message);
        return {};
    }
    return found.install.value().path;
}

// Why nothing was found, or nothing, with a failed check, when a jar was.
std::string why(const Found& found) {
    if (found.install.has_value()) {
        const std::string what = "found " + utf8(found.install.value().path);
        FAIL_CHECK(what);
        return {};
    }
    return found.install.error().message;
}

// A file or folder made unreadable for the life of the guard, and readable
// again after, so the temporary folder can be removed. Where permissions do
// not stop reading, as for root, held() is false.
class Unreadable {
public:
    explicit Unreadable(fs::path path) : path_(std::move(path)) {
        std::error_code ignored;
        fs::permissions(path_, fs::perms::none, ignored);
    }
    Unreadable(const Unreadable&) = delete;
    Unreadable& operator=(const Unreadable&) = delete;
    Unreadable(Unreadable&&) = delete;
    Unreadable& operator=(Unreadable&&) = delete;
    ~Unreadable() {
        std::error_code ignored;
        fs::permissions(path_, fs::perms::owner_all, ignored);
    }

    [[nodiscard]] bool held() const {
        std::error_code error;
        if (fs::is_directory(path_, error)) {
            fs::directory_iterator listing(path_, error);
            return static_cast<bool>(error);
        }
        return !std::ifstream(path_).is_open();
    }

private:
    fs::path path_;
};

} // namespace

TEST_CASE("each system's launcher keeps the game in a folder of its own") {
    CHECK(game_folders(System::macos, player_home, std::nullopt) ==
          Folders{player_home / "Library" / "Application Support" / "minecraft"});
    CHECK(game_folders(System::windows, player_home, player_appdata) ==
          Folders{player_appdata / ".minecraft"});
    // Linux has the launcher's own folder, then Flathub's.
    CHECK(game_folders(System::linux_os, player_home, std::nullopt) ==
          Folders{player_home / ".minecraft",
                  player_home / ".var" / "app" / "com.mojang.Minecraft" / ".minecraft"});
    // APPDATA is only Windows'.
    CHECK(game_folders(System::macos, player_home, player_appdata) ==
          game_folders(System::macos, player_home, {}));
    CHECK(game_folders(System::linux_os, player_home, player_appdata) ==
          game_folders(System::linux_os, player_home, {}));
}

TEST_CASE("Windows without APPDATA looks in the home folder") {
    CHECK(game_folders(System::windows, player_home, std::nullopt) ==
          Folders{player_home / ".minecraft"});
    CHECK(game_folders(System::windows, player_home, fs::path()) ==
          Folders{player_home / ".minecraft"});
}

TEST_CASE("nothing is looked for under an empty home") {
    CHECK(game_folders(System::macos, fs::path(), std::nullopt).empty());
    CHECK(game_folders(System::linux_os, fs::path(), player_appdata).empty());
    CHECK(game_folders(System::windows, fs::path(), std::nullopt).empty());
    CHECK(game_folders(System::windows, fs::path(), player_appdata) ==
          Folders{player_appdata / ".minecraft"});
}

TEST_CASE("the system is the one the build is for") {
#ifdef __APPLE__
    CHECK(vx::minecraft::this_system == System::macos);
#elif defined(_WIN32)
    CHECK(vx::minecraft::this_system == System::windows);
#else
    CHECK(vx::minecraft::this_system == System::linux_os);
#endif
}

TEST_CASE("the newest release with a jar is chosen, by release time") {
    const TemporaryFolder game;
    // By name, 1.21.10 comes first and 1.8.9 last: neither is the newest.
    add_version(game.path(), "1.21.10", release("2025-10-07T09:17:23+00:00"));
    add_version(game.path(), "1.8.9", release("2015-12-03T09:24:39+00:00"));
    add_version(game.path(), "1.21.11", release("2025-12-09T12:23:30+00:00"));
    const Found found = find(named(game.path()));
    CHECK(version_of(found) == "1.21.11");
    CHECK(path_of(found) == jar_of(game.path(), "1.21.11"));
    if (found.install.has_value()) {
        CHECK(found.install.value().jar.size() == 0);
    }
    CHECK(found.diagnostics.empty());
}

TEST_CASE("a version folder without its jar is passed over before its description is read") {
    const TemporaryFolder game;
    add_version(game.path(), "26.2", release("2026-06-16T12:03:33+00:00"));
    // Newer, listed by the launcher but never played: no jar. Its
    // description is not JSON, and no warning says so, since it is not read.
    add_version(game.path(), "26.3", "not JSON", std::nullopt);
    // A jar without a description is passed over too.
    write(jar_of(game.path(), "26.4"), empty_jar());
    const Found found = find(named(game.path()));
    CHECK(version_of(found) == "26.2");
    CHECK(found.diagnostics.empty());
}

TEST_CASE("snapshots and modded profiles are passed over, whatever their times") {
    const TemporaryFolder game;
    add_version(game.path(), "26.2", release("2026-06-16T12:03:33+00:00"));
    add_version(game.path(), "26.3-snapshot-1",
                R"({"type": "snapshot", "releaseTime": "2026-07-01T10:00:00+00:00"})");
    // Fabric's profile: a release, made later than the version it builds on,
    // with its own copy of that version's jar.
    add_version(game.path(), "fabric-loader-0.19.2-26.2",
                R"({"inheritsFrom": "26.2", "type": "release",
                    "releaseTime": "2026-07-02T10:00:00+0000"})");
    const Found found = find(named(game.path()));
    CHECK(version_of(found) == "26.2");
    CHECK(found.diagnostics.empty());
}

TEST_CASE("releases of the same time are taken in byte order of their names") {
    const TemporaryFolder game;
    add_version(game.path(), "1.21.9", release("2025-09-30T11:58:43+00:00"));
    add_version(game.path(), "1.21.10", release("2025-09-30T11:58:43+00:00"));
    CHECK(version_of(find(named(game.path()))) == "1.21.10");

    // Forty of three times, interleaved by name, whose jars do not open, so
    // that the sort has to move them: their warnings show the order they were
    // tried in, the newest time first, each time's in byte order.
    const TemporaryFolder tied;
    const std::array<std::string, 3> times = {
        "2026-06-16T12:03:33+00:00", "2026-05-15T15:45:43+00:00", "2026-04-09T10:12:23+00:00"};
    std::array<std::vector<std::string>, 3> warnings;
    for (int minor = 10; minor < 50; ++minor) {
        const std::string id = "26." + std::to_string(minor);
        const auto time = static_cast<std::size_t>(minor % 3);
        add_version(tied.path(), id, release(times.at(time)), "not a zip archive");
        warnings.at(time).push_back(utf8(jar_of(tied.path(), id)) +
                                    ": warning: not a zip archive: it has no end of central "
                                    "directory record; the version is passed over");
    }
    std::vector<std::string> in_order;
    for (const std::vector<std::string>& of_one_time : warnings) {
        in_order.insert(in_order.end(), of_one_time.begin(), of_one_time.end());
    }
    CHECK(find(named(tied.path())).diagnostics == in_order);
}

TEST_CASE("a description that cannot be used is a warning on it, and its version is passed over") {
    const TemporaryFolder game;
    add_version(game.path(), "26.2", release("2026-06-16T12:03:33+00:00"));
    add_version(game.path(), "26.4", R"({"type": "release"})");
    add_version(game.path(), "26.3", "{\n  \"type\": release\n}");
    const Found found = find(named(game.path()));
    CHECK(version_of(found) == "26.2");
    // In byte order of the folders' names.
    const fs::path versions = game.path() / "versions";
    CHECK(found.diagnostics ==
          std::vector<std::string>{
              utf8(versions / "26.3" / "26.3.json") +
                  ":2:11: warning: not JSON: unexpected character, expected a JSON value; the "
                  "version is passed over",
              utf8(versions / "26.4" / "26.4.json") +
                  ": warning: a release's description gives its releaseTime as a string; the "
                  "version is passed over"});
}

TEST_CASE("a newer release whose jar does not open is a warning, and the next is used") {
    const TemporaryFolder game;
    add_version(game.path(), "26.2", release("2026-06-16T12:03:33+00:00"), "not a zip archive");
    add_version(game.path(), "26.1.2", release("2026-04-09T10:12:23+00:00"));
    add_version(game.path(), "1.21.11", release("2025-12-09T12:23:30+00:00"), "nor this");
    const Found found = find(named(game.path()));
    CHECK(version_of(found) == "26.1.2");
    // The older jar is never tried.
    CHECK(found.diagnostics ==
          std::vector<std::string>{utf8(jar_of(game.path(), "26.2")) +
                                   ": warning: not a zip archive: it has no end of central "
                                   "directory record; the version is passed over"});
}

TEST_CASE("a game folder without a release whose jar opens says which it lacks") {
    const TemporaryFolder game;
    add_version(game.path(), "26.3-snapshot-1",
                R"({"type": "snapshot", "releaseTime": "2026-07-01T10:00:00+00:00"})");
    add_version(game.path(), "26.3", release("2026-07-15T10:00:00+00:00"), std::nullopt);
    Found found = find(named(game.path()));
    CHECK(why(found) == "no Minecraft release with a jar in " + utf8(game.path()));
    CHECK(found.diagnostics.empty());

    add_version(game.path(), "26.2", release("2026-06-16T12:03:33+00:00"), "not a zip archive");
    found = find(named(game.path()));
    CHECK(why(found) == "no Minecraft release in " + utf8(game.path()) + " has a jar that opens");
    CHECK(found.diagnostics.size() == 1);
}

TEST_CASE("a description or a versions folder that cannot be read is a warning") {
    const TemporaryFolder game;
    add_version(game.path(), "26.2", release("2026-06-16T12:03:33+00:00"));
    add_version(game.path(), "26.3", release("2026-07-15T10:00:00+00:00"));
    const fs::path versions = game.path() / "versions";
    {
        const Unreadable description(versions / "26.3" / "26.3.json");
        if (!description.held()) {
            MESSAGE("skipped: permissions do not stop this user reading a file");
            return;
        }
        const Found found = find(named(game.path()));
        CHECK(version_of(found) == "26.2");
        CHECK(found.diagnostics ==
              std::vector<std::string>{utf8(versions / "26.3" / "26.3.json") +
                                       ": warning: cannot be read; the version is passed over"});
    }
    const Unreadable listing(versions);
    const Found found = find(named(game.path()));
    CHECK(why(found) == "no Minecraft release with a jar in " + utf8(game.path()));
    REQUIRE(found.diagnostics.size() == 1);
    CHECK(found.diagnostics[0].starts_with(utf8(versions) + ": warning: cannot be listed ("));
    CHECK(found.diagnostics[0].ends_with("); no version in it is used"));
}

TEST_CASE("files and other folders among the versions are passed over") {
    const TemporaryFolder game;
    add_version(game.path(), "26.2", release("2026-06-16T12:03:33+00:00"));
    write(game.path() / "versions" / "version_manifest_v2.json", "{}");
    // A copy whose files kept the old name is no version folder.
    write(game.path() / "versions" / "26.2-copy" / "26.2.json",
          release("2026-09-01T00:00:00+00:00"));
    write(game.path() / "versions" / "26.2-copy" / "26.2.jar", empty_jar());
    const Found found = find(named(game.path()));
    CHECK(version_of(found) == "26.2");
    CHECK(found.diagnostics.empty());
}

TEST_CASE("a named path that is not a game folder is an error saying why") {
    const TemporaryFolder folder;
    const fs::path missing = folder.path() / "missing";
    CHECK(why(find(named(missing))) == utf8(missing) + " does not exist");
    // --minecraft= with nothing after it.
    CHECK(why(find(named(fs::path()))) == "an empty path names no game folder and no jar");
    // A folder with no versions in it, such as the versions folder itself.
    CHECK(why(find(named(folder.path()))) ==
          utf8(folder.path()) + " is not a game folder: it holds no versions folder");
    write(folder.path() / "notes.txt", "");
    CHECK(why(find(named(folder.path() / "notes.txt"))) ==
          utf8(folder.path() / "notes.txt") + " is not a game folder: it holds no versions folder");
}

TEST_CASE("without a path, the launcher's folders are looked in, in order") {
    const TemporaryFolder home;
    const vx::minecraft::InstallQuery query{.home = home.path(), .system = System::linux_os};
    const fs::path launcher = home.path() / ".minecraft";
    const fs::path flathub = home.path() / ".var" / "app" / "com.mojang.Minecraft" / ".minecraft";
    CHECK(why(find(query)) ==
          "no Minecraft game folder at " + utf8(launcher) + " or " + utf8(flathub));

    // The first folder, with no release, does not stop the search.
    fs::create_directories(launcher / "versions");
    CHECK(why(find(query)) == "no Minecraft release with a jar in " + utf8(launcher));
    add_version(flathub, "26.1.2", release("2026-04-09T10:12:23+00:00"));
    Found found = find(query);
    CHECK(version_of(found) == "26.1.2");
    CHECK(path_of(found) == jar_of(flathub, "26.1.2"));

    // A release in the first folder is used, even an older one.
    add_version(launcher, "1.21.11", release("2025-12-09T12:23:30+00:00"));
    found = find(query);
    CHECK(version_of(found) == "1.21.11");
    CHECK(path_of(found) == jar_of(launcher, "1.21.11"));
}

TEST_CASE("Flathub's folder is looked in when the launcher's own has no jar that opens") {
    const TemporaryFolder home;
    const fs::path launcher = home.path() / ".minecraft";
    const fs::path flathub = home.path() / ".var" / "app" / "com.mojang.Minecraft" / ".minecraft";
    add_version(launcher, "26.2", release("2026-06-16T12:03:33+00:00"), "not a zip archive");
    add_version(flathub, "26.1.2", release("2026-04-09T10:12:23+00:00"));
    const Found found = find({.home = home.path(), .system = System::linux_os});
    CHECK(path_of(found) == jar_of(flathub, "26.1.2"));
    CHECK(found.diagnostics ==
          std::vector<std::string>{utf8(jar_of(launcher, "26.2")) +
                                   ": warning: not a zip archive: it has no end of central "
                                   "directory record; the version is passed over"});
}

TEST_CASE("a game folder with no versions folder yet is passed over with no warning") {
    // A launcher installed and never run.
    const TemporaryFolder home;
    fs::create_directories(home.path() / ".minecraft");
    const Found found = find({.home = home.path(), .system = System::linux_os});
    CHECK(why(found) == "no Minecraft release with a jar in " + utf8(home.path() / ".minecraft"));
    CHECK(found.diagnostics.empty());
}

TEST_CASE("each system's folder is where find_install looks") {
    const TemporaryFolder home;
    const fs::path appdata = home.path() / "AppData" / "Roaming";
    add_version(home.path() / "Library" / "Application Support" / "minecraft", "26.2",
                release("2026-06-16T12:03:33+00:00"));
    add_version(appdata / ".minecraft", "26.1.2", release("2026-04-09T10:12:23+00:00"));
    const auto query = [&](System system) {
        return vx::minecraft::InstallQuery{
            .home = home.path(), .appdata = appdata, .system = system};
    };
    CHECK(version_of(find(query(System::macos))) == "26.2");
    CHECK(version_of(find(query(System::windows))) == "26.1.2");
    CHECK(why(find({})) == "no home folder to look for Minecraft in");
}
