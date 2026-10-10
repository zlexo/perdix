#include "../core/png_bytes.hpp"
#include "../core/zip_bytes.hpp"
#include "startup_figures.hpp"
#include "startup_pack.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/skin_file.hpp>
#include <vx/world/skin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vx::app::SkinChoice;
using vx::app::StartupFigures;
using vx::app::StartupPack;

class TemporaryFolder {
public:
    TemporaryFolder() {
        std::random_device random;
        root_ = fs::temp_directory_path() /
                ("vx-app-figures-" + std::to_string(random()) + "-" + std::to_string(random()));
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

// An opaque image of one color.
std::string solid_png(std::uint32_t width, std::uint32_t height, std::uint8_t red,
                      std::uint8_t green) {
    std::string rgba;
    for (std::uint32_t texel = 0; texel < width * height; ++texel) {
        rgba += static_cast<char>(red);
        rgba += static_cast<char>(green);
        rgba += '\0';
        rgba += '\xFF';
    }
    return vx::test::make_png(width, height, vx::test::PngColor::rgba, rgba);
}

// A pack with Perdix's own skin, red, and the owner's, green.
void write_pack_skins(const TemporaryFolder& folder) {
    folder.write("pack/assets/core/textures/entity/player/perdix.png", solid_png(64, 64, 255, 0));
    folder.write("pack/assets/core/textures/entity/player/zlex.png", solid_png(64, 64, 0, 255));
}

// A client jar with the game's eighteen default skins, but `left_out`, and
// its font, an atlas of one color.
StartupPack::Game write_jar(const TemporaryFolder& folder, std::string_view left_out = "") {
    std::vector<vx::test::ZipEntry> entries = {
        {.name = "version.json", .data = R"({"id": "26.2"})"},
        {.name = "assets/minecraft/textures/font/ascii.png",
         .data = solid_png(128, 128, 255, 255)}};
    for (const std::string_view arms : {"slim", "wide"}) {
        for (const std::string_view name : vx::minecraft::default_skin_names) {
            const std::string entry = "assets/minecraft/textures/entity/player/" +
                                      std::string(arms) + "/" + std::string(name) + ".png";
            if (entry.ends_with(std::string(left_out)) && !left_out.empty()) {
                continue;
            }
            entries.push_back({.name = entry, .data = solid_png(64, 64, 10, 20)});
        }
    }
    const vx::test::Zip zip = vx::test::make_zip(entries);
    folder.write("client.jar",
                 std::string(reinterpret_cast<const char*>(zip.bytes.data()), zip.bytes.size()));
    return {.version = "26.2", .jar = folder.root() / "client.jar"};
}

StartupFigures load(const TemporaryFolder& folder, const std::optional<StartupPack::Game>& game,
                    const std::string& yours = "zlex", const std::string& perdix = "perdix") {
    return vx::app::load_startup_figures(folder.root() / "pack", game,
                                         vx::app::parse_skin_choice(yours),
                                         vx::app::parse_skin_choice(perdix));
}

std::string listed(const std::vector<vx::Diagnostic>& diagnostics) {
    std::string text;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        text += vx::format(diagnostic) + "\n";
    }
    return text;
}

std::size_t index_of(const StartupFigures& figures, const std::string& name) {
    return static_cast<std::size_t>(std::ranges::find(figures.names, name) - figures.names.begin());
}

} // namespace

TEST_CASE("without the game Perdix wears his own skin and you the owner's, and no font") {
    const TemporaryFolder folder;
    write_pack_skins(folder);
    const StartupFigures figures = load(folder, std::nullopt);
    INFO(listed(figures.diagnostics));
    CHECK(figures.diagnostics.empty());
    REQUIRE(figures.skins.size() == 2);
    CHECK(figures.names == std::vector<std::string>{"perdix", "zlex"});
    CHECK(figures.perdix == 0);
    CHECK(figures.yours == 1);
    CHECK(figures.skins[0].arms == vx::Arms::wide);
    CHECK(figures.skins[1].arms == vx::Arms::slim);
    CHECK(figures.skins[0].rgba[0] == 255); // red
    CHECK(figures.skins[1].rgba[1] == 255); // green
    CHECK_FALSE(figures.font.has_value());
    CHECK(figures.skins_line == "Skins: perdix (Perdix), zlex (you)");
    CHECK(figures.font_line == "Font: none, so no name tags without the player's Minecraft");
}

TEST_CASE("with the game come its eighteen skins and its font") {
    const TemporaryFolder folder;
    write_pack_skins(folder);
    const StartupFigures figures = load(folder, write_jar(folder));
    INFO(listed(figures.diagnostics));
    CHECK(figures.diagnostics.empty());
    REQUIRE(figures.skins.size() == 20);
    CHECK(figures.names.at(2) == "alex (slim)");
    CHECK(figures.names.at(19) == "zuri (wide)");
    CHECK(figures.skins.at(2).arms == vx::Arms::slim);
    CHECK(figures.skins.at(19).arms == vx::Arms::wide);
    REQUIRE(figures.font.has_value());
    CHECK(figures.font_line.starts_with("Font: Minecraft 26.2, from "));
    CHECK(figures.skins_line == "Skins: perdix (Perdix), zlex (you), 18 of Minecraft 26.2's");
}

