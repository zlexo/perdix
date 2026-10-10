#include <vx/minecraft/skin_file.hpp>

#include "png.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>
#include <vx/world/skin.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vx::minecraft {

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
