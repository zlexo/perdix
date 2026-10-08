#include <vx/mesh/quad.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <random>

namespace {

constexpr std::array<vx::Face, 6> faces{vx::Face::pos_x, vx::Face::neg_x, vx::Face::pos_y,
                                        vx::Face::neg_y, vx::Face::pos_z, vx::Face::neg_z};

} // namespace

TEST_CASE("a quad's fields sit in the bits the vertex shader reads") {
    const vx::Quad quad = vx::encode_quad({.block = {.x = 1, .y = 2, .z = 3},
                                           .width = 4,
                                           .height = 5,
                                           .face = vx::Face::neg_y,
                                           .texture = 0x1234,
                                           .occlusion = {0, 1, 2, 3}});
    // x 1, y 2 << 5, z 3 << 10, width - 1 = 3 << 15, height - 1 = 4 << 20,
    // face 3 (-y) << 25; the spare bits 0.
    CHECK(quad.geometry == 0x0641'8C41U);
    // texture 0x1234, then corners 0 to 3 at 2 bits each from bit 16:
    // 0b11'10'01'00, so 0xE4.
    CHECK(quad.material == 0x00E4'1234U);
}

TEST_CASE("every field of a quad comes back from its encoding, at its limits") {
    int checked = 0;
    for (const int x : {0, 31}) {
        for (const int y : {0, 31}) {
            for (const int z : {0, 31}) {
                for (const int width : {1, 32}) {
                    for (const int height : {1, 32}) {
                        for (const vx::Face face : faces) {
                            for (const std::uint16_t texture :
                                 {std::uint16_t{0}, std::uint16_t{0xFFFF}}) {
                                for (const std::array<std::uint8_t, 4> occlusion :
                                     {std::array<std::uint8_t, 4>{0, 1, 2, 3},
                                      std::array<std::uint8_t, 4>{3, 2, 1, 0}}) {
                                    const vx::QuadFields fields{.block = {.x = x, .y = y, .z = z},
                                                                .width = width,
                                                                .height = height,
                                                                .face = face,
                                                                .texture = texture,
                                                                .occlusion = occlusion};
                                    const vx::QuadFields back =
                                        vx::decode_quad(vx::encode_quad(fields));
                                    if (back != fields) {
                                        INFO("x ", x, " y ", y, " z ", z, " width ", width,
                                             " height ", height, " face ", static_cast<int>(face),
                                             " texture ", texture);
                                        CHECK(back == fields);
                                    }
                                    ++checked;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(checked == 768);
}

TEST_CASE("every field of a quad comes back from its encoding, on random quads") {
    std::mt19937 generator(3);
    std::uniform_int_distribution<int> position(0, 31);
    std::uniform_int_distribution<int> size(1, 32);
    std::uniform_int_distribution<int> face(0, 5);
    std::uniform_int_distribution<int> texture(0, 0xFFFF);
    std::uniform_int_distribution<int> occlusion(0, 3);
    for (int round = 0; round < 100'000; ++round) {
        vx::QuadFields fields{
            .block = {.x = position(generator), .y = position(generator), .z = position(generator)},
            .width = size(generator),
            .height = size(generator),
            .face = static_cast<vx::Face>(face(generator)),
            .texture = static_cast<std::uint16_t>(texture(generator)),
            .occlusion = {}};
        for (std::uint8_t& corner : fields.occlusion) {
            corner = static_cast<std::uint8_t>(occlusion(generator));
        }
        if (vx::decode_quad(vx::encode_quad(fields)) != fields) {
            INFO("round ", round);
            CHECK(vx::decode_quad(vx::encode_quad(fields)) == fields);
            break;
        }
    }
}

TEST_CASE("decoding leaves the spare bits out") {
    const vx::QuadFields fields{.block = {.x = 7, .y = 8, .z = 9},
                                .width = 10,
                                .height = 11,
                                .face = vx::Face::pos_z,
                                .texture = 12,
                                .occlusion = {1, 3, 0, 2}};
    vx::Quad quad = vx::encode_quad(fields);
    CHECK((quad.geometry >> 28U) == 0);
    CHECK((quad.material >> 24U) == 0);
    quad.geometry |= 0xF000'0000U;
    quad.material |= 0xFF00'0000U;
    CHECK(vx::decode_quad(quad) == fields);
}

TEST_CASE("each face's quads run along the axes RFC 0003 gives it") {
    // A quad 4 wide and 5 tall on the block at (1, 2, 3). Width runs along z
    // on x faces and along x otherwise; height along z on y faces and along y
    // otherwise. A face looking along a positive axis lies one block further.
    struct Expected {
        vx::Face face;
        std::array<std::array<int, 3>, 4> corners;
    };
    const std::array<Expected, 6> table{{
        {.face = vx::Face::pos_x, .corners = {{{2, 2, 3}, {2, 2, 7}, {2, 7, 7}, {2, 7, 3}}}},
        {.face = vx::Face::neg_x, .corners = {{{1, 2, 3}, {1, 2, 7}, {1, 7, 7}, {1, 7, 3}}}},
        {.face = vx::Face::pos_y, .corners = {{{1, 3, 3}, {5, 3, 3}, {5, 3, 8}, {1, 3, 8}}}},
        {.face = vx::Face::neg_y, .corners = {{{1, 2, 3}, {5, 2, 3}, {5, 2, 8}, {1, 2, 8}}}},
        {.face = vx::Face::pos_z, .corners = {{{1, 2, 4}, {5, 2, 4}, {5, 7, 4}, {1, 7, 4}}}},
        {.face = vx::Face::neg_z, .corners = {{{1, 2, 3}, {5, 2, 3}, {5, 7, 3}, {1, 7, 3}}}},
    }};
    for (const Expected& expected : table) {
        const vx::QuadFields quad =
            vx::decode_quad(vx::encode_quad({.block = {.x = 1, .y = 2, .z = 3},
                                             .width = 4,
                                             .height = 5,
                                             .face = expected.face,
                                             .texture = 0,
                                             .occlusion = {3, 3, 3, 3}}));
        for (int corner = 0; corner < 4; ++corner) {
            INFO("face ", static_cast<int>(expected.face), " corner ", corner);
            CHECK(vx::quad_corner(quad, corner) ==
                  expected.corners[static_cast<std::size_t>(corner)]);
        }
    }
}

TEST_CASE("the +x, +y and +z faces of a block at 31 lie at 32") {
    const auto corner_0 = [](vx::Face face) {
        return vx::quad_corner(
            vx::decode_quad(vx::encode_quad({.block = {.x = 31, .y = 31, .z = 31},
                                             .width = 1,
                                             .height = 1,
                                             .face = face,
                                             .texture = 0,
                                             .occlusion = {3, 3, 3, 3}})),
            0);
    };
    CHECK(corner_0(vx::Face::pos_x) == std::array{32, 31, 31});
    CHECK(corner_0(vx::Face::pos_y) == std::array{31, 32, 31});
    CHECK(corner_0(vx::Face::pos_z) == std::array{31, 31, 32});
    CHECK(corner_0(vx::Face::neg_x) == std::array{31, 31, 31});
    // And a quad as large as a chunk reaches its far side.
    const vx::QuadFields whole_face =
        vx::decode_quad(vx::encode_quad({.block = {.x = 0, .y = 31, .z = 0},
                                         .width = 32,
                                         .height = 32,
                                         .face = vx::Face::pos_y,
                                         .texture = 0,
                                         .occlusion = {3, 3, 3, 3}}));
    CHECK(vx::quad_corner(whole_face, 2) == std::array{32, 32, 32});
}
