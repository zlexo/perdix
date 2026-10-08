#pragma once

#include <vx/world/coordinates.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace vx {

// One visible rectangle of block faces (RFC 0003), 8 bytes where four float
// vertices would take 128. The vertex shader rebuilds its four corners; the
// chunk's place relative to the camera comes from per-draw data.
//
//   geometry  bits 0-14 x, y, z (5 each) | 15-19 width - 1 | 20-24 height - 1
//             | 25-27 face | 28-31 spare
//   material  bits 0-15 texture layer | 16-23 occlusion, 2 bits a corner,
//             corner 0 lowest | 24-31 spare
//
// The shader reads these bits as they are, so a test pins them.
struct Quad {
    std::uint32_t geometry = 0;
    std::uint32_t material = 0;

    friend constexpr bool operator==(const Quad&, const Quad&) = default;
};
static_assert(sizeof(Quad) == 8);

// A quad's fields, unpacked.
struct QuadFields {
    // Of the blocks whose faces the quad covers, the one with the smallest
    // coordinates: 0 to 31 on each axis.
    LocalPos block;
    // How many blocks the quad covers along its face's width and height axes
    // (face_axes): 1 to 32 each.
    int width = 1;
    int height = 1;
    Face face = Face::pos_x;
    // The layer of the block texture array (RFC 0004).
    std::uint16_t texture = 0;
    // Ambient occlusion at each corner, corner 0 first: 3 is fully lit, 0 the
    // darkest.
    std::array<std::uint8_t, 4> occlusion{3, 3, 3, 3};

    friend constexpr bool operator==(const QuadFields&, const QuadFields&) = default;
};

// The axes of a face's quads (RFC 0003): the one the face looks along, and
// the ones its width and height run along. 0 is x, 1 is y, 2 is z.
struct FaceAxes {
    std::size_t normal = 0;
    std::size_t width = 0;
    std::size_t height = 0;
};

constexpr FaceAxes face_axes(Face face) noexcept {
    constexpr std::array<FaceAxes, 3> by_axis{{
        {.normal = 0, .width = 2, .height = 1}, // x faces: width along z, height along y
        {.normal = 1, .width = 0, .height = 2}, // y faces: x, then z
        {.normal = 2, .width = 0, .height = 1}, // z faces: x, then y
    }};
    return by_axis[static_cast<std::size_t>(face) / 2];
}

// Whether a face looks along +x, +y or +z, rather than -x, -y or -z.
constexpr bool faces_positive(Face face) noexcept {
    return (static_cast<unsigned>(face) & 1U) == 0;
}

// A quad's geometry word: where it is, its size and its face.
constexpr std::uint32_t quad_geometry(LocalPos block, int width, int height, Face face) noexcept {
    assert(block.x >= 0 && block.x < chunk_size);
    assert(block.y >= 0 && block.y < chunk_size);
    assert(block.z >= 0 && block.z < chunk_size);
    assert(width >= 1 && width <= chunk_size);
    assert(height >= 1 && height <= chunk_size);
    const auto bits = [](int value) { return static_cast<std::uint32_t>(value); };
    return bits(block.x) | (bits(block.y) << 5U) | (bits(block.z) << 10U) |
           (bits(width - 1) << 15U) | (bits(height - 1) << 20U) |
           (static_cast<std::uint32_t>(face) << 25U);
}

// A quad's material word: its texture and its corners' occlusion. Faces
// whose material words are equal look the same, so the mesher merges them.
constexpr std::uint32_t quad_material(std::uint16_t texture,
                                      std::array<std::uint8_t, 4> occlusion) noexcept {
    std::uint32_t corners = 0;
    for (std::size_t corner = 0; corner < occlusion.size(); ++corner) {
        assert(occlusion[corner] <= 3);
        corners |= std::uint32_t{occlusion[corner]} << (2 * corner);
    }
    return std::uint32_t{texture} | (corners << 16U);
}

constexpr Quad encode_quad(const QuadFields& quad) noexcept {
    return {.geometry = quad_geometry(quad.block, quad.width, quad.height, quad.face),
            .material = quad_material(quad.texture, quad.occlusion)};
}

// The spare bits are left out: whatever a later RFC puts there, the fields
// read the same.
constexpr QuadFields decode_quad(Quad quad) noexcept {
    const auto field = [](std::uint32_t word, unsigned first, unsigned bits) {
        return (word >> first) & ((1U << bits) - 1U);
    };
    const auto five = [&](unsigned first) {
        return static_cast<std::int32_t>(field(quad.geometry, first, 5));
    };
    QuadFields fields{.block = {.x = five(0), .y = five(5), .z = five(10)},
                      .width = five(15) + 1,
                      .height = five(20) + 1,
                      .face = static_cast<Face>(field(quad.geometry, 25, 3)),
                      .texture = static_cast<std::uint16_t>(field(quad.material, 0, 16)),
                      .occlusion = {}};
    for (std::size_t corner = 0; corner < 4; ++corner) {
        fields.occlusion[corner] = static_cast<std::uint8_t>(
            field(quad.material, 16 + (2 * static_cast<unsigned>(corner)), 2));
    }
    return fields;
}

// Where corner `corner` (0 to 3) of a quad is, in blocks from the chunk's
// lowest corner: 0 to 32 on each axis, x first. In the face's width and
// height axes, corner 0 is the quad's lowest corner, corner 1 is (width, 0),
// corner 2 (width, height) and corner 3 (0, height). A face looking along +x,
// +y or +z lies one block further along its normal than its block: the +x
// face of a block at x = 31 lies at x = 32.
constexpr std::array<int, 3> quad_corner(const QuadFields& quad, int corner) noexcept {
    assert(corner >= 0 && corner < 4);
    const FaceAxes axes = face_axes(quad.face);
    std::array<int, 3> at{quad.block.x, quad.block.y, quad.block.z};
    if (faces_positive(quad.face)) {
        at[axes.normal] += 1;
    }
    if (corner == 1 || corner == 2) {
        at[axes.width] += quad.width;
    }
    if (corner == 2 || corner == 3) {
        at[axes.height] += quad.height;
    }
    return at;
}

} // namespace vx
