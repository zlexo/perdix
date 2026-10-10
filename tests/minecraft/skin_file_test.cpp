#include "../core/png_bytes.hpp"
#include "player_minecraft.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/file.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/minecraft/skin_file.hpp>
#include <vx/world/skin.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using vx::test::PngColor;

// A texel as {r, g, b, a}.
using Texel = std::array<int, 4>;

// An image's RGBA samples, from a function of (x, y) giving each texel.
template <class Color> std::string samples(int width, int height, Color color) {
    std::string out;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (const int channel : color(x, y)) {
                out.push_back(static_cast<char>(channel));
            }
        }
    }
    return out;
}

std::span<const std::byte> bytes_of(const std::string& text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

vx::Skin read(const std::string& png, vx::Arms arms = vx::Arms::wide) {
    vx::Result<vx::Skin> skin = vx::minecraft::read_skin(bytes_of(png), arms);
    if (!skin.has_value()) {
        FAIL(skin.error().message);
    }
    return std::move(skin).value();
}

Texel texel(const vx::Skin& skin, int x, int y) {
    const auto at = 4 * ((static_cast<std::size_t>(y) * 64) + static_cast<std::size_t>(x));
    return {skin.rgba[at], skin.rgba[at + 1], skin.rgba[at + 2], skin.rgba[at + 3]};
}

// Whether (x, y) is on the base layer, from RFC 0012: 0, 0 to 32, 16; 0, 16
// to 64, 32; 16, 48 to 48, 64.
bool on_base(int x, int y) {
    return (x < 32 && y < 16) || (y >= 16 && y < 32) || (x >= 16 && x < 48 && y >= 48);
}

// Compares a skin with what is expected of every texel, and names the first
// few that differ.
template <class Expected> void check_texels(const vx::Skin& skin, Expected expected) {
    int wrong = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const Texel want = expected(x, y);
            const Texel got = texel(skin, x, y);
            if (got != want && ++wrong <= 4) {
                FAIL_CHECK("texel (" << x << ", " << y << ") is " << got[0] << " " << got[1] << " "
                                     << got[2] << " " << got[3] << ", not " << want[0] << " "
                                     << want[1] << " " << want[2] << " " << want[3]);
            }
        }
    }
    CHECK(wrong == 0);
}

// A face of a box on the skin: its rectangle of texels.
struct Face {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// The faces of a 4 by 12 by 4 limb whose texels start at (u, v), from RFC
// 0012's texel layout. The right side is a right limb's outer side and a left
// limb's inner side.
struct Limb {
    Face top, bottom, right, front, left, back;
};
Limb limb(int u, int v) {
    return {.top = {.x = u + 4, .y = v, .width = 4, .height = 4},
            .bottom = {.x = u + 8, .y = v, .width = 4, .height = 4},
            .right = {.x = u, .y = v + 4, .width = 4, .height = 12},
            .front = {.x = u + 4, .y = v + 4, .width = 4, .height = 12},
            .left = {.x = u + 8, .y = v + 4, .width = 4, .height = 12},
            .back = {.x = u + 12, .y = v + 4, .width = 4, .height = 12}};
}

// A jar holding these default skins, deflated as the game's entries are:
// each a name under assets/minecraft/textures/entity/player/ and its PNG.
vx::Archive jar_of(const std::vector<std::pair<std::string, std::string>>& skins) {
    std::vector<vx::test::ZipEntry> entries;
    for (const auto& [name, png] : skins) {
        entries.push_back({.name = "assets/minecraft/textures/entity/player/" + name,
                           .data = png,
                           .flags = 8,
                           .deflate = true});
    }
    vx::Result<vx::Archive> jar =
        vx::Archive::from_bytes("26.2.jar", vx::test::make_zip(entries).bytes);
    REQUIRE(jar.has_value());
    return std::move(jar).value();
}

} // namespace

TEST_CASE("a 64 by 64 skin keeps its texels, only its base layer made opaque") {
    // Every texel's color differs, and its alpha is 0, 85, 170 or 255 in turn,
    // so the outer layers keep what they are given.
    const auto color = [](int x, int y) { return Texel{x * 4, y * 4, 7, ((x + y) % 4) * 85}; };
    const vx::Skin skin = read(vx::test::make_png(64, 64, PngColor::rgba, samples(64, 64, color)));
    check_texels(skin, [&](int x, int y) {
        Texel want = color(x, y);
        if (on_base(x, y)) {
            want[3] = 255;
        }
        return want;
    });
}

TEST_CASE("a 64 by 64 skin keeps an opaque hat") {
    // Only a legacy skin's hat is taken for empty when it is all opaque.
    const auto color = [](int x, int y) { return Texel{x * 4, y * 4, 9, 255}; };
    const vx::Skin skin = read(vx::test::make_png(64, 64, PngColor::rgba, samples(64, 64, color)));
    check_texels(skin, color);
}

