#include "png.hpp"

#include <vx/core/result.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace vx::minecraft {

Result<Image> decode_png(std::span<const std::byte> png, std::span<const ImageSize> sizes) {
    const auto* const data = reinterpret_cast<const stbi_uc*>(png.data());
    const auto length = static_cast<int>(png.size());
    ImageSize size;
    int channels = 0;
    const auto not_png = [] {
        // stb_image gives no reason when one of its allocations fails.
        const char* const reason = stbi_failure_reason();
        return Error{std::string("is not a PNG file stb_image can read (") +
                     (reason != nullptr ? reason : "no reason given") + ")"};
    };
    const auto wrong_size = [&] {
        std::string message = "is " + std::to_string(size.width) + " by " +
                              std::to_string(size.height) + " pixels, not ";
        for (std::size_t index = 0; index < sizes.size(); ++index) {
            message += (index == 0 ? "" : " or ") + std::to_string(sizes[index].width) + " by " +
                       std::to_string(sizes[index].height);
        }
        return Error{message};
    };
    if (stbi_info_from_memory(data, length, &size.width, &size.height, &channels) == 0) {
        return not_png();
    }
    if (std::ranges::find(sizes, size) == sizes.end()) {
        return wrong_size();
    }
    const ImageSize expected = size;
    stbi_uc* const pixels =
        stbi_load_from_memory(data, length, &size.width, &size.height, &channels, 4);
    if (pixels == nullptr) {
        return not_png();
    }
    // The load reads the header again; the copy below trusts what it says.
    if (size != expected) {
        stbi_image_free(pixels);
        return wrong_size();
    }
    const auto count =
        static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4;
    Image image{.size = size, .rgba = std::vector<std::uint8_t>(count)};
    std::memcpy(image.rgba.data(), pixels, count);
    stbi_image_free(pixels);
    return image;
}

} // namespace vx::minecraft
