#include "../core/png_bytes.hpp"
#include "../core/zip_bytes.hpp"
#include "player_minecraft.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/core/file.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/minecraft/made_textures.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/render/block_pack.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/world/block_registry.hpp>

#include <doctest/doctest.h>
#include <stb_image.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vx::render::BlockImage;
using vx::render::Texel;
using vx::test::PngColor;

// How the game's 26.2 jar stores each of the textures below, and so how the
// synthetic jar stores them: 8-bit gray, gray with 0 made transparent by a
// tRNS chunk, a palette of 4 or 8 bits an index, or RGBA.
enum class Kind : std::uint8_t { gray, gray_holes, palette_4, palette_8, rgba };

// One of the game's block textures, as the synthetic jar holds it, every
// texel worked out from (x, y) by `texel`, which is also what the test
// expects of it.
struct GameTexture {
    std::string name;
    Kind kind;
    std::function<Texel(std::size_t, std::size_t)> texel;
};

std::uint8_t byte(std::size_t value) {
    return static_cast<std::uint8_t>(value & 0xFFU);
}

Texel gray(std::size_t value) {
    return {.r = byte(value), .g = byte(value), .b = byte(value), .a = 255};
}

// A texture with every texel its own: the index sets it apart from the
// others.
Texel rgb(std::size_t index, std::size_t x, std::size_t y) {
    return {.r = byte((20 * index) + x),
            .g = byte(10 + (15 * y)),
            .b = byte(255 - (9 * index)),
            .a = 255};
}

// Every block texture the core pack's map names, each in the kind of PNG the
// game's 26.2 jar uses for it. A 4-bit palette holds 16 colors, so those
// textures take one of 16 by (x + 3y) mod 16; the leaves take 15 and a
// transparent entry.
std::vector<GameTexture> game_textures() {
    const auto all_colors = [](std::size_t index) {
        return [index](std::size_t x, std::size_t y) { return rgb(index, x, y); };
    };
    const auto sixteen_colors = [](std::size_t index) {
        return [index](std::size_t x, std::size_t y) { return rgb(index, (x + (3 * y)) % 16, 0); };
    };
    return {
        {.name = "stone",
         .kind = Kind::gray,
         .texel = [](std::size_t x, std::size_t y) { return gray(30 + x + y); }},
        {.name = "cobblestone", .kind = Kind::palette_4, .texel = sixteen_colors(1)},
        {.name = "dirt", .kind = Kind::palette_4, .texel = sixteen_colors(2)},
        {.name = "sand", .kind = Kind::palette_4, .texel = sixteen_colors(3)},
        {.name = "grass_block_top",
         .kind = Kind::gray,
         .texel = [](std::size_t x, std::size_t y) { return gray(150 + x + y); }},
        {.name = "grass_block_side", .kind = Kind::palette_8, .texel = all_colors(5)},
        {.name = "grass_block_side_overlay",
         .kind = Kind::gray_holes,
         .texel = [](std::size_t x, std::size_t y) { return y < 5 ? gray(120 + x) : Texel{}; }},
        {.name = "oak_log", .kind = Kind::palette_4, .texel = sixteen_colors(7)},
        {.name = "oak_log_top", .kind = Kind::palette_8, .texel = all_colors(8)},
        {.name = "oak_planks", .kind = Kind::palette_8, .texel = all_colors(9)},
        {.name = "oak_leaves",
         .kind = Kind::palette_4,
         .texel =
             [](std::size_t x, std::size_t y) {
                 return (x + y) % 3 == 0 ? Texel{} : rgb(10, (x + (3 * y)) % 15, 0);
             }},
        {.name = "glass",
         .kind = Kind::rgba,
         .texel =
             [](std::size_t x, std::size_t y) {
                 Texel texel = rgb(11, x, y);
                 texel.a = (x + y) % 3 == 0 ? 0 : 255;
                 return texel;
             }},
    };
}

std::string png_of(const GameTexture& texture) {
    std::string gray_samples;
    std::string rgba;
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            const Texel texel = texture.texel(x, y);
            gray_samples.push_back(static_cast<char>(texel.r));
            rgba += {static_cast<char>(texel.r), static_cast<char>(texel.g),
                     static_cast<char>(texel.b), static_cast<char>(texel.a)};
        }
    }
    switch (texture.kind) {
    case Kind::gray:
        return vx::test::make_png(16, 16, PngColor::gray, gray_samples);
    case Kind::gray_holes:
        return vx::test::make_png(16, 16, PngColor::gray, gray_samples, 0);
    case Kind::palette_4:
        return vx::test::make_palette_png(16, 16, 4, rgba);
    case Kind::palette_8:
        return vx::test::make_palette_png(16, 16, 8, rgba);
    case Kind::rgba:
        return vx::test::make_png(16, 16, PngColor::rgba, rgba);
    }
    return {};
}

