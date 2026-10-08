#pragma once

#include "zip_bytes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vx::test {

// The PNG color types make_png writes, each with 8 bits a sample.
enum class PngColor : std::uint8_t { gray = 0, rgb = 2, rgba = 6 };

inline std::size_t samples_of(PngColor color) {
    switch (color) {
    case PngColor::gray:
        return 1;
    case PngColor::rgb:
        return 3;
    case PngColor::rgba:
        return 4;
    }
    return 0;
}

inline void append_big32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

// A chunk: its length, its type and data, and the CRC-32 of the two.
inline void append_chunk(std::string& out, std::string_view type, std::string_view data) {
    append_big32(out, static_cast<std::uint32_t>(data.size()));
    const std::string typed = std::string(type) + std::string(data);
    out += typed;
    append_big32(out, crc_of(typed));
}

// A zlib stream (RFC 1950) around stored deflate blocks.
inline std::string zlib_stored(std::string_view data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const char c : data) {
        a = (a + static_cast<std::uint8_t>(c)) % 65521;
        b = (b + a) % 65521;
    }
    std::string out = "\x78\x01";
    out += deflate_stored(data);
    append_big32(out, (b << 16U) | a);
    return out;
}

// A PNG of `width` by `height` texels, with `samples` its rows top first,
// each row's samples left to right with no filter byte. A gray image may
// name a gray value as transparent (a tRNS chunk), which is how Minecraft's
// overlays mark their holes.
inline std::string make_png(std::uint32_t width, std::uint32_t height, PngColor color,
                            std::string_view samples,
                            std::optional<std::uint8_t> transparent_gray = std::nullopt) {
    const std::size_t row = width * samples_of(color);
    std::string raw;
    for (std::size_t y = 0; y < height; ++y) {
        raw.push_back('\0'); // no filter
        raw.append(samples.substr(y * row, row));
    }
    std::string header;
    append_big32(header, width);
    append_big32(header, height);
    header.push_back('\x08'); // bits a sample
    header.push_back(static_cast<char>(color));
    header.append(3, '\0'); // deflate, adaptive filters, no interlace
    std::string out = "\x89PNG\r\n\x1a\n";
    append_chunk(out, "IHDR", header);
    if (transparent_gray) {
        append_chunk(out, "tRNS", std::string{'\0', static_cast<char>(*transparent_gray)});
    }
    append_chunk(out, "IDAT", zlib_stored(raw));
    append_chunk(out, "IEND", "");
    return out;
}

// A palette PNG (color type 3) of `width` by `height` texels at `bits`, 1,
// 2, 4 or 8, an index, as the game stores most of its block textures. `rgba`
// holds each texel's color, four bytes a texel, top row first; the palette
// is every distinct color in order of first use, with a tRNS chunk when any
// is not opaque. More colors than the bits can index give no PNG at all, so
// a test that asks for them fails to decode.
inline std::string make_palette_png(std::uint32_t width, std::uint32_t height, unsigned bits,
                                    std::string_view rgba) {
    std::vector<std::string> colors;
    std::vector<std::size_t> indices;
    for (std::size_t texel = 0; texel < std::size_t{width} * height; ++texel) {
        const std::string color(rgba.substr(texel * 4, 4));
        const auto found = std::ranges::find(colors, color);
        indices.push_back(static_cast<std::size_t>(found - colors.begin()));
        if (found == colors.end()) {
            colors.push_back(color);
        }
    }
    if (colors.size() > (std::size_t{1} << bits)) {
        return {};
    }
    std::string palette;
    std::string alphas;
    bool translucent = false;
    for (const std::string& color : colors) {
        palette += color.substr(0, 3);
        alphas.push_back(color[3]);
        translucent = translucent || color[3] != '\xFF';
    }
    // Each row packed from its first texel in the high bits, padded to a byte.
    std::string raw;
    for (std::size_t y = 0; y < height; ++y) {
        raw.push_back('\0'); // no filter
        unsigned packed = 0;
        unsigned filled = 0;
        for (std::size_t x = 0; x < width; ++x) {
            packed = (packed << bits) | static_cast<unsigned>(indices[(y * width) + x]);
            filled += bits;
            if (filled == 8) {
                raw.push_back(static_cast<char>(packed));
                packed = 0;
                filled = 0;
            }
        }
        if (filled > 0) {
            raw.push_back(static_cast<char>(packed << (8 - filled)));
        }
    }
    std::string header;
    append_big32(header, width);
    append_big32(header, height);
    header.push_back(static_cast<char>(bits));
    header.push_back('\x03'); // palette
    header.append(3, '\0');   // deflate, adaptive filters, no interlace
    std::string out = "\x89PNG\r\n\x1a\n";
    append_chunk(out, "IHDR", header);
    append_chunk(out, "PLTE", palette);
    if (translucent) {
        append_chunk(out, "tRNS", alphas);
    }
    append_chunk(out, "IDAT", zlib_stored(raw));
    append_chunk(out, "IEND", "");
    return out;
}

} // namespace vx::test