TEST_CASE("a legacy skin is made square with its right limbs mirrored into the left ones") {
    // Every texel's color differs. Half of them are see-through (alpha 100),
    // so the hat is kept.
    const auto color = [](int x, int y) {
        return Texel{x * 4, y * 8, 200, (x + y) % 2 == 0 ? 100 : 255};
    };
    const vx::Skin skin = read(vx::test::make_png(64, 32, PngColor::rgba, samples(64, 32, color)));
    // The bottom half starts transparent black. Each face of the right leg
    // and arm goes to the same face of the left one, mirrored left to right,
    // the outer side to the outer side.
    std::array<Texel, std::size_t{64} * 32> bottom{};
    const std::array<std::pair<Limb, Limb>, 2> limbs = {std::pair{limb(0, 16), limb(16, 48)},
                                                        std::pair{limb(40, 16), limb(32, 48)}};
    for (const auto& [right, left] : limbs) {
        const std::array<std::pair<Face, Face>, 6> faces = {
            std::pair{right.top, left.top},    std::pair{right.bottom, left.bottom},
            std::pair{right.right, left.left}, std::pair{right.front, left.front},
            std::pair{right.left, left.right}, std::pair{right.back, left.back}};
        for (const auto& [from, to] : faces) {
            for (int y = 0; y < from.height; ++y) {
                for (int x = 0; x < from.width; ++x) {
                    const int to_x = to.x + to.width - 1 - x;
                    const auto at = (static_cast<std::size_t>(to.y + y - 32) * 64) +
                                    static_cast<std::size_t>(to_x);
                    bottom.at(at) = color(from.x + x, from.y + y);
                }
            }
        }
    }
    check_texels(skin, [&](int x, int y) {
        Texel want =
            y < 32
                ? color(x, y)
                : bottom.at((static_cast<std::size_t>(y - 32) * 64) + static_cast<std::size_t>(x));
        if (on_base(x, y)) {
            want[3] = 255;
        }
        return want;
    });
    // The left limbs' corners that no face covers are base layer: opaque black.
    CHECK(texel(skin, 16, 48) == Texel{0, 0, 0, 255});
    CHECK(texel(skin, 47, 51) == Texel{0, 0, 0, 255});
    // The left sleeve and pants are empty.
    CHECK(texel(skin, 52, 52) == Texel{0, 0, 0, 0});
    CHECK(texel(skin, 0, 63) == Texel{0, 0, 0, 0});
}

TEST_CASE("a legacy skin's hat is made transparent when its whole right half is opaque") {
    // Alpha 128 or more is opaque. The left half does not count: its texel at
    // (31, 20) is see-through until the body is made opaque, after the hat.
    const auto opaque = [](int x, int y) {
        return Texel{x * 4, y * 8, 9, x == 31 && y == 20 ? 0 : 128 + (((x + y) % 2) * 127)};
    };
    const vx::Skin cleared =
        read(vx::test::make_png(64, 32, PngColor::rgba, samples(64, 32, opaque)));
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            INFO("texel (", x, ", ", y, ")");
            // The hat loses its alpha, not its color; the rest is base.
            const bool hat = x >= 32 && y < 16;
            CHECK(texel(cleared, x, y) == Texel{x * 4, y * 8, 9, hat ? 0 : 255});
        }
    }
    // One texel of the right half below 128, here its last, keeps the hat.
    const auto seen_through = [&](int x, int y) {
        Texel color = opaque(x, y);
        if (x == 63 && y == 31) {
            color[3] = 127;
        }
        return color;
    };
    const vx::Skin kept =
        read(vx::test::make_png(64, 32, PngColor::rgba, samples(64, 32, seen_through)));
    for (int y = 0; y < 16; ++y) {
        for (int x = 32; x < 64; ++x) {
            INFO("texel (", x, ", ", y, ")");
            CHECK(texel(kept, x, y) == opaque(x, y));
        }
    }
}

TEST_CASE("a skin keeps the arms it is read for") {
    const std::string png = vx::test::make_png(64, 64, PngColor::gray, std::string(64 * 64, 'x'));
    CHECK(read(png, vx::Arms::slim).arms == vx::Arms::slim);
    CHECK(read(png, vx::Arms::wide).arms == vx::Arms::wide);
}

TEST_CASE("a skin of any other size or not a PNG is refused") {
    for (const auto& [width, height] :
         {std::pair{64, 48}, std::pair{128, 128}, std::pair{32, 64}, std::pair{64, 16}}) {
        const std::string png = vx::test::make_png(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), PngColor::gray,
            std::string(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 'x'));
        const vx::Result<vx::Skin> skin = vx::minecraft::read_skin(bytes_of(png), vx::Arms::wide);
        REQUIRE_FALSE(skin.has_value());
        CHECK(skin.error().message == "is " + std::to_string(width) + " by " +
                                          std::to_string(height) +
                                          " pixels, not 64 by 64 or 64 by 32");
    }
    const vx::Result<vx::Skin> text =
        vx::minecraft::read_skin(bytes_of("not a PNG"), vx::Arms::wide);
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().message.starts_with("is not a PNG file stb_image can read ("));
}

