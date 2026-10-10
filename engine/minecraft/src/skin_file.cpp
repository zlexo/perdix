#include <vx/minecraft/skin_file.hpp>

#include "png.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>
#include <vx/world/skin.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vx::minecraft {
namespace {

// A skin from before 1.8: the top half of today's, with one arm and one leg.
constexpr int legacy_skin_height = 32;

// Texels from (x0, y0) to (x1, y1) on a skin, the ends exclusive.
struct Area {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// The base layer: the head; the body, right arm and right leg; the left arm
// and left leg.
constexpr Area head_base = {.x0 = 0, .y0 = 0, .x1 = 32, .y1 = 16};
constexpr Area body_base = {.x0 = 0, .y0 = 16, .x1 = 64, .y1 = 32};
constexpr Area left_limbs_base = {.x0 = 16, .y0 = 48, .x1 = 48, .y1 = 64};
// A legacy skin's right half: the hat area and the half below it.
constexpr Area legacy_right_half = {.x0 = 32, .y0 = 0, .x1 = 64, .y1 = 32};

// One face of a legacy skin's right limb, `width` by `height` texels at
// (x, y), and the place of the left limb's face it makes, mirrored left to
// right, at (to_x, to_y).
struct LimbFace {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int to_x = 0;
    int to_y = 0;
};

// The game's copies (SkinTextureDownloader, 26.2): each face to the same face
// of the left limb, the outer side to the outer side and the inner to the
// inner, so that the left limbs look like the right ones in a mirror.
constexpr std::array<LimbFace, 12> left_limb_faces = {{
    // The right leg's top, bottom, outer side, front, inner side and back.
    {.x = 4, .y = 16, .width = 4, .height = 4, .to_x = 20, .to_y = 48},
    {.x = 8, .y = 16, .width = 4, .height = 4, .to_x = 24, .to_y = 48},
    {.x = 0, .y = 20, .width = 4, .height = 12, .to_x = 24, .to_y = 52},
    {.x = 4, .y = 20, .width = 4, .height = 12, .to_x = 20, .to_y = 52},
    {.x = 8, .y = 20, .width = 4, .height = 12, .to_x = 16, .to_y = 52},
    {.x = 12, .y = 20, .width = 4, .height = 12, .to_x = 28, .to_y = 52},
    // The right arm's, in the same order.
    {.x = 44, .y = 16, .width = 4, .height = 4, .to_x = 36, .to_y = 48},
    {.x = 48, .y = 16, .width = 4, .height = 4, .to_x = 40, .to_y = 48},
    {.x = 40, .y = 20, .width = 4, .height = 12, .to_x = 40, .to_y = 52},
    {.x = 44, .y = 20, .width = 4, .height = 12, .to_x = 36, .to_y = 52},
    {.x = 48, .y = 20, .width = 4, .height = 12, .to_x = 32, .to_y = 52},
    {.x = 52, .y = 20, .width = 4, .height = 12, .to_x = 44, .to_y = 52},
}};

// The first byte of the texel at (x, y).
constexpr std::size_t texel_at(int x, int y) noexcept {
    return 4 * ((static_cast<std::size_t>(y) * skin_size) + static_cast<std::size_t>(x));
}

void set_alpha(Skin& skin, Area area, std::uint8_t alpha) noexcept {
    for (int y = area.y0; y < area.y1; ++y) {
        for (int x = area.x0; x < area.x1; ++x) {
            skin.rgba[texel_at(x, y) + 3] = alpha;
        }
    }
}

// The old skins drew no hat, but many filled its area anyway: when nothing in
// the right half is see-through, the hat area is taken for empty.
void clear_filled_hat(Skin& skin) noexcept {
    for (int y = legacy_right_half.y0; y < legacy_right_half.y1; ++y) {
        for (int x = legacy_right_half.x0; x < legacy_right_half.x1; ++x) {
            if (skin.rgba[texel_at(x, y) + 3] < 128) {
                return;
            }
        }
    }
    // The half below is made opaque again with the rest of the base.
    set_alpha(skin, legacy_right_half, 0);
}

} // namespace

Result<Skin> read_skin(std::span<const std::byte> png, Arms arms) {
    constexpr std::array<ImageSize, 2> sizes = {
        ImageSize{.width = skin_size, .height = skin_size},
        ImageSize{.width = skin_size, .height = legacy_skin_height}};
    const Result<Image> image = decode_png(png, sizes);
    if (!image) {
        return image.error();
    }
    // A legacy skin fills the top half; the rest starts transparent black.
    Skin skin{.rgba = {}, .arms = arms};
    std::ranges::copy(image.value().rgba, skin.rgba.begin());
    const bool legacy = image.value().size.height == legacy_skin_height;
    if (legacy) {
        for (const LimbFace& face : left_limb_faces) {
            for (int y = 0; y < face.height; ++y) {
                for (int x = 0; x < face.width; ++x) {
                    const std::size_t from = texel_at(face.x + x, face.y + y);
                    const std::size_t to = texel_at(face.to_x + face.width - 1 - x, face.to_y + y);
                    std::copy_n(skin.rgba.begin() + static_cast<std::ptrdiff_t>(from), 4,
                                skin.rgba.begin() + static_cast<std::ptrdiff_t>(to));
                }
            }
        }
    }
    set_alpha(skin, head_base, 255);
    if (legacy) {
        clear_filled_hat(skin);
    }
    set_alpha(skin, body_base, 255);
    set_alpha(skin, left_limbs_base, 255);
    return skin;
}

Result<Skin> default_skin(const Archive& jar, std::string_view name, Arms arms) {
    if (std::ranges::find(default_skin_names, name) == default_skin_names.end()) {
        std::string names;
        for (const std::string_view known : default_skin_names) {
            names += (names.empty() ? "" : ", ") + std::string(known);
        }
        return Error{"the game has no default skin named '" + std::string(name) + "', only " +
                     names};
    }
    const std::string entry = "assets/minecraft/textures/entity/player/" +
                              std::string(arms == Arms::slim ? "slim/" : "wide/") +
                              std::string(name) + ".png";
    // The archive's own message names it and the entry.
    const Result<std::vector<std::byte>> bytes = jar.read(entry);
    if (!bytes) {
        return bytes.error();
    }
    constexpr std::array<ImageSize, 1> size = {ImageSize{.width = skin_size, .height = skin_size}};
    const Result<Image> image = decode_png(bytes.value(), size);
    if (!image) {
        return Error{"'" + entry + "' " + image.error().message};
    }
    Skin skin{.rgba = {}, .arms = arms};
    std::ranges::copy(image.value().rgba, skin.rgba.begin());
    return skin;
}

} // namespace vx::minecraft