// Two color maps: grass's texel (x, y) is (x, y, 7), foliage's (y, x, 9).
std::string color_map(bool swapped, char blue) {
    std::string samples;
    for (std::size_t y = 0; y < 256; ++y) {
        for (std::size_t x = 0; x < 256; ++x) {
            samples +=
                {static_cast<char>(swapped ? y : x), static_cast<char>(swapped ? x : y), blue};
        }
    }
    return vx::test::make_png(256, 256, PngColor::rgb, samples);
}

// A client jar laid out as the game's: deflated entries with data
// descriptors, and a version.json naming 26.2. One texture may be left out.
std::string client_jar(std::string_view without = {}) {
    std::vector<vx::test::ZipEntry> entries = {
        {.name = "version.json", .data = R"({"id": "26.2", "name": "26.2"})"},
        {.name = "assets/minecraft/textures/colormap/grass.png", .data = color_map(false, '\x07')},
        {.name = "assets/minecraft/textures/colormap/foliage.png", .data = color_map(true, '\x09')},
    };
    for (const GameTexture& texture : game_textures()) {
        if (texture.name != without) {
            entries.push_back({.name = "assets/minecraft/textures/block/" + texture.name + ".png",
                               .data = png_of(texture)});
        }
    }
    for (vx::test::ZipEntry& entry : entries) {
        entry.flags = 8;
        entry.deflate = true;
    }
    const vx::test::Zip zip = vx::test::make_zip(entries);
    return {reinterpret_cast<const char*>(zip.bytes.data()), zip.bytes.size()};
}

// A file of its own in the system's temporary folder, removed with the test.
class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view bytes) {
        std::random_device random;
        folder_ = fs::temp_directory_path() / ("vx-minecraft-pack-" + std::to_string(random()) +
                                               "-" + std::to_string(random()));
        fs::create_directories(folder_);
        std::ofstream(path(), std::ios::binary) << bytes;
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;
    ~TemporaryFile() {
        std::error_code error;
        fs::remove_all(folder_, error);
    }

    [[nodiscard]] fs::path path() const { return folder_ / "client.jar"; }

private:
    fs::path folder_;
};

std::string listed(const std::vector<vx::Diagnostic>& diagnostics) {
    std::string text;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        text += vx::format(diagnostic) + "\n";
    }
    return text;
}

// The core pack's textures made from a jar named on the command line, as the
// app will make them: found, mapped, made.
std::vector<vx::MadeTexture> made_from(const fs::path& jar,
                                       std::vector<vx::Diagnostic>& diagnostics) {
    vx::Result<vx::minecraft::Install> install =
        vx::minecraft::find_install({.path = jar}, diagnostics);
    REQUIRE(install.has_value());
    CHECK(install.value().version == "26.2"); // from its version.json
    const fs::path root = VX_SHIPPED_PACK_DIR;
    const std::optional<std::string> text = vx::read_file(root / "assets/core/minecraft.toml");
    REQUIRE(text.has_value());
    vx::minecraft::TextureMapLoad map =
        vx::minecraft::load_texture_map({.path = "assets/core/minecraft.toml", .text = *text});
    REQUIRE(map.map.has_value());
    return vx::minecraft::make_textures(install.value().jar, install.value().version, *map.map,
                                        diagnostics);
}

// A channel multiplied by a tint's, rounded to the nearest, worked out here
// in floating point rather than with the layer's own integers.
std::uint8_t by_hand(std::uint8_t channel, std::uint8_t tint) {
    return static_cast<std::uint8_t>(std::lround(channel * tint / 255.0));
}

Texel tinted_by_hand(Texel texel, Texel color) {
    return {.r = by_hand(texel.r, color.r),
            .g = by_hand(texel.g, color.g),
            .b = by_hand(texel.b, color.b),
            .a = texel.a};
}

