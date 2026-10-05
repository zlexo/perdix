#include <vx/core/diagnostic.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
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