TEST_CASE("the game's skins are chosen by name and arms and fall back without the game") {
    const TemporaryFolder folder;
    write_pack_skins(folder);
    const StartupFigures with_game = load(folder, write_jar(folder), "steve:slim", "kai");
    CHECK(with_game.names.at(with_game.yours) == "steve (slim)");
    CHECK(with_game.names.at(with_game.perdix) == "kai (wide)");
    CHECK(with_game.diagnostics.empty());

    const StartupFigures without = load(folder, std::nullopt, "steve:slim");
    CHECK(without.yours == 0); // Perdix's own
    REQUIRE(without.diagnostics.size() == 1);
    CHECK(without.diagnostics[0].severity == vx::Severity::warning);
    CHECK(without.diagnostics[0].file == "--skin");
    CHECK(without.diagnostics[0].message.find("'steve' is one of the game's skins") !=
          std::string::npos);
}

TEST_CASE("a skin file named is added and one that cannot be read falls back") {
    const TemporaryFolder folder;
    write_pack_skins(folder);
    // A legacy 64 by 32 skin, made square the game's way.
    folder.write("mine.png", solid_png(64, 32, 1, 2));
    const std::string path = (folder.root() / "mine.png").string();
    const StartupFigures added = load(folder, std::nullopt, path + ":slim");
    REQUIRE(added.skins.size() == 3);
    CHECK(added.yours == 2);
    CHECK(added.names.at(2) == path);
    CHECK(added.skins.at(2).arms == vx::Arms::slim);

    const StartupFigures missing = load(folder, std::nullopt, path + ".gone");
    CHECK(missing.yours == 0);
    REQUIRE(missing.diagnostics.size() == 1);
    CHECK(missing.diagnostics[0].message.find("cannot be read") != std::string::npos);
}

TEST_CASE("a pack skin on the other arms is one more skin") {
    const TemporaryFolder folder;
    write_pack_skins(folder);
    const StartupFigures figures = load(folder, std::nullopt, "zlex", "zlex:wide");
    REQUIRE(figures.skins.size() == 3);
    CHECK(figures.perdix == 2);
    CHECK(figures.names.at(2) == "zlex (wide)");
    CHECK(figures.skins.at(2).arms == vx::Arms::wide);
    CHECK(figures.skins.at(2).rgba == figures.skins.at(1).rgba);
    CHECK(figures.yours == 1);
}

TEST_CASE("a default skin the jar lacks is a warning and is left out") {
    const TemporaryFolder folder;
    write_pack_skins(folder);
    const StartupFigures figures = load(folder, write_jar(folder, "slim/steve.png"), "steve:slim");
    CHECK(figures.skins.size() == 19);
    CHECK(index_of(figures, "steve (slim)") == figures.names.size());
    // The skin's own warning, then the choice falling back.
    REQUIRE(figures.diagnostics.size() == 2);
    CHECK(figures.diagnostics[0].message.find("slim/steve.png") != std::string::npos);
    CHECK(figures.diagnostics[1].file == "--skin");
    CHECK(figures.yours == 0);
}

TEST_CASE("without Perdix's own skin a fallback draws the missing texture") {
    const TemporaryFolder folder;
    folder.write("pack/assets/core/textures/entity/player/zlex.png", solid_png(64, 64, 0, 255));
    const StartupFigures figures = load(folder, std::nullopt);
    REQUIRE(figures.skins.size() == 1);
    CHECK(figures.yours == 0); // the owner's, the one skin there is
    CHECK(figures.perdix >= figures.skins.size());
    CHECK(figures.skins_line == "Skins: none (Perdix), zlex (you)");
    REQUIRE_FALSE(figures.diagnostics.empty());
    CHECK(figures.diagnostics[0].file == "assets/core/textures/entity/player/perdix.png");
}

TEST_CASE("a skin choice is a name with slim or wide arms after a colon") {
    CHECK(vx::app::parse_skin_choice("steve:slim").name == "steve");
    CHECK(vx::app::parse_skin_choice("steve:slim").arms == vx::Arms::slim);
    CHECK(vx::app::parse_skin_choice("alex:wide").arms == vx::Arms::wide);
    CHECK_FALSE(vx::app::parse_skin_choice("zlex").arms.has_value());
    CHECK(vx::app::parse_skin_choice("C:/skins/a.png:slim").name == "C:/skins/a.png");
    CHECK(vx::app::parse_skin_choice(":slim").name == ":slim");
}