// What each core texture must be, worked out from the game textures above:
// grass is (50, 173, 7) at plains, foliage (173, 50, 9).
std::map<std::string, BlockImage> expected_textures() {
    std::map<std::string, GameTexture> game;
    for (GameTexture& texture : game_textures()) {
        game.emplace(texture.name, std::move(texture));
    }
    const Texel grass{.r = 50, .g = 173, .b = 7, .a = 255};
    const Texel foliage{.r = 173, .g = 50, .b = 9, .a = 255};
    const auto image = [&](const std::function<Texel(std::size_t, std::size_t)>& texel) {
        BlockImage made{};
        for (std::size_t y = 0; y < 16; ++y) {
            for (std::size_t x = 0; x < 16; ++x) {
                made[(y * 16) + x] = texel(x, y);
            }
        }
        return made;
    };
    const auto plain = [&](const std::string& name) { return image(game.at(name).texel); };
    std::map<std::string, BlockImage> expected = {
        {"core:stone", plain("stone")},
        {"core:cobblestone", plain("cobblestone")},
        {"core:dirt", plain("dirt")},
        {"core:sand", plain("sand")},
        {"core:oak_log", plain("oak_log")},
        {"core:oak_log_top", plain("oak_log_top")},
        {"core:oak_planks", plain("oak_planks")},
        {"core:glass", plain("glass")},
    };
    expected["core:grass_top"] = image([&](std::size_t x, std::size_t y) {
        return tinted_by_hand(game.at("grass_block_top").texel(x, y), grass);
    });
    expected["core:oak_leaves"] = image([&](std::size_t x, std::size_t y) {
        return tinted_by_hand(game.at("oak_leaves").texel(x, y), foliage);
    });
    // The overlay's top five rows over the side.
    expected["core:grass_side"] = image([&](std::size_t x, std::size_t y) {
        const Texel overlay = game.at("grass_block_side_overlay").texel(x, y);
        return y < 5 ? tinted_by_hand(overlay, grass) : game.at("grass_block_side").texel(x, y);
    });
    return expected;
}

// Level 0 of layer `layer` in build_block_textures' bytes: level 0 of every
// layer comes first, one after another.
BlockImage level_0(const std::vector<std::byte>& bytes, std::size_t layer) {
    BlockImage image{};
    std::memcpy(image.data(), &bytes[layer * sizeof(image)], sizeof(image));
    return image;
}

} // namespace

TEST_CASE("the core pack draws with a synthetic client jar's textures, layer for layer") {
    const TemporaryFile jar(client_jar());
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<vx::MadeTexture> made = made_from(jar.path(), diagnostics);
    INFO(listed(diagnostics));
    CHECK(diagnostics.empty());
    CHECK(made.size() == 11);

    const vx::render::BlockPack pack = vx::render::load_block_pack(VX_SHIPPED_PACK_DIR, made);
    INFO(listed(pack.diagnostics));
    CHECK(pack.diagnostics.empty());
    REQUIRE(pack.registry.has_value());
    const std::span<const vx::TextureRef> layers = pack.registry->textures();
    REQUIRE(pack.textures.size() == layers.size() * vx::render::block_texture_layer_bytes);

    // Every layer but the checker, each face of every block drawing one:
    // the texture worked out by hand, turned and filled as RFC 0004 does.
    const std::map<std::string, BlockImage> expected = expected_textures();
    for (std::size_t layer = 1; layer < layers.size(); ++layer) {
        INFO(layers[layer].name << " turned " << int{layers[layer].quarter_turns});
        REQUIRE(expected.contains(layers[layer].name));
        BlockImage image =
            vx::render::turned(expected.at(layers[layer].name), layers[layer].quarter_turns);
        vx::render::fill_transparent(image);
        CHECK(level_0(pack.textures, layer) == image);
    }
}

TEST_CASE("a jar missing one texture leaves it to the core pack's own file, and says so") {
    const TemporaryFile jar(client_jar("grass_block_side_overlay"));
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<vx::MadeTexture> made = made_from(jar.path(), diagnostics);
    REQUIRE(diagnostics.size() == 1);
    CHECK(vx::format(diagnostics[0]) ==
          "assets/core/minecraft.toml:12:47: warning: Minecraft 26.2 has no block texture "
          "'grass_block_side_overlay'; core:grass_side is drawn from its own file");
    CHECK(made.size() == 10);

    const vx::render::BlockPack pack = vx::render::load_block_pack(VX_SHIPPED_PACK_DIR, made);
    CHECK(pack.diagnostics.empty());
    REQUIRE(pack.registry.has_value());
    const std::span<const vx::TextureRef> layers = pack.registry->textures();
    const fs::path file =
        fs::path(VX_SHIPPED_PACK_DIR) / "assets/core/textures/blocks/grass_side.png";
    const std::optional<std::string> bytes = vx::read_file(file);
    REQUIRE(bytes.has_value());
    const vx::Result<BlockImage> own =
        vx::render::decode_block_texture(std::as_bytes(std::span(*bytes)));
    REQUIRE(own.has_value());
    bool found = false;
    for (std::size_t layer = 1; layer < layers.size(); ++layer) {
        if (layers[layer].name == "core:grass_side") {
            BlockImage image = vx::render::turned(own.value(), layers[layer].quarter_turns);
            vx::render::fill_transparent(image);
            CHECK(level_0(pack.textures, layer) == image);
            found = true;
        }
    }
    CHECK(found);
}

