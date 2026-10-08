#include "../core/png_bytes.hpp"
#include "../core/zip_bytes.hpp"
#include "startup_pack.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/render/block_textures.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vx::test::PngColor;

// A folder of its own under the system's temporary folder, holding a pack
// and, beside it, whatever a test puts there; removed when the test ends.
class TemporaryFolder {
public:
    TemporaryFolder() {
        std::random_device random;
        root_ = fs::temp_directory_path() /
                ("vx-app-startup-" + std::to_string(random()) + "-" + std::to_string(random()));
        fs::create_directories(root_);
    }
    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;
    TemporaryFolder(TemporaryFolder&&) = delete;
    TemporaryFolder& operator=(TemporaryFolder&&) = delete;
    ~TemporaryFolder() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    [[nodiscard]] const fs::path& root() const { return root_; }

    void write(const std::string& relative, std::string_view bytes) const {
        const fs::path path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << bytes;
    }

private:
    fs::path root_;
};

// A 16 by 16 gray texture of one value.
std::string gray_png(char value) {
    return vx::test::make_png(16, 16, PngColor::gray, std::string(256, value));
}

// A pack of one block, t:rock, whose texture file is gray 50, and whose
// map draws it as the game's stone.
void write_pack(const TemporaryFolder& folder, std::string_view map = "rock = \"stone\"\n") {
    folder.write("pack/assets/t/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    folder.write("pack/assets/t/textures/blocks/rock.png", gray_png('\x32'));
    folder.write("pack/assets/t/minecraft.toml", map);
}

// A client jar holding the game's stone, gray 200, and a version.json.
std::string client_jar() {
    const std::vector<vx::test::ZipEntry> entries = {
        {.name = "version.json", .data = R"({"id": "26.2"})"},
        {.name = "assets/minecraft/textures/block/stone.png", .data = gray_png('\xC8')}};
    const vx::test::Zip zip = vx::test::make_zip(entries);
    return {reinterpret_cast<const char*>(zip.bytes.data()), zip.bytes.size()};
}

std::string listed(const std::vector<vx::Diagnostic>& diagnostics) {
    std::string text;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        text += vx::format(diagnostic) + "\n";
    }
    return text;
}

// Level 0's first texel of layer 1, the pack's one texture, as {r, a}.
std::vector<int> rock_texel(const vx::app::StartupPack& startup) {
    const std::vector<std::byte>& bytes = startup.pack.textures;
    REQUIRE(bytes.size() == 2 * vx::render::block_texture_layer_bytes);
    const std::size_t at = 4 * 256; // after the checker's level 0
    return {static_cast<int>(bytes[at]), static_cast<int>(bytes[at + 3])};
}

} // namespace

TEST_CASE("with no Minecraft found, the pack draws its own textures, and the report says why") {
    const TemporaryFolder folder;
    write_pack(folder);
    // A home with nothing in it, as on a machine without the game.
    fs::create_directories(folder.root() / "home");
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack", {.use = true, .query = {.home = folder.root() / "home"}});
    INFO(listed(startup.pack.diagnostics));
    CHECK(startup.pack.diagnostics.empty());
    CHECK(startup.textures.starts_with("Textures: the pack's own; no Minecraft "));
    CHECK(startup.textures.ends_with(" (--minecraft=PATH names one)"));
    CHECK(rock_texel(startup) == std::vector<int>{50, 255});
}

TEST_CASE("--no-minecraft draws the pack's own textures whatever is installed") {
    const TemporaryFolder folder;
    write_pack(folder);
    folder.write("client.jar", client_jar());
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack", {.use = false, .query = {.path = folder.root() / "client.jar"}});
    CHECK(startup.pack.diagnostics.empty());
    CHECK(startup.textures == "Textures: the pack's own (--no-minecraft)");
    CHECK(rock_texel(startup) == std::vector<int>{50, 255});
}

TEST_CASE("a Minecraft named on the command line that cannot be used is an error") {
    const TemporaryFolder folder;
    write_pack(folder);
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack", {.use = true, .query = {.path = folder.root() / "absent.jar"}});
    REQUIRE(startup.named_game.has_value());
    CHECK(startup.named_game->message.find("absent.jar") != std::string::npos);
    // The pack is loaded all the same, with its own textures.
    CHECK(startup.pack.diagnostics.empty());
    CHECK(rock_texel(startup) == std::vector<int>{50, 255});
}

TEST_CASE("a named version that cannot be used still lets every other problem be listed") {
    const TemporaryFolder folder;
    write_pack(folder);
    // A block file with a misspelt key, and a game folder without that version.
    folder.write("pack/assets/t/blocks/rock.toml", "[textures]\nal = \"rock\"\n");
    fs::create_directories(folder.root() / "game" / "versions");
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack",
        {.use = true, .query = {.path = folder.root() / "game", .version = "9.9"}});
    REQUIRE(startup.named_game.has_value());
    CHECK(startup.named_game->message.starts_with("no version 9.9 in "));
    CHECK(listed(startup.pack.diagnostics)
              .find("assets/t/blocks/rock.toml:2:1: error: unknown key 'al'") != std::string::npos);
}

