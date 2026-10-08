#include <vx/minecraft/made_textures.hpp>

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_registry.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vx::minecraft {
namespace {

constexpr int block_texture_side = 16;
constexpr int color_map_side = 256;

// A PNG's texels as RGBA8, when it is `side` by `side` texels. The size is
// read from its header first, so a file that claims a huge image is refused
// before anything is decoded. A problem is worded to follow the entry's
// name: "is 16 by 32 pixels, not 16 by 16".
Result<std::vector<std::uint8_t>> decode(std::span<const std::byte> png, int side) {
    const auto* const data = reinterpret_cast<const stbi_uc*>(png.data());
    const auto length = static_cast<int>(png.size());
    int width = 0;
    int height = 0;
    int channels = 0;
    const auto not_png = [] {
        // stb_image gives no reason when one of its allocations fails.
        const char* const reason = stbi_failure_reason();
        return Error{std::string("is not a PNG file stb_image can read (") +
                     (reason != nullptr ? reason : "no reason given") + ")"};
    };
    const auto wrong_size = [&] {
        return Error{"is " + std::to_string(width) + " by " + std::to_string(height) +
                     " pixels, not " + std::to_string(side) + " by " + std::to_string(side)};
    };
    if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0) {
        return not_png();
    }
    if (width != side || height != side) {
        return wrong_size();
    }
    stbi_uc* const pixels = stbi_load_from_memory(data, length, &width, &height, &channels, 4);
    if (pixels == nullptr) {
        return not_png();
    }
    // The load reads the header again; the copy below trusts what it says.
    if (width != side || height != side) {
        stbi_image_free(pixels);
        return wrong_size();
    }
    const auto count = static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4;
    std::vector<std::uint8_t> rgba(count);
    std::memcpy(rgba.data(), pixels, count);
    stbi_image_free(pixels);
    return rgba;
}

// What making textures reads from the jar, each entry once however many
// textures use it. A problem is worded to follow "Minecraft 26.2".
class Jar {
public:
    explicit Jar(const Archive& archive) : archive_(archive) {}

    // A block texture's texels: assets/minecraft/textures/block/<name>.png.
    const Result<Texels>& block_texture(const std::string& name) {
        const auto found = textures_.find(name);
        if (found != textures_.end()) {
            return found->second;
        }
        return textures_.emplace(name, read_block_texture(name)).first->second;
    }

    // The plains color of a color map: assets/minecraft/textures/colormap/<name>.png.
    const Result<Color>& plains_color(const std::string& name) {
        const auto found = colors_.find(name);
        if (found != colors_.end()) {
            return found->second;
        }
        return colors_.emplace(name, read_plains_color(name)).first->second;
    }

private:
    // The entry's PNG, decoded, or why not, worded to follow "Minecraft 26.2":
    // " has no <kind> '<name>'", or "'s <kind> '<name>' ...".
    Result<std::vector<std::uint8_t>> image(const std::string& entry, const std::string& kind,
                                            const std::string& name, int side) const {
        if (!archive_.contains(entry)) {
            return Error{" has no " + kind + " '" + name + "'"};
        }
        const Result<std::vector<std::byte>> bytes = archive_.read(entry);
        if (!bytes) {
            return Error{"'s " + kind + " '" + name + "' cannot be read (" + bytes.error().message +
                         ")"};
        }
        Result<std::vector<std::uint8_t>> rgba = decode(bytes.value(), side);
        if (!rgba) {
            return Error{"'s " + kind + " '" + name + "' " + rgba.error().message};
        }
        return rgba;
    }

    Result<Texels> read_block_texture(const std::string& name) const {
        Result<std::vector<std::uint8_t>> rgba =
            image("assets/minecraft/textures/block/" + name + ".png", "block texture", name,
                  block_texture_side);
        if (!rgba) {
            return rgba.error();
        }
        Texels texels{};
        std::ranges::copy(rgba.value(), texels.begin());
        return texels;
    }

    Result<Color> read_plains_color(const std::string& name) const {
        const Result<std::vector<std::uint8_t>> rgba =
            image("assets/minecraft/textures/colormap/" + name + ".png", "color map", name,
                  color_map_side);
        if (!rgba) {
            return rgba.error();
        }
        const ColorMapTexel at = color_map_texel(plains_temperature, plains_downfall);
        const std::size_t first = 4 * ((static_cast<std::size_t>(at.y) * color_map_side) +
                                       static_cast<std::size_t>(at.x));
        return Color{rgba.value()[first], rgba.value()[first + 1], rgba.value()[first + 2]};
    }

    const Archive& archive_;
    std::map<std::string, Result<Texels>, std::less<>> textures_;
    std::map<std::string, Result<Color>, std::less<>> colors_;
};

// A layer's texels multiplied by its tint's color; alpha is left alone.
void tint(Texels& texels, const Color& color) noexcept {
    for (std::size_t texel = 0; texel < texels.size(); texel += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            texels[texel + channel] = tinted(texels[texel + channel], color[channel]);
        }
    }
}

} // namespace