TEST_CASE("the default skins are the game's nine") {
    CHECK(vx::minecraft::default_skin_names ==
          std::array<std::string_view, 9>{"alex", "ari", "efe", "kai", "makena", "noor", "steve",
                                          "sunny", "zuri"});
}

TEST_CASE("a default skin is read from the jar's folder for its arms") {
    const auto gray = [](int value) {
        return vx::test::make_png(64, 64, PngColor::gray,
                                  std::string(64 * 64, static_cast<char>(value)));
    };
    const vx::Archive jar = jar_of({{"slim/alex.png", gray(40)}, {"wide/alex.png", gray(80)}});
    const vx::Result<vx::Skin> slim = vx::minecraft::default_skin(jar, "alex", vx::Arms::slim);
    REQUIRE(slim.has_value());
    CHECK(slim.value().arms == vx::Arms::slim);
    CHECK(texel(slim.value(), 9, 9) == Texel{40, 40, 40, 255});
    const vx::Result<vx::Skin> wide = vx::minecraft::default_skin(jar, "alex", vx::Arms::wide);
    REQUIRE(wide.has_value());
    CHECK(wide.value().arms == vx::Arms::wide);
    CHECK(texel(wide.value(), 9, 9) == Texel{80, 80, 80, 255});
}

TEST_CASE("a palette default skin with a transparent entry decodes to its colors as they are") {
    // As the game's are: a palette, one entry transparent, the hat empty. The
    // game draws them unchanged, so a transparent texel stays transparent even
    // on the base layer, here at (0, 0), which no face uses.
    const auto color = [](int x, int y) {
        if (x >= 8 && x < 16 && y >= 8 && y < 16) {
            return Texel{200, 150, 100, 255}; // the face
        }
        if (x >= 20 && x < 28 && y >= 20 && y < 32) {
            return Texel{50, 60, 70, 255}; // the body's front
        }
        if (x == 21 && y == 37) {
            return Texel{90, 91, 92, 255}; // one jacket texel
        }
        return Texel{10, 20, 30, 0};
    };
    const std::string png = vx::test::make_palette_png(64, 64, 4, samples(64, 64, color));
    REQUIRE_FALSE(png.empty());
    const vx::Archive jar = jar_of({{"wide/noor.png", png}});
    const vx::Result<vx::Skin> skin = vx::minecraft::default_skin(jar, "noor", vx::Arms::wide);
    REQUIRE(skin.has_value());
    check_texels(skin.value(), color);
    CHECK(texel(skin.value(), 0, 0) == Texel{10, 20, 30, 0});
    CHECK(texel(skin.value(), 40, 8) == Texel{10, 20, 30, 0});
}

TEST_CASE("a default skin the jar cannot give is an error that says which and why") {
    const std::string square =
        vx::test::make_png(64, 64, PngColor::gray, std::string(64 * 64, 'x'));
    std::vector<vx::test::ZipEntry> entries = {
        {.name = "assets/minecraft/textures/entity/player/slim/kai.png",
         .data = vx::test::make_png(64, 32, PngColor::gray, std::string(64 * 32, 'x'))},
        {.name = "assets/minecraft/textures/entity/player/slim/noor.png", .data = "GIF89a"},
        {.name = "assets/minecraft/textures/entity/player/wide/zuri.png", .data = square},
    };
    vx::test::Zip zip = vx::test::make_zip(entries);
    // The last byte of zuri's stored data, so that its CRC-32 fails.
    zip.bytes[zip.central_headers[0] - 1] ^= std::byte{0xFF};
    vx::Result<vx::Archive> archive = vx::Archive::from_bytes("26.2.jar", std::move(zip.bytes));
    REQUIRE(archive.has_value());
    const vx::Archive& jar = archive.value();
    const auto message = [&](std::string_view name, vx::Arms arms) {
        const vx::Result<vx::Skin> skin = vx::minecraft::default_skin(jar, name, arms);
        REQUIRE_FALSE(skin.has_value());
        return skin.error().message;
    };
    CHECK(message("bob", vx::Arms::wide) ==
          "the game has no default skin named 'bob', only alex, ari, efe, kai, makena, noor, "
          "steve, sunny, zuri");
    CHECK(message("Steve", vx::Arms::wide)
              .starts_with("the game has no default skin named "
                           "'Steve'"));
    CHECK(message("steve", vx::Arms::wide) ==
          "26.2.jar has no entry 'assets/minecraft/textures/entity/player/wide/steve.png'");
    // The slim one is no stand-in for the wide one.
    CHECK(message("kai", vx::Arms::wide) ==
          "26.2.jar has no entry 'assets/minecraft/textures/entity/player/wide/kai.png'");
    // A legacy size is no default skin: the game draws its defaults as they are.
    CHECK(message("kai", vx::Arms::slim) ==
          "'assets/minecraft/textures/entity/player/slim/kai.png' is 64 by 32 pixels, not 64 by "
          "64");
    CHECK(message("noor", vx::Arms::slim)
              .starts_with("'assets/minecraft/textures/entity/player/slim/noor.png' is not a PNG "
                           "file stb_image can read ("));
    CHECK(message("zuri", vx::Arms::wide) ==
          "26.2.jar: 'assets/minecraft/textures/entity/player/wide/zuri.png' fails its CRC-32 "
          "check");
}

