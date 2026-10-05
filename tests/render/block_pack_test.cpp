#include <vx/core/diagnostic.hpp>
#include <vx/render/block_pack.hpp>
#include <vx/render/block_textures.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

namespace {

// A pack in a folder of its own under the system's temporary folder,
// removed when the test ends.
class TempPack {
public:
    TempPack() {
        std::random_device random;
        root_ = std::filesystem::temp_directory_path() /
                ("vx-render-pack-" + std::to_string(random()) + "-" + std::to_string(random()));
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

    // Writes `text` to the file at `relative`, a path under the root.
    void write(const std::string& relative, const std::string& text) const {
        const std::filesystem::path path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    // Copies one of the texture tests' files to `relative`.
    void copy_texture(const std::string& name, const std::string& relative) const {
        const std::filesystem::path path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::copy_file(std::filesystem::path(VX_BLOCK_TEXTURES_DIR) / "test" /
                                       "textures" / "blocks" / (name + ".png"),
                                   path);
    }

private:
    std::filesystem::path root_;
};

} // namespace

TEST_CASE("a pack loads its blocks and builds every layer of its textures") {
    const TempPack pack;
    pack.write("assets/t/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    pack.copy_texture("solid", "assets/t/textures/blocks/rock.png");
    const vx::render::BlockPack loaded = vx::render::load_block_pack(pack.root());
    std::string problems;
    for (const vx::Diagnostic& diagnostic : loaded.diagnostics) {
        problems += vx::format(diagnostic) + "\n";
    }
    INFO(problems);
    CHECK(loaded.diagnostics.empty());
    REQUIRE(loaded.registry.has_value());
    // The checker and the rock.
    REQUIRE(loaded.registry->textures().size() == 2);
    CHECK(loaded.textures.size() == 2 * vx::render::block_texture_layer_bytes);
}

TEST_CASE("a pack whose texture is missing keeps its registry, has no bytes, and says where") {
    const TempPack pack;
    pack.write("assets/t/blocks/rock.toml", "[textures]\nall = \"rock\"\n");
    const vx::render::BlockPack loaded = vx::render::load_block_pack(pack.root());
    CHECK(loaded.registry.has_value());
    CHECK(loaded.textures.empty());
    REQUIRE(loaded.diagnostics.size() == 1);
    CHECK(loaded.diagnostics[0].severity == vx::Severity::error);
    // Relative to the root, as a block file is named.
    CHECK(loaded.diagnostics[0].file == "assets/t/textures/blocks/rock.png");
    CHECK(loaded.diagnostics[0].message.find("no such file") != std::string::npos);
}

TEST_CASE("a pack whose block file has an error builds no textures") {
    const TempPack pack;
    pack.write("assets/t/blocks/rock.toml", "[textures]\nal = \"rock\"\n");
    const vx::render::BlockPack loaded = vx::render::load_block_pack(pack.root());
    CHECK_FALSE(loaded.registry.has_value());
    CHECK(loaded.textures.empty());
    REQUIRE_FALSE(loaded.diagnostics.empty());
    // The block file's problems alone: no texture was looked for.
    for (const vx::Diagnostic& diagnostic : loaded.diagnostics) {
        CHECK(diagnostic.file == "assets/t/blocks/rock.toml");
    }
}
