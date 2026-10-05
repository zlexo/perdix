#include <vx/render/block_textures.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

constexpr std::size_t size = block_texture_size;

constexpr std::size_t at(std::size_t x, std::size_t y) noexcept {
    return (y * size) + x;
}

// The alpha test discards below 0.5: an 8-bit alpha of 127 or less.
constexpr std::uint8_t passes_alpha_test = 128;

// sRGB to linear light, for each 8-bit value.
const std::array<double, 256>& linear_of() {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> linear{};
        for (std::size_t value = 0; value < linear.size(); ++value) {
            const double c = static_cast<double>(value) / 255.0;
            linear[value] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return linear;
    }();
    return table;
}

std::uint8_t to_byte(double unit) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0, 1.0) * 255.0));
}

std::uint8_t srgb_of(double linear) noexcept {
    linear = std::clamp(linear, 0.0, 1.0);
    return to_byte(linear <= 0.0031308 ? linear * 12.92
                                       : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055);
}

// A color in linear light, with alpha from 0 to 1.
struct Linear {
    double r = 0;
    double g = 0;
    double b = 0;
    double a = 0;
};

Texel texel_of(const Linear& color) noexcept {
    return {
        .r = srgb_of(color.r), .g = srgb_of(color.g), .b = srgb_of(color.b), .a = to_byte(color.a)};
}

// The order of an ordered-dither (Bayer) matrix of side `side`, a power of
// two: neighbors come far apart in it, so taking its first few spreads them
// out.
std::size_t bayer_rank(std::size_t x, std::size_t y, std::size_t side) noexcept {
    // The finest 2 by 2 pattern is the most significant digit, so the first
    // quarter of the ranks is every other texel of every other row.
    std::size_t rank = 0;
    for (std::size_t half = 1; half < side; half *= 2) {
        constexpr std::array<std::size_t, 4> quadrant = {0, 2, 3, 1}; // [[0, 2], [3, 1]]
        rank = (rank * 4) + quadrant[(((y / half) % 2) * 2) + ((x / half) % 2)];
    }
    return rank;
}

// A level's alpha in 8 bits, scaled so the share of texels that pass the
// alpha test matches `coverage`, as near as a whole texel allows. Castaño
// ("Computing Alpha Mipmaps") scales every alpha by one factor; here the
// factor puts the k-th most opaque texel exactly at the test, so the k most
// opaque pass. One factor cannot split texels of equal alpha, and pixel art
// is full of them, so ties at the test are broken by a Bayer rank, moving
// those texels one step of 255 to whichever side they belong.
std::vector<std::uint8_t> keep_coverage(const std::vector<double>& alpha, std::size_t side,
                                        double coverage) {
    const auto k =
        static_cast<std::size_t>(std::lround(coverage * static_cast<double>(alpha.size())));
    std::vector<std::size_t> order(alpha.size());
    for (std::size_t texel = 0; texel < order.size(); ++texel) {
        order[texel] = texel;
    }
    std::ranges::sort(order, [&](std::size_t one, std::size_t other) {
        if (alpha[one] != alpha[other]) {
            return alpha[one] > alpha[other];
        }
        return bayer_rank(one % side, one / side, side) <
               bayer_rank(other % side, other / side, side);
    });
    // The texel that should be the last to pass sits at 0.5, or, when none
    // should, the most opaque one does.
    const double pivot = alpha[order[k == 0 ? 0 : k - 1]];
    const double scale = pivot > 0 ? 0.5 / pivot : 1.0;
    std::vector<std::uint8_t> bytes(alpha.size());
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const std::size_t texel = order[rank];
        const std::uint8_t byte = to_byte(alpha[texel] * scale);
        if (rank < k) {
            // A texel no source texel covers stays a hole.
            bytes[texel] = alpha[texel] > 0 ? std::max(byte, passes_alpha_test) : byte;
        } else {
            bytes[texel] = std::min<std::uint8_t>(byte, passes_alpha_test - 1);
        }
    }
    return bytes;
}