TEST_CASE("the owner's skin in the core pack reads as a 64 by 64 skin") {
    const std::filesystem::path path =
        std::filesystem::path(VX_SHIPPED_PACK_DIR) / "assets/core/textures/entity/player/zlex.png";
    const std::optional<std::string> png = vx::read_file(path);
    REQUIRE(png.has_value());
    // Its header's width and height, big-endian after the signature and the
    // header's length and type: not a legacy skin made square.
    REQUIRE(png->size() > 24);
    const auto field = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8U) | static_cast<std::uint8_t>((*png)[at + index]);
        }
        return value;
    };
    CHECK(field(16) == 64);
    CHECK(field(20) == 64);
    const vx::Result<vx::Skin> skin = vx::minecraft::read_skin(bytes_of(*png), vx::Arms::slim);
    if (!skin.has_value()) {
        FAIL(skin.error().message);
    }
}

TEST_CASE("Perdix's own skin in the core pack reads as a 64 by 64 skin") {
    const std::filesystem::path path = std::filesystem::path(VX_SHIPPED_PACK_DIR) /
                                       "assets/core/textures/entity/player/perdix.png";
    const std::optional<std::string> png = vx::read_file(path);
    REQUIRE(png.has_value());
    REQUIRE(png->size() > 24);
    const auto field = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8U) | static_cast<std::uint8_t>((*png)[at + index]);
        }
        return value;
    };
    CHECK(field(16) == 64);
    CHECK(field(20) == 64);
    const vx::Result<vx::Skin> skin = vx::minecraft::read_skin(bytes_of(*png), vx::Arms::wide);
    if (!skin.has_value()) {
        FAIL(skin.error().message);
    }
}

TEST_CASE("the game's default skins read from the player's own Minecraft, when asked") {
    // A local check against the real game, never in CI: with the variable set,
    // finding no jar fails the test.
    if (!vx::test::player_minecraft_asked()) {
        MESSAGE(vx::test::not_asked);
        return;
    }
    std::vector<vx::Diagnostic> diagnostics;
    const vx::Result<vx::minecraft::Install> install =
        vx::minecraft::find_install(vx::test::player_query(), diagnostics);
    if (!install.has_value()) {
        FAIL(install.error().message);
    }
    const vx::Archive& jar = install.value().jar;
    for (const std::string_view name : vx::minecraft::default_skin_names) {
        for (const vx::Arms arms : {vx::Arms::slim, vx::Arms::wide}) {
            const std::string folder = arms == vx::Arms::slim ? "slim/" : "wide/";
            INFO(folder, name);
            const vx::Result<vx::Skin> skin = vx::minecraft::default_skin(jar, name, arms);
            if (!skin.has_value()) {
                FAIL_CHECK(skin.error().message);
                continue;
            }
            // As RFC 0012 found them: alpha 0 or 255 only, the hat empty.
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const int alpha = texel(skin.value(), x, y)[3];
                    if ((alpha != 0 && alpha != 255) || (x >= 32 && y < 16 && alpha != 0)) {
                        FAIL_CHECK("texel (" << x << ", " << y << ") has alpha " << alpha);
                    }
                }
            }
            // Read as a skin file, only the see-through texels of the base
            // layer change, made opaque: those are on no face of the model.
            const vx::Result<std::vector<std::byte>> png = jar.read(
                "assets/minecraft/textures/entity/player/" + folder + std::string(name) + ".png");
            REQUIRE(png.has_value());
            const vx::Skin as_file = read(
                std::string(reinterpret_cast<const char*>(png.value().data()), png.value().size()),
                arms);
            check_texels(as_file, [&](int x, int y) {
                Texel want = texel(skin.value(), x, y);
                if (on_base(x, y)) {
                    want[3] = 255;
                }
                return want;
            });
        }
    }
    MESSAGE("Minecraft ", install.value().version, ": the ",
            2 * vx::minecraft::default_skin_names.size(), " default skins read");
}
