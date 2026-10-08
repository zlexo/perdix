#include <vx/core/diagnostic.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr vx::BlockState state(int id) {
    return static_cast<vx::BlockState>(static_cast<std::uint16_t>(id));
}

// A pack in a folder of its own under the system's temporary folder,
// removed when the test ends.
class TempPack {
public:
    TempPack() {
        std::random_device random;
        root_ = std::filesystem::temp_directory_path() /
                ("vx-pack-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::filesystem::create_directories(root_);
    }
    TempPack(const TempPack&) = delete;
    TempPack& operator=(const TempPack&) = delete;
    TempPack(TempPack&&) = delete;
    TempPack& operator=(TempPack&&) = delete;
    ~TempPack() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    void write(const std::string& relative, const std::string& text) const {
        const std::filesystem::path path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    void folder(const std::string& relative) const {
        std::filesystem::create_directories(root_ / relative);
    }

    // A link at `relative` to `target`, which need not exist.
    void link(const std::string& relative, const std::string& target) const {
        const std::filesystem::path path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::create_symlink(target, path);
    }

    // A block of the namespace core, and its texture.
    void stone() const {
        write("assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
        write("assets/core/textures/blocks/stone.png", "a picture");
    }

private:
    std::filesystem::path root_;
};

std::vector<std::string> formatted(const std::vector<vx::Diagnostic>& diagnostics) {
    std::vector<std::string> lines;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        lines.push_back(vx::format(diagnostic));
    }
    return lines;
}

} // namespace

TEST_CASE("a pack on disk loads the block files of every namespace, and nothing else") {
    const TempPack pack;
    pack.write("assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
    pack.write("assets/core/blocks/dirt.toml", "[textures]\nall = \"dirt\"\n");
    pack.write("assets/mod/blocks/thing.toml", "[textures]\nall = \"thing\"\n");
    // Neither is a block file.
    pack.write("assets/core/blocks/notes.txt", "stone is grey\n");
    pack.write("assets/core/blocks/.DS_Store", "Bud1");
    for (const char* const texture :
         {"assets/core/textures/blocks/stone.png", "assets/core/textures/blocks/dirt.png",
          "assets/mod/textures/blocks/thing.png"}) {
        pack.write(texture, "a picture");
    }

    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK(formatted(load.diagnostics).empty());
    REQUIRE(load.registry.has_value());
    CHECK(load.registry->state_count() == 4);
    CHECK(load.registry->find("core:dirt") == state(1));
    CHECK(load.registry->find("core:stone") == state(2));
    CHECK(load.registry->find("mod:thing") == state(3));
}

TEST_CASE("a texture file that no block uses is a warning on its path, and the pack still loads") {
    const TempPack pack;
    pack.write("assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
    pack.write("assets/core/textures/blocks/stone.png", "a picture");
    pack.write("assets/core/textures/blocks/moss.png", "a picture");
    pack.write("assets/core/textures/blocks/moss.txt", "not a texture file");

    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{
              "assets/core/textures/blocks/moss.png: warning: no block uses this texture"});
}

TEST_CASE("a block file's problems are placed in it by its path within the pack") {
    const TempPack pack;
    pack.write("assets/core/blocks/glass.toml",
               "opacty = \"cutout\"\n[textures]\nall = \"glass\"\n");

    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{"assets/core/blocks/glass.toml:1:1: error: unknown key "
                                   "'opacty'; did you mean 'opacity'?"});
}

TEST_CASE("a pack with no assets folder, or nothing in it, is an error") {
    const TempPack pack;
    const std::string assets = (pack.root() / "assets").generic_string();
    const vx::BlockLoad missing = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(missing.registry.has_value());
    CHECK(formatted(missing.diagnostics) ==
          std::vector<std::string>{assets + ": error: no such folder, or it holds no namespace"});

    std::filesystem::create_directories(pack.root() / "assets");
    const vx::BlockLoad empty = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(empty.registry.has_value());
    CHECK(formatted(empty.diagnostics) == formatted(missing.diagnostics));
}

TEST_CASE("a block file that cannot be read is an error on its path") {
    const TempPack pack;
    pack.write("assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
    const std::filesystem::path file = pack.root() / "assets/core/blocks/stone.toml";
    std::filesystem::permissions(file, std::filesystem::perms::none);
    // Running as root would read it anyway, and this test would test nothing.
    REQUIRE_FALSE(std::ifstream(file).good());

    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(load.registry.has_value());
    CHECK(
        formatted(load.diagnostics) ==
        std::vector<std::string>{"assets/core/blocks/stone.toml: error: the file cannot be read"});
    std::filesystem::permissions(file, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
}

TEST_CASE("a pack's folder and file names match exactly, as on Linux, whatever the file system") {
    // The Mac's file system would find each of these by the right name, and
    // Linux's would not: it is an error on both.
    const TempPack pack;
    SUBCASE("assets") {
        pack.write("Assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
        const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
        CHECK_FALSE(load.registry.has_value());
        CHECK(formatted(load.diagnostics) ==
              std::vector<std::string>{"Assets: error: 'Assets' must be named 'assets': names in a "
                                       "pack match exactly, as on Linux"});
    }
    SUBCASE("blocks") {
        pack.write("assets/core/Blocks/stone.toml", "[textures]\nall = \"stone\"\n");
        pack.write("assets/core/textures/blocks/stone.png", "a picture");
        const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
        CHECK_FALSE(load.registry.has_value());
        CHECK(
            formatted(load.diagnostics) ==
            std::vector<std::string>{"assets/core/Blocks: error: 'Blocks' must be named 'blocks': "
                                     "names in a pack match exactly, as on Linux"});
    }
    SUBCASE("textures") {
        pack.write("assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
        pack.write("assets/core/Textures/blocks/stone.png", "a picture");
        const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
        CHECK_FALSE(load.registry.has_value());
        CHECK(formatted(load.diagnostics) ==
              std::vector<std::string>{"assets/core/Textures: error: 'Textures' must be named "
                                       "'textures': names in a pack match exactly, as on Linux"});
    }
    SUBCASE("endings") {
        pack.stone();
        pack.write("assets/core/blocks/dirt.TOML", "[textures]\nall = \"stone\"\n");
        pack.write("assets/core/textures/blocks/moss.PNG", "a picture");
        const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
        CHECK_FALSE(load.registry.has_value());
        CHECK(formatted(load.diagnostics) ==
              std::vector<std::string>{
                  "assets/core/blocks/dirt.TOML: error: block files end in '.toml'",
                  "assets/core/textures/blocks/moss.PNG: error: texture files end in '.png'"});
    }
}

TEST_CASE("a textures folder that cannot be read is an error, and leaves no registry") {
    const TempPack pack;
    pack.write("assets/core/blocks/stone.toml", "[textures]\nall = \"stone\"\n");
    pack.write("assets/core/textures/blocks", "a file where a folder should be");
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{"assets/core/textures/blocks: error: not a folder"});
}

TEST_CASE("a blocks folder that is a file is an error") {
    const TempPack pack;
    pack.write("assets/core/blocks", "a file where a folder should be");
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{"assets/core/blocks: error: not a folder"});
}

TEST_CASE("an assets folder that is a file, or holds only files, has no namespace") {
    const TempPack pack;
    SUBCASE("a file") {
        pack.write("assets", "a file where a folder should be");
        CHECK(formatted(vx::BlockRegistry::load(pack.root()).diagnostics) ==
              std::vector<std::string>{"assets: error: not a folder"});
    }
    SUBCASE("only files") {
        pack.write("assets/.DS_Store", "Bud1");
        pack.write("assets/readme.txt", "blocks go in a namespace");
        CHECK(formatted(vx::BlockRegistry::load(pack.root()).diagnostics) ==
              std::vector<std::string>{(pack.root() / "assets").generic_string() +
                                       ": error: no such folder, or it holds no namespace"});
    }
}

TEST_CASE("a pack with no block file is an error") {
    const TempPack pack;
    // A misspelt folder, and textures alone.
    pack.write("assets/core/block/stone.toml", "[textures]\nall = \"stone\"\n");
    pack.write("assets/core/textures/blocks/stone.png", "a picture");
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{(pack.root() / "assets").generic_string() +
                                   ": error: no block files: a block is "
                                   "assets/<namespace>/blocks/<name>.toml"});
}

TEST_CASE("folders named like block or texture files, and hidden files, are not read") {
    const TempPack pack;
    pack.stone();
    pack.folder("assets/core/blocks/heap.toml");
    pack.folder("assets/core/textures/blocks/heap.png");
    // macOS leaves ._ files beside files copied to other file systems.
    pack.write("assets/core/blocks/._stone.toml", "\x00\x05\x16\x07");
    pack.write("assets/core/textures/blocks/._stone.png", "\x00\x05\x16\x07");
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK(formatted(load.diagnostics).empty());
    REQUIRE(load.registry.has_value());
    CHECK(load.registry->state_count() == 2);
}

TEST_CASE("a link to nothing where a block file should be is an error, not a gap") {
    const TempPack pack;
    pack.stone();
    pack.link("assets/core/blocks/dirt.toml", "nowhere.toml");
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{"assets/core/blocks/dirt.toml: error: the file cannot be read"});
}

TEST_CASE("a block file that cannot be read does not hide the problems of the others") {
    const TempPack pack;
    pack.link("assets/core/blocks/a.toml", "nowhere.toml");
    pack.write("assets/core/blocks/b.toml", "opacty = \"cutout\"\n[textures]\nall = \"b\"\n");
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK_FALSE(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{
              "assets/core/blocks/a.toml: error: the file cannot be read",
              "assets/core/blocks/b.toml:1:1: error: unknown key 'opacty'; did you mean "
              "'opacity'?"});
}

TEST_CASE("a texture file is used only by blocks that name it in its own namespace") {
    const TempPack pack;
    pack.stone();
    // mod has textures and no blocks; its stone is not core's.
    pack.write("assets/mod/textures/blocks/stone.png", "a picture");
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    CHECK(load.registry.has_value());
    CHECK(formatted(load.diagnostics) ==
          std::vector<std::string>{
              "assets/mod/textures/blocks/stone.png: warning: no block uses this texture"});
}

TEST_CASE("texture files no block uses are warned about in name order") {
    const TempPack pack;
    pack.stone();
    for (const char* const name : {"moss", "clay", "amber", "zinc", "birch"}) {
        pack.write(std::string("assets/core/textures/blocks/") + name + ".png", "a picture");
    }
    const vx::BlockLoad load = vx::BlockRegistry::load(pack.root());
    std::vector<std::string> expected;
    for (const char* const name : {"amber", "birch", "clay", "moss", "zinc"}) {
        expected.push_back(std::string("assets/core/textures/blocks/") + name +
                           ".png: warning: no block uses this texture");
    }
    CHECK(formatted(load.diagnostics) == expected);
}