// The color a fully transparent texel at (x, y) takes in this ring: the
// average, in linear light, of its neighbors filled in earlier rings, if
// it has any.
std::optional<Texel> ring_color(const BlockImage& image, const std::array<bool, 256>& filled,
                                std::size_t x, std::size_t y) {
    const std::array<double, 256>& linear = linear_of();
    const std::array<std::size_t, 4> neighbors = {
        at((x + 1) % size, y), at((x + size - 1) % size, y), at(x, (y + 1) % size),
        at(x, (y + size - 1) % size)};
    Linear sum;
    double count = 0;
    for (const std::size_t neighbor : neighbors) {
        if (filled[neighbor]) {
            sum.r += linear[image[neighbor].r];
            sum.g += linear[image[neighbor].g];
            sum.b += linear[image[neighbor].b];
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return texel_of({.r = sum.r / count, .g = sum.g / count, .b = sum.b / count, .a = 0});
}

// The texel of a level at (x, y): the box of level 0 texels under it,
// averaged in linear light with color weighted by alpha. Where every texel
// is fully transparent the weights sum to zero, and the colors
// fill_transparent gave them stand in, averaged plainly.
Linear box_average(const BlockImage& image, std::size_t level, std::size_t x, std::size_t y) {
    const std::array<double, 256>& linear = linear_of();
    const std::size_t box = std::size_t{1} << level;
    Linear weighted;
    Linear plain;
    for (std::size_t dy = 0; dy < box; ++dy) {
        for (std::size_t dx = 0; dx < box; ++dx) {
            const Texel& texel = image[at((x * box) + dx, (y * box) + dy)];
            const double alpha = static_cast<double>(texel.a) / 255.0;
            weighted.r += alpha * linear[texel.r];
            weighted.g += alpha * linear[texel.g];
            weighted.b += alpha * linear[texel.b];
            weighted.a += alpha;
            plain.r += linear[texel.r];
            plain.g += linear[texel.g];
            plain.b += linear[texel.b];
        }
    }
    const auto count = static_cast<double>(box * box);
    const bool any = weighted.a > 0;
    const double divisor = any ? weighted.a : count;
    const Linear& sum = any ? weighted : plain;
    return {
        .r = sum.r / divisor, .g = sum.g / divisor, .b = sum.b / divisor, .a = weighted.a / count};
}

} // namespace

BlockImage turned(const BlockImage& image, int quarter_turns) noexcept {
    BlockImage result = image;
    for (int turn = 0; turn < ((quarter_turns % 4) + 4) % 4; ++turn) {
        const BlockImage before = result;
        // The left column, read from the bottom up, becomes the top row.
        for (std::size_t y = 0; y < size; ++y) {
            for (std::size_t x = 0; x < size; ++x) {
                result[at(x, y)] = before[at(y, size - 1 - x)];
            }
        }
    }
    return result;
}

void fill_transparent(BlockImage& image) {
    std::array<bool, 256> filled{};
    for (std::size_t texel = 0; texel < image.size(); ++texel) {
        filled[texel] = image[texel].a > 0;
    }
    if (std::ranges::none_of(filled, [](bool f) { return f; })) {
        return; // nothing visible to spread
    }
    // A ring at a time: each texel next to the filled ones takes the average
    // of those neighbors, and only then does the ring count as filled, so no
    // texel reads a color set in the same ring.
    while (!std::ranges::all_of(filled, [](bool f) { return f; })) {
        std::vector<std::pair<std::size_t, Texel>> ring;
        for (std::size_t y = 0; y < size; ++y) {
            for (std::size_t x = 0; x < size; ++x) {
                if (!filled[at(x, y)]) {
                    if (const std::optional<Texel> color = ring_color(image, filled, x, y)) {
                        ring.emplace_back(at(x, y), *color);
                    }
                }
            }
        }
        for (const auto& [texel, color] : ring) {
            image[texel] = color;
            filled[texel] = true;
        }
    }
}

BlockMips mipmaps(const BlockImage& image) {
    BlockMips mips{};
    std::ranges::copy(image, mips.begin());

    const auto passes = static_cast<std::size_t>(std::ranges::count_if(
        image, [](const Texel& texel) { return texel.a >= passes_alpha_test; }));
    const bool alpha_tested = passes < image.size();
    const double coverage = static_cast<double>(passes) / static_cast<double>(image.size());

    for (std::size_t level = 1; level < block_texture_levels; ++level) {
        const std::size_t side = size >> level;
        std::vector<Linear> colors(side * side);
        for (std::size_t y = 0; y < side; ++y) {
            for (std::size_t x = 0; x < side; ++x) {
                colors[(y * side) + x] = box_average(image, level, x, y);
            }
        }
        auto* const first = &mips[block_texture_level_start[level]];
        std::ranges::transform(colors, first, texel_of);
        if (alpha_tested) {
            std::vector<double> alpha(colors.size());
            std::ranges::transform(colors, alpha.begin(), &Linear::a);
            const std::vector<std::uint8_t> kept = keep_coverage(alpha, side, coverage);
            for (std::size_t texel = 0; texel < kept.size(); ++texel) {
                first[texel].a = kept[texel];
            }
        }
    }
    return mips;
}

BlockImage checker() noexcept {
    constexpr Texel magenta{.r = 255, .g = 0, .b = 255, .a = 255};
    constexpr Texel black{.r = 0, .g = 0, .b = 0, .a = 255};
    BlockImage image{};
    for (std::size_t y = 0; y < size; ++y) {
        for (std::size_t x = 0; x < size; ++x) {
            image[at(x, y)] = ((x / 8) + (y / 8)) % 2 == 0 ? magenta : black;
        }
    }
    return image;
}

} // namespace vx::render