TEST_CASE(
    "with Minecraft found, its textures stand in for the pack's, and the report counts them") {
    const TemporaryFolder folder;
    write_pack(folder);
    folder.write("client.jar", client_jar());
    const fs::path jar = folder.root() / "client.jar";
    const vx::app::StartupPack startup =
        vx::app::load_startup_pack(folder.root() / "pack", {.use = true, .query = {.path = jar}});
    INFO(listed(startup.pack.diagnostics));
    CHECK(startup.pack.diagnostics.empty());
    const std::u8string jar_name = jar.u8string();
    CHECK(startup.textures ==
          "Textures: Minecraft 26.2, from " +
              std::string(reinterpret_cast<const char*>(jar_name.data()), jar_name.size()) +
              ", 1 of 1");
    // The game's stone, gray 200, in place of the pack's gray 50.
    CHECK(rock_texel(startup) == std::vector<int>{200, 255});
}

TEST_CASE("every step's diagnostics reach the app, the map's first and the check's last") {
    const TemporaryFolder folder;
    // The map names game textures the jar lacks (two warnings from making
    // the textures, in the map's order) and a key no block uses (a warning
    // from checking the map against the blocks, which comes last).
    write_pack(folder, "rock = \"granite\"\nmoss = \"moss_block\"\n");
    folder.write("client.jar", client_jar());
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack", {.use = true, .query = {.path = folder.root() / "client.jar"}});
    const std::vector<vx::Diagnostic>& diagnostics = startup.pack.diagnostics;
    REQUIRE(diagnostics.size() == 3);
    CHECK(vx::format(diagnostics[0]) ==
          "assets/t/minecraft.toml:1:8: warning: Minecraft 26.2 has no block texture 'granite'; "
          "t:rock is drawn from its own file");
    CHECK(vx::format(diagnostics[1]) ==
          "assets/t/minecraft.toml:2:8: warning: Minecraft 26.2 has no block texture "
          "'moss_block'; t:moss is drawn from its own file");
    CHECK(vx::format(diagnostics[2]) ==
          "assets/t/minecraft.toml:2:1: warning: no block uses the texture t:moss");
    CHECK(startup.textures.ends_with(", 0 of 1"));
    CHECK(rock_texel(startup) == std::vector<int>{50, 255});
}

TEST_CASE("the block pack's own problems reach the app") {
    const TemporaryFolder folder;
    write_pack(folder);
    folder.write("pack/assets/t/blocks/rock.toml", "[textures]\nal = \"rock\"\n");
    const vx::app::StartupPack startup =
        vx::app::load_startup_pack(folder.root() / "pack", {.use = false, .query = {}});
    CHECK(vx::has_errors(startup.pack.diagnostics));
    CHECK(listed(startup.pack.diagnostics)
              .find("assets/t/blocks/rock.toml:2:1: error: unknown key 'al'") != std::string::npos);
}

TEST_CASE("the game search's warnings reach the app, with the report saying why none is used") {
    // A Linux home whose one release has a jar that does not open.
    const TemporaryFolder folder;
    write_pack(folder);
    folder.write("home/.minecraft/versions/26.2/26.2.json",
                 R"({"type": "release", "releaseTime": "2026-06-16T12:03:33+00:00"})");
    folder.write("home/.minecraft/versions/26.2/26.2.jar", "not a zip archive");
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack",
        {.use = true,
         .query = {.home = folder.root() / "home", .system = vx::minecraft::System::linux_os}});
    REQUIRE(startup.pack.diagnostics.size() == 1);
    CHECK(startup.pack.diagnostics[0].severity == vx::Severity::warning);
    CHECK(startup.pack.diagnostics[0].message.ends_with("; the version is passed over"));
    CHECK(startup.textures.starts_with("Textures: the pack's own; no Minecraft release in "));
    CHECK(startup.textures.ends_with(" has a jar that opens (--minecraft=PATH names one)"));
}

TEST_CASE("the report counts the textures the pack's blocks use, not every texture made") {
    // The jar makes both, and no block uses moss.
    const TemporaryFolder folder;
    write_pack(folder, "rock = \"stone\"\nmoss = \"stone\"\n");
    folder.write("client.jar", client_jar());
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack", {.use = true, .query = {.path = folder.root() / "client.jar"}});
    CHECK(startup.textures.ends_with(", 1 of 1"));
    CHECK(listed(startup.pack.diagnostics) ==
          "assets/t/minecraft.toml:2:1: warning: no block uses the texture t:moss\n");
}

TEST_CASE("a pack with no map does not look for the game") {
    const TemporaryFolder folder;
    folder.write("pack/assets/t/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    folder.write("pack/assets/t/textures/blocks/rock.png", gray_png('\x32'));
    const vx::app::StartupPack startup = vx::app::load_startup_pack(
        folder.root() / "pack", {.use = true, .query = {.home = folder.root() / "home"}});
    CHECK(startup.pack.diagnostics.empty());
    CHECK(startup.textures == "Textures: the pack's own; it has no minecraft.toml");
}

TEST_CASE("a map with an error is an error of the pack's") {
    const TemporaryFolder folder;
    write_pack(folder, "rock = 1\n");
    const vx::app::StartupPack startup =
        vx::app::load_startup_pack(folder.root() / "pack", {.use = false, .query = {}});
    CHECK(vx::has_errors(startup.pack.diagnostics));
    CHECK(vx::format(startup.pack.diagnostics.front())
              .starts_with("assets/t/minecraft.toml:1:8: error: "));
}