namespace {

// An image of the player's jar, decoded apart from make_textures, by stb_image
// straight, as RGBA8 of any size.
struct Decoded {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

Decoded decode_entry(const vx::Archive& jar, const std::string& entry) {
    const vx::Result<std::vector<std::byte>> bytes = jar.read(entry);
    REQUIRE_MESSAGE(bytes.has_value(), entry);
    Decoded image;
    int channels = 0;
    stbi_uc* const pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.value().data()),
        static_cast<int>(bytes.value().size()), &image.width, &image.height, &channels, 4);
    REQUIRE_MESSAGE(pixels != nullptr, entry);
    image.rgba.assign(pixels, pixels + (static_cast<std::size_t>(image.width) *
                                        static_cast<std::size_t>(image.height) * 4));
    stbi_image_free(pixels);
    return image;
}

// A 16 by 16 texture of the jar as texels.
BlockImage block_texture(const vx::Archive& jar, const std::string& name) {
    const Decoded image = decode_entry(jar, "assets/minecraft/textures/block/" + name + ".png");
    REQUIRE(image.width == 16);
    REQUIRE(image.height == 16);
    BlockImage texels{};
    std::memcpy(texels.data(), image.rgba.data(), sizeof(texels));
    return texels;
}

// A color map's texel at plains, (50, 173), as RFC 0009 works it out.
Texel plains(const vx::Archive& jar, const std::string& color_map) {
    const Decoded image =
        decode_entry(jar, "assets/minecraft/textures/colormap/" + color_map + ".png");
    REQUIRE(image.width == 256);
    REQUIRE(image.height == 256);
    const std::size_t at = 4 * ((173 * 256) + 50);
    return {.r = image.rgba[at], .g = image.rgba[at + 1], .b = image.rgba[at + 2], .a = 255};
}

} // namespace

TEST_CASE("the core pack draws with the player's own Minecraft, when asked") {
    // RFC 0009's local check against the real game, never in CI: with the
    // variable set, finding no jar fails the test.
    if (!vx::test::player_minecraft_asked()) {
        MESSAGE(vx::test::not_asked);
        return;
    }
    const vx::minecraft::InstallQuery query = vx::test::player_query();
    std::vector<vx::Diagnostic> diagnostics;
    const vx::Result<vx::minecraft::Install> install =
        vx::minecraft::find_install(query, diagnostics);
    if (!install.has_value()) {
        FAIL(install.error().message);
    }
    const vx::Archive& jar = install.value().jar;
    vx::minecraft::TextureMapsLoad maps = vx::minecraft::load_texture_maps(VX_SHIPPED_PACK_DIR);
    REQUIRE(maps.maps.size() == 1);
    const vx::minecraft::TextureMap& map = maps.maps[0];
    const auto start = std::chrono::steady_clock::now();
    const std::vector<vx::MadeTexture> made =
        vx::minecraft::make_textures(jar, install.value().version, map, diagnostics);
    const auto took = std::chrono::steady_clock::now() - start;
    INFO(listed(diagnostics));
    CHECK(diagnostics.empty());
    CHECK(made.size() == map.textures.size()); // 11 of 11

    // Every texture worked out apart: each layer decoded by stb_image straight,
    // tinted in floating point, and drawn over the one below where it passes
    // the alpha test. The grass top and the leaves come out the color maps'
    // texels at (50, 173); the grass side, its side file with the tinted
    // overlay over it.
    const Texel grass = plains(jar, "grass");
    const Texel foliage = plains(jar, "foliage");
    for (const vx::MadeTexture& texture : made) {
        INFO(texture.name);
        const std::string key = texture.name.substr(texture.name.find(':') + 1);
        const auto& layers = map.textures.at(key).layers;
        BlockImage expected{};
        for (std::size_t index = 0; index < layers.size(); ++index) {
            BlockImage layer = block_texture(jar, layers[index].texture);
            if (const auto* const color_map = std::get_if<std::string>(&layers[index].tint)) {
                const Texel color = plains(jar, *color_map);
                for (Texel& texel : layer) {
                    texel = tinted_by_hand(texel, color);
                }
            }
            for (std::size_t texel = 0; texel < layer.size(); ++texel) {
                if (index == 0 || layer[texel].a >= 128) {
                    expected[texel] = layer[texel];
                }
            }
        }
        BlockImage actual{};
        std::memcpy(actual.data(), texture.rgba.data(), sizeof(actual));
        CHECK(actual == expected);
    }
    MESSAGE("Minecraft ", install.value().version, ": ", made.size(), " of ", map.textures.size(),
            " core textures made in ", std::chrono::duration<double, std::milli>(took).count(),
            " ms; plains grass ", int{grass.r}, " ", int{grass.g}, " ", int{grass.b}, ", foliage ",
            int{foliage.r}, " ", int{foliage.g}, " ", int{foliage.b});
}