ColorMapTexel color_map_texel(float temperature, float downfall) noexcept {
    // Java's (int) of a double, which is 0 for NaN: clamping lets NaN through.
    const auto java_int = [](double value) {
        return std::isnan(value) ? 0 : static_cast<int>(value);
    };
    const auto t = static_cast<double>(std::clamp(temperature, 0.0F, 1.0F));
    const double d = static_cast<double>(std::clamp(downfall, 0.0F, 1.0F)) * t;
    return {.x = java_int((1.0 - t) * 255.0), .y = java_int((1.0 - d) * 255.0)};
}

void draw_over(Texels& lower, const Texels& upper) noexcept {
    for (std::size_t texel = 0; texel < upper.size(); texel += 4) {
        if (upper[texel + 3] >= alpha_test_passes) {
            std::copy_n(upper.begin() + static_cast<std::ptrdiff_t>(texel), 4,
                        lower.begin() + static_cast<std::ptrdiff_t>(texel));
        }
    }
}

std::vector<MadeTexture> make_textures(const Archive& jar, std::string_view version,
                                       const TextureMap& map,
                                       std::vector<Diagnostic>& diagnostics) {
    Jar reader(jar);
    std::vector<MadeTexture> made;
    std::vector<Diagnostic> problems;
    for (const auto& [key, mapped] : map.textures) {
        const std::string name = map.namespace_name + ':' + key;
        const auto fall_back = [&](SourcePlace place, const Error& why) {
            problems.push_back({.severity = Severity::warning,
                                .file = map.path,
                                .line = place.line,
                                .column = place.column,
                                .message = "Minecraft " + std::string(version) + why.message +
                                           "; " + name + " is drawn from its own file"});
        };
        Texels texels{};
        bool whole = true;
        for (std::size_t index = 0; index < mapped.layers.size(); ++index) {
            const Layer& layer = mapped.layers[index];
            const Result<Texels>& read = reader.block_texture(layer.texture);
            if (!read) {
                fall_back(layer.place, read.error());
                whole = false;
                continue;
            }
            Texels layer_texels = read.value();
            if (const auto* const color_map = std::get_if<std::string>(&layer.tint)) {
                const Result<Color>& color = reader.plains_color(*color_map);
                if (!color) {
                    fall_back(layer.tint_place, color.error());
                    whole = false;
                    continue;
                }
                tint(layer_texels, color.value());
            } else if (const auto* const color = std::get_if<Color>(&layer.tint)) {
                tint(layer_texels, *color);
            }
            if (index == 0) {
                texels = layer_texels;
            } else {
                draw_over(texels, layer_texels);
            }
        }
        if (whole) {
            made.push_back({.name = name, .rgba = texels});
        }
    }
    // The map holds its textures in name order; the warnings follow the file.
    std::ranges::stable_sort(problems, {}, [](const Diagnostic& problem) {
        return SourcePlace{.line = problem.line, .column = problem.column};
    });
    diagnostics.insert(diagnostics.end(), problems.begin(), problems.end());
    return made;
}

} // namespace vx::minecraft
