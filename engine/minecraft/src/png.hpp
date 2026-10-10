#pragma once

#include <vx/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vx::minecraft {

// An image's width and height, in pixels.
struct ImageSize {
    int width = 0;
    int height = 0;

    friend constexpr bool operator==(const ImageSize&, const ImageSize&) = default;
};

// An image's texels, RGBA8, rows from the top.
struct Image {
    ImageSize size;
    std::vector<std::uint8_t> rgba;
};

// A PNG's texels, decoded with stb_image, when its size is one of `sizes`.
// The size is read from its header first, so a file that claims a huge image
// is refused before anything is decoded. A problem is worded to follow the
// file's name: "is 16 by 32 pixels, not 16 by 16".
[[nodiscard]] Result<Image> decode_png(std::span<const std::byte> png,
                                       std::span<const ImageSize> sizes);

} // namespace vx::minecraft
