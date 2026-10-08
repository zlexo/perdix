#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/texture_map.hpp>

#include <doctest/doctest.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

// A pack in a folder of its own under the system's temporary folder,
// removed when the test ends.
class TemporaryPack {
public:
    TemporaryPack() {
        std::random_device random;
        root_ = fs::temp_directory_path() /
                ("vx-minecraft-maps-" + std::to_string(random()) + "-" + std::to_string(random()));
        fs::create_directories(root_);
    }
    TemporaryPack(const TemporaryPack&) = delete;
    TemporaryPack& operator=(const TemporaryPack&) = delete;
    TemporaryPack(TemporaryPack&&) = delete;
    TemporaryPack& operator=(TemporaryPack&&) = delete;
    ~TemporaryPack() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    [[nodiscard]] const fs::path& root() const { return root_; }

    // Writes `text` to the file at `relative`, a path under the root.
    void write(const std::string& relative, const std::string& text) const {
        const fs::path path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

private:
    fs::path root_;
};

std::vector<std::string> formatted(const std::vector<vx::Diagnostic>& diagnostics) {
    std::vector<std::string> lines;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        lines.push_back(vx::format(diagnostic));
    }
    return lines;
}

} // namespace

TEST_CASE("a pack's maps are its namespaces' minecraft.toml files, read and checked") {
    const TemporaryPack pack;
    pack.write("assets/c/minecraft.toml", "dirt = \"dirt\"\n");
    pack.write("assets/a/minecraft.toml", "stone = \"stone\"\n");
    // A namespace without a map, a hidden folder, and a file beside the
    // namespaces.
    pack.write("assets/b/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    pack.write("assets/.cache/minecraft.toml", "not TOML at all");
    pack.write("assets/LICENSE", "all rights reserved");
    const vx::minecraft::TextureMapsLoad load = vx::minecraft::load_texture_maps(pack.root());
    CHECK(formatted(load.diagnostics).empty());
    REQUIRE(load.maps.size() == 2);
    CHECK(load.maps[0].namespace_name == "a");
    CHECK(load.maps[0].path == "assets/a/minecraft.toml");
    CHECK(load.maps[0].textures.contains("stone"));
    CHECK(load.maps[1].namespace_name == "c");
    CHECK(load.maps[1].textures.contains("dirt"));
}

TEST_CASE("a map with an error is left out, named under the pack, and the others still read") {
    const TemporaryPack pack;
    pack.write("assets/a/minecraft.toml", "stone = 1\n");
    pack.write("assets/b/minecraft.toml", "dirt = \"dirt\"\n");
    const vx::minecraft::TextureMapsLoad load = vx::minecraft::load_texture_maps(pack.root());
    REQUIRE(load.maps.size() == 1);
    CHECK(load.maps[0].namespace_name == "b");
    CHECK(
        formatted(load.diagnostics) ==
        std::vector<std::string>{
            R"(assets/a/minecraft.toml:1:9: error: a texture is a name in quotes, such as "stone", a table such as { texture = "grass_block_top", tint = "grass" }, or a list of them)"});
}

TEST_CASE("a map named in another case, or one that cannot be read, is an error") {
    // The Mac's file system would open "Minecraft.toml" by the name
    // "minecraft.toml", and Linux's would not: names match exactly.
    const TemporaryPack pack;
    pack.write("assets/a/Minecraft.toml", "stone = \"stone\"\n");
    fs::create_directories(pack.root() / "assets" / "b" / "minecraft.toml");
    const vx::minecraft::TextureMapsLoad load = vx::minecraft::load_texture_maps(pack.root());
    CHECK(load.maps.empty());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{"assets/a/Minecraft.toml: error: 'Minecraft.toml' must be named "
                                   "'minecraft.toml': names in a pack match exactly, as on Linux",
                                   "assets/b/minecraft.toml: error: the file cannot be read"});
}

TEST_CASE("a pack without an assets folder has no maps, and leaves saying so to its blocks") {
    const TemporaryPack pack;
    const vx::minecraft::TextureMapsLoad load = vx::minecraft::load_texture_maps(pack.root());
    CHECK(load.maps.empty());
    CHECK(load.diagnostics.empty());
    // Nor one whose assets folder is named in another case, which the Mac
    // would open by the name assets.
    pack.write("Assets/t/minecraft.toml", "stone = \"stone\"\n");
    const vx::minecraft::TextureMapsLoad other = vx::minecraft::load_texture_maps(pack.root());
    CHECK(other.maps.empty());
    CHECK(other.diagnostics.empty());
}

#ifndef _WIN32
TEST_CASE("a map that is not a regular file is not read") {
    // A pipe would block the read forever, and a link to a device such as
    // /dev/zero would be read until memory ran out.
    const TemporaryPack pack;
    fs::create_directories(pack.root() / "assets" / "p");
    REQUIRE(mkfifo((pack.root() / "assets" / "p" / "minecraft.toml").c_str(), 0600) == 0);
    fs::create_directories(pack.root() / "assets" / "z");
    fs::create_symlink("/dev/zero", pack.root() / "assets" / "z" / "minecraft.toml");
    const vx::minecraft::TextureMapsLoad load = vx::minecraft::load_texture_maps(pack.root());
    CHECK(load.maps.empty());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{"assets/p/minecraft.toml: error: the file cannot be read",
                                   "assets/z/minecraft.toml: error: the file cannot be read"});
}
#endif
