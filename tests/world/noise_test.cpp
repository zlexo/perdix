#include <vx/core/random.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/lod.hpp>
#include <vx/world/noise.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// The known answers come from KdotJPG's OpenSimplex2.java itself (at
// 4cd120d, the file this port follows), run on a JVM, whose float and double
// arithmetic is IEEE 754 with no fused multiply-add. Past the reference's
// 32-bit lattice indices, from the same file with only those indices
// widened to long; there the unchanged reference returns infinities or
// zero. fBm from a Java model of the rules in noise.hpp, written apart from
// this code, over the widened reference.

namespace {

struct Answer2 {
    std::uint64_t seed;
    double x;
    double z;
    std::uint32_t bits;
};

struct Answer3 {
    std::uint64_t seed;
    double x;
    double y;
    double z;
    std::uint32_t bits;
};

struct FbmAnswer2 {
    int octaves;
    double x;
    double z;
    std::uint32_t bits;
};

struct FbmAnswer3 {
    int octaves;
    double x;
    double y;
    double z;
    std::uint32_t bits;
};

constexpr Answer2 near_2d[] = {
    {0x0000000000000000ULL, 0.0, 0.0, 0x00000000U},
    {0x0000000000000000ULL, -1.75, 3.125, 0xBF382AB7U},
    {0x0000000000000000ULL, 1000.001, 2000.002, 0x3E6EA32CU},
    {0x0000000000000000ULL, 1500000.0, -2500000.0, 0xBE7822FCU},
    {0x0000000000000000ULL, -7.25E8, -3.5E8, 0x3E537CCCU},
    {0x0000000000000001ULL, 0.5, 0.25, 0xBF126B51U},
    {0x0000000000000001ULL, 12.3, -45.6, 0x3E8CC3CDU},
    {0x0000000000000001ULL, -123456.789, 98765.4321, 0xBE806302U},
    {0x0000000000000001ULL, 3.0E8, 7.0E8, 0x3F03DF19U},
    {0x0000000000005EEDULL, 0.0, 0.0, 0x80000000U},
    {0x0000000000005EEDULL, 0.5, 0.25, 0x3F3037C1U},
    {0x0000000000005EEDULL, -1.75, 3.125, 0xBE837F1BU},
    {0x0000000000005EEDULL, 12.3, -45.6, 0xBF45EBDEU},
    {0x0000000000005EEDULL, 1000.001, 2000.002, 0xBEB88CBBU},
    {0x0000000000005EEDULL, -123456.789, 98765.4321, 0x3EF61722U},
    {0x0000000000005EEDULL, 1500000.0, -2500000.0, 0xBF53BEBEU},
    {0x0000000000005EEDULL, 3.0E8, 7.0E8, 0xBF58AAAFU},
    {0x0000000000005EEDULL, -7.25E8, -3.5E8, 0xBEA07B10U},
    {0xFFFFFFFFFFFFFFFFULL, 0.5, 0.25, 0xBEED1135U},
    {0xFFFFFFFFFFFFFFFFULL, 12.3, -45.6, 0x3E8CC1F5U},
    {0xFFFFFFFFFFFFFFFFULL, -123456.789, 98765.4321, 0xBE83623BU},
    {0xFFFFFFFFFFFFFFFFULL, 3.0E8, 7.0E8, 0x3F03DF19U},
    {0x123456789ABCDEF0ULL, 0.0, 0.0, 0x80000000U},
    {0x123456789ABCDEF0ULL, -1.75, 3.125, 0xBF16903BU},
    {0x123456789ABCDEF0ULL, 1000.001, 2000.002, 0x3EB6E097U},
    {0x123456789ABCDEF0ULL, 1500000.0, -2500000.0, 0x3DA4BCE8U},
    {0x123456789ABCDEF0ULL, -7.25E8, -3.5E8, 0x3E537CCCU},
};

constexpr Answer3 near_3d[] = {
    {0x0000000000000000ULL, 0.0, 0.0, 0.0, 0x00000000U},
    {0x0000000000000000ULL, -1.75, 3.125, -0.0625, 0x3E00AAC0U},
    {0x0000000000000000ULL, 1000.001, 64.5, 2000.002, 0x3F3E4D3BU},
    {0x0000000000000000ULL, 1500000.0, 250.0, -2500000.0, 0xBF1C7D52U},
    {0x0000000000000000ULL, -7.25E8, 4.0E7, -3.5E8, 0x3F0B7517U},
    {0x0000000000000001ULL, 0.5, 0.25, 0.125, 0x3E8108AEU},
    {0x0000000000000001ULL, 12.3, -45.6, 78.9, 0xBE061228U},
    {0x0000000000000001ULL, -123456.789, -300.25, 98765.4321, 0x3DF1F3CEU},
    {0x0000000000000001ULL, 3.0E8, -1.0E8, 7.0E8, 0x3EDB9E79U},
    {0x0000000000005EEDULL, 0.0, 0.0, 0.0, 0x00000000U},
    {0x0000000000005EEDULL, 0.5, 0.25, 0.125, 0x3DD4202CU},
    {0x0000000000005EEDULL, -1.75, 3.125, -0.0625, 0xBE6C2B89U},
    {0x0000000000005EEDULL, 12.3, -45.6, 78.9, 0x3E9D4EBCU},
    {0x0000000000005EEDULL, 1000.001, 64.5, 2000.002, 0x3EDF95D1U},
    {0x0000000000005EEDULL, -123456.789, -300.25, 98765.4321, 0x3E51FB97U},
    {0x0000000000005EEDULL, 1500000.0, 250.0, -2500000.0, 0xBD8479DAU},
    {0x0000000000005EEDULL, 3.0E8, -1.0E8, 7.0E8, 0xBF11532CU},
    {0x0000000000005EEDULL, -7.25E8, 4.0E7, -3.5E8, 0xBF0B7517U},
    {0xFFFFFFFFFFFFFFFFULL, 0.5, 0.25, 0.125, 0x3DB9E25CU},
    {0xFFFFFFFFFFFFFFFFULL, 12.3, -45.6, 78.9, 0x3ECF9A96U},
    {0xFFFFFFFFFFFFFFFFULL, -123456.789, -300.25, 98765.4321, 0x3DDE2A50U},
    {0xFFFFFFFFFFFFFFFFULL, 3.0E8, -1.0E8, 7.0E8, 0x3E3459A3U},
    {0x123456789ABCDEF0ULL, 0.0, 0.0, 0.0, 0x00000000U},
    {0x123456789ABCDEF0ULL, -1.75, 3.125, -0.0625, 0xBEA31655U},
    {0x123456789ABCDEF0ULL, 1000.001, 64.5, 2000.002, 0x3F3A2898U},
    {0x123456789ABCDEF0ULL, 1500000.0, 250.0, -2500000.0, 0xBF2ACDDBU},
    {0x123456789ABCDEF0ULL, -7.25E8, 4.0E7, -3.5E8, 0x3F406143U},
};

constexpr Answer3 far_3d[] = {
    {0x0000000000005EEDULL, 2.147483647E9, 0.0, 2.147483647E9,
     0xBC4F64D8U}, // int reference: 0xFF800000U
    {0x0000000000005EEDULL, -2.147483648E9, 1000.5, -2.147483648E9,
     0x3EB0AB9CU},                                               // int reference: 0xFF800000U
    {0x0000000000005EEDULL, 3.0E9, -2.0E9, 3.5E9, 0xBF14E3C8U},  // int reference: 0xFF800000U
    {0x0000000000005EEDULL, -3.9E9, 2.1E9, 1.25E9, 0x3DCBD8D1U}, // int reference: 0x7F800000U
    {0x0000000000005EEDULL, 2.147483647E9, 2.147483647E9, 2.147483647E9,
     0xBEBDD37DU}, // int reference: 0x00000000U
};

constexpr Answer2 far_2d[] = {
    {0x0000000000005EEDULL, 2.147483647E9, 2.147483647E9,
     0x3DE12AEAU},                                        // int reference: 0x00000000U
    {0x0000000000005EEDULL, -3.9E9, 1.25E9, 0x3C9C04D9U}, // int reference: 0x00000000U
    {0x0000000000005EEDULL, 4.0E9, -3.3E9, 0x3EA2CE8AU},  // int reference: 0x00000000U
};

// Seed 0x5EED, 1/64 cycles a block, 4 octaves, lacunarity 2, gain 0.5.
constexpr FbmAnswer3 fbm_3d[] = {
    {4, 0.0, 0.0, 0.0, 0x00000000U},
    {4, 17.0, 64.0, -33.0, 0xBD2F77A8U},
    {4, -1000.5, 72.5, 4096.5, 0xBD4A7E7DU},
    {4, 1.073741824E9, 1024.0, -1.073741824E9, 0xBC8F764FU},
    {2, 0.0, 0.0, 0.0, 0x00000000U},
    {2, 17.0, 64.0, -33.0, 0xBE212DE8U},
    {2, -1000.5, 72.5, 4096.5, 0x3CA171FCU},
    {2, 1.073741824E9, 1024.0, -1.073741824E9, 0x3CF49E92U},
    {0, 0.0, 0.0, 0.0, 0x00000000U},
    {0, 17.0, 64.0, -33.0, 0x00000000U},
    {0, -1000.5, 72.5, 4096.5, 0x00000000U},
    {0, 1.073741824E9, 1024.0, -1.073741824E9, 0x00000000U},
};

// Seed 7, 0.0123 cycles a block, 5 octaves, lacunarity 1.9, gain 0.55: no
// step of it is exact in binary.
constexpr FbmAnswer3 fbm_3d_inexact[] = {
    {5, 0.0, 0.0, 0.0, 0x00000000U},
    {5, 17.0, 64.0, -33.0, 0x3D2BA610U},
    {5, -1000.5, 72.5, 4096.5, 0xBE821E56U},
    {5, 1.073741824E9, 1024.0, -1.073741824E9, 0x3E47865EU},
};

// Seed 0x5EED, 1/256 cycles a block, 6 octaves, lacunarity 2, gain 0.5.
constexpr FbmAnswer2 fbm_2d[] = {
    {6, 0.0, 0.0, 0x00000000U},        {6, 17.0, -33.0, 0x3EB3EDDAU},
    {6, -1000.5, 4096.5, 0xBD973A16U}, {6, 1.0737418245E9, -1.07374182325E9, 0xBC87BB6CU},
    {3, 0.0, 0.0, 0x00000000U},        {3, 17.0, -33.0, 0x3EAACC35U},
    {3, -1000.5, 4096.5, 0xBDB1F992U}, {3, 1.0737418245E9, -1.07374182325E9, 0xBA42E701U},
};

std::uint32_t bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

// A double in [low, high) from the top 53 bits of a stream.
double uniform(vx::Rng& rng, double low, double high) {
    const double unit = static_cast<double>(rng.next() >> 11U) * 0x1.0p-53;
    return low + unit * (high - low);
}

} // namespace

TEST_CASE("simplex2 and simplex3 give the reference's bits") {
    for (const Answer2& answer : near_2d) {
        CAPTURE(answer.seed);
        CAPTURE(answer.x);
        CAPTURE(answer.z);
        CHECK(bits(vx::simplex2(answer.seed, answer.x, answer.z)) == answer.bits);
    }
    for (const Answer3& answer : near_3d) {
        CAPTURE(answer.seed);
        CAPTURE(answer.x);
        CAPTURE(answer.y);
        CAPTURE(answer.z);
        CHECK(bits(vx::simplex3(answer.seed, answer.x, answer.y, answer.z)) == answer.bits);
    }
}

TEST_CASE("past 2 to the 31 in noise space the 64-bit lattice gives the widened reference's bits") {
    for (const Answer3& answer : far_3d) {
        CAPTURE(answer.x);
        CAPTURE(answer.y);
        CAPTURE(answer.z);
        CHECK(bits(vx::simplex3(answer.seed, answer.x, answer.y, answer.z)) == answer.bits);
    }
    for (const Answer2& answer : far_2d) {
        CAPTURE(answer.x);
        CAPTURE(answer.z);
        CHECK(bits(vx::simplex2(answer.seed, answer.x, answer.z)) == answer.bits);
    }
}

TEST_CASE("fBm sums its octaves as the model does, the short ones left out on request") {
    const vx::Fbm3 exact{0x5EED, 1.0 / 64, 4};
    CHECK(exact.count() == 4);
    for (const FbmAnswer3& answer : fbm_3d) {
        CAPTURE(answer.octaves);
        CAPTURE(answer.x);
        CAPTURE(answer.y);
        CAPTURE(answer.z);
        CHECK(bits(exact.sample(answer.x, answer.y, answer.z, answer.octaves)) == answer.bits);
    }
    const vx::Fbm3 inexact{7, 0.0123, 5, 1.9, 0.55F};
    for (const FbmAnswer3& answer : fbm_3d_inexact) {
        CAPTURE(answer.x);
        CAPTURE(answer.y);
        CAPTURE(answer.z);
        CHECK(bits(inexact.sample(answer.x, answer.y, answer.z)) == answer.bits);
    }
    const vx::Fbm2 climate{0x5EED, 1.0 / 256, 6};
    for (const FbmAnswer2& answer : fbm_2d) {
        CAPTURE(answer.octaves);
        CAPTURE(answer.x);
        CAPTURE(answer.z);
        CHECK(bits(climate.sample(answer.x, answer.z, answer.octaves)) == answer.bits);
    }
    // Asking for more octaves than there are sums them all; the count is
    // clamped to 1 to max_octaves.
    CHECK(bits(exact.sample(17, 64, -33, 9)) == bits(exact.sample(17, 64, -33)));
    CHECK(vx::Fbm3{1, 0.01, 0}.count() == 1);
    CHECK(vx::Fbm3{1, 0.01, 99}.count() == vx::Octaves::max_octaves);
}

TEST_CASE("noise stays within -1 to 1 with no NaN and reaches near both ends") {
    // Points spread over the engine's whole range of blocks, at frequencies
    // from a cycle a block down to one in 4,096, and straight into noise
    // space up to 4 x 10^9 cycles out.
    vx::Rng rng{vx::stream_seed(1, vx::salt("test.noise.range"))};
    const double frequencies[] = {1.0, 0.25, 1.0 / 64, 0.0123, 1.0 / 4096};
    float low2 = 1;
    float high2 = -1;
    float low3 = 1;
    float high3 = -1;
    bool all_finite = true;
    for (int i = 0; i < 400'000; ++i) {
        const std::uint64_t seed = rng.next();
        const double frequency = frequencies[rng.below(5)];
        const double reach = (i % 2 == 0) ? 0x1.0p31 * frequency : 4.0e9;
        const double x = uniform(rng, -reach, reach);
        const double y = uniform(rng, -reach, reach);
        const double z = uniform(rng, -reach, reach);
        const float two = vx::simplex2(seed, x, z);
        const float three = vx::simplex3(seed, x, y, z);
        all_finite = all_finite && std::isfinite(two) && std::isfinite(three);
        low2 = std::min(low2, two);
        high2 = std::max(high2, two);
        low3 = std::min(low3, three);
        high3 = std::max(high3, three);
    }
    CHECK(all_finite);
    CHECK(low2 >= -1.0F);
    CHECK(high2 <= 1.0F);
    CHECK(low3 >= -1.0F);
    CHECK(high3 <= 1.0F);
    // The reference's normalizers put its extremes at about 1: over 20
    // million points, 2D from -0.9999997 to 0.9999992, 3D from -0.99931 to
    // 0.99996. A wrong normalizer shows here as well as above.
    CHECK(low2 < -0.9F);
    CHECK(high2 > 0.9F);
    CHECK(low3 < -0.9F);
    CHECK(high3 > 0.9F);

    const vx::Fbm3 base{0x5EED, 1.0 / 64, 4};
    const vx::Fbm2 climate{0x5EED, 1.0 / 256, 6};
    bool fbm_in_range = true;
    for (int i = 0; i < 100'000; ++i) {
        const double x = uniform(rng, -0x1.0p31, 0x1.0p31);
        const double y = uniform(rng, -0x1.0p31, 0x1.0p31);
        const double z = uniform(rng, -0x1.0p31, 0x1.0p31);
        const float three = base.sample(x, y, z);
        const float two = climate.sample(x, z);
        fbm_in_range = fbm_in_range && std::isfinite(three) && std::isfinite(two) &&
                       std::abs(three) <= 1.0F && std::abs(two) <= 1.0F;
    }
    CHECK(fbm_in_range);
}

namespace {

// RFC 0008's roughness: the RMS of the second difference along rows of
// samples a block apart, 1,024 to a row, 64 rows from `start`, 37 blocks
// apart in z (and in y for the 3D field's vertical rows), so far from the
// origin and near it the measure sees different noise of the same kind.
double roughness_3d(const vx::Fbm3& field, double x, double y, double z, bool vertical) {
    constexpr int row = 1024;
    constexpr int rows = 64;
    std::vector<float> values(row);
    double sum = 0;
    for (int r = 0; r < rows; ++r) {
        const double offset = 37.0 * r;
        const vx::Lattice3 lattice =
            vertical ? vx::Lattice3{.x = x + offset, .y = y, .z = z, .nx = 1, .ny = row, .nz = 1}
                     : vx::Lattice3{.x = x, .y = y, .z = z + offset, .nx = row, .ny = 1, .nz = 1};
        field.fill(lattice, values);
        for (std::size_t i = 1; i + 1 < values.size(); ++i) {
            const double second = static_cast<double>(values[i - 1]) -
                                  2.0 * static_cast<double>(values[i]) +
                                  static_cast<double>(values[i + 1]);
            sum += second * second;
        }
    }
    return std::sqrt(sum / (static_cast<double>(rows) * (row - 2)));
}

double roughness_2d(const vx::Fbm2& field, double x, double z) {
    constexpr int row = 1024;
    constexpr int rows = 64;
    std::vector<float> values(row);
    double sum = 0;
    for (int r = 0; r < rows; ++r) {
        field.fill({.x = x, .z = z + 37.0 * r, .nx = row, .nz = 1}, values);
        for (std::size_t i = 1; i + 1 < values.size(); ++i) {
            const double second = static_cast<double>(values[i - 1]) -
                                  2.0 * static_cast<double>(values[i]) +
                                  static_cast<double>(values[i + 1]);
            sum += second * second;
        }
    }
    return std::sqrt(sum / (static_cast<double>(rows) * (row - 2)));
}

} // namespace

TEST_CASE("noise at 2 to the 30 blocks is as smooth as at the origin, within 10 percent") {
    constexpr double far = 0x1.0p30;
    const vx::Fbm3 base{0x5EED, 1.0 / 64, 4};
    for (const bool vertical : {false, true}) {
        CAPTURE(vertical);
        const double origin = roughness_3d(base, 0, 0, 0, vertical);
        CAPTURE(origin);
        CHECK(origin > 0);
        for (const double sign : {1.0, -1.0}) {
            const double out = roughness_3d(base, sign * far, sign * far, -sign * far, vertical);
            CAPTURE(out);
            CHECK(std::abs(out / origin - 1.0) < 0.1);
        }
    }
    const vx::Fbm2 climate{0x5EED, 1.0 / 64, 4};
    const double origin = roughness_2d(climate, 0, 0);
    CAPTURE(origin);
    for (const double sign : {1.0, -1.0}) {
        const double out = roughness_2d(climate, sign * far, -sign * far);
        CAPTURE(out);
        CHECK(std::abs(out / origin - 1.0) < 0.1);
    }
}

namespace {

// A chunk's density corners at a level: 9 by 6 by 9 of them, 4 by 8 by 4
// cells apart, at cell middles (RFC 0008's Generating at a level).
vx::Lattice3 corners(vx::LodPos chunk) {
    const double cell = vx::cell_blocks(chunk.level);
    const double side = vx::chunk_size * cell;
    return {
        .x = chunk.x * side + 0.5 * cell,
        .y = chunk.y * side + 0.5 * cell,
        .z = chunk.z * side + 0.5 * cell,
        .step_x = 4 * cell,
        .step_y = 8 * cell,
        .step_z = 4 * cell,
        .nx = 9,
        .ny = 6,
        .nz = 9,
    };
}

std::size_t corner_index(int x, int y, int z) {
    return static_cast<std::size_t>(x + 9 * (z + 9 * y));
}

std::vector<float> fill(const vx::Fbm3& field, const vx::Lattice3& lattice) {
    std::vector<float> values(lattice.size());
    field.fill(lattice, values);
    return values;
}

// The chunks the seam tests compare: near the origin, either side of it,
// and the last whole chunks before 2^30 blocks at each level.
std::vector<vx::LodPos> seam_chunks() {
    std::vector<vx::LodPos> chunks;
    for (std::uint8_t level = 0; level < vx::lod_levels; ++level) {
        const std::int32_t far = (std::int32_t{1} << (30 - vx::chunk_bits - level)) - 2;
        chunks.push_back({0, 0, 0, level});
        chunks.push_back({-1, -1, -1, level});
        chunks.push_back({3, -2, -5, level});
        chunks.push_back({far, far, far, level});
        chunks.push_back({-far, far, -far, level});
    }
    return chunks;
}

} // namespace

TEST_CASE("the lattice call gives sample's bits at every point") {
    // A frequency and lacunarity with no exact steps, as the parameter file
    // will hold, and an exact one.
    for (const vx::Fbm3& field :
         {vx::Fbm3{7, 0.0123, 5, 1.9, 0.55F}, vx::Fbm3{0x5EED, 1.0 / 64, 4}}) {
        for (const vx::LodPos chunk : seam_chunks()) {
            CAPTURE(chunk.x);
            CAPTURE(chunk.level);
            const vx::Lattice3 lattice = corners(chunk);
            for (const int octaves : {field.count(), 2}) {
                std::vector<float> values(lattice.size());
                field.fill(lattice, values, octaves);
                int wrong = 0;
                for (int y = 0; y < lattice.ny; ++y) {
                    for (int z = 0; z < lattice.nz; ++z) {
                        for (int x = 0; x < lattice.nx; ++x) {
                            const float one = field.sample(lattice.x + x * lattice.step_x,
                                                           lattice.y + y * lattice.step_y,
                                                           lattice.z + z * lattice.step_z, octaves);
                            wrong += bits(one) != bits(values[corner_index(x, y, z)]) ? 1 : 0;
                        }
                    }
                }
                CHECK(wrong == 0);
            }
        }
    }
    const vx::Fbm2 climate{7, 0.0123, 5, 1.9, 0.55F};
    const vx::Lattice2 columns{
        .x = -1'000'000.5, .z = 0x1.0p29 + 2.5, .step_x = 16, .step_z = 16, .nx = 9, .nz = 9};
    std::vector<float> values(columns.size());
    climate.fill(columns, values);
    int wrong = 0;
    for (int z = 0; z < columns.nz; ++z) {
        for (int x = 0; x < columns.nx; ++x) {
            const float one =
                climate.sample(columns.x + x * columns.step_x, columns.z + z * columns.step_z);
            wrong += bits(one) != bits(values[static_cast<std::size_t>(x + 9 * z)]) ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("corners that neighboring chunks share are bit-identical at every level") {
    const vx::Fbm3 field{7, 0.0123, 5, 1.9, 0.55F};
    for (const vx::LodPos chunk : seam_chunks()) {
        CAPTURE(chunk.x);
        CAPTURE(chunk.y);
        CAPTURE(chunk.z);
        CAPTURE(chunk.level);
        const std::vector<float> here = fill(field, corners(chunk));
        const std::vector<float> east =
            fill(field, corners({chunk.x + 1, chunk.y, chunk.z, chunk.level}));
        const std::vector<float> south =
            fill(field, corners({chunk.x, chunk.y, chunk.z + 1, chunk.level}));
        const std::vector<float> above =
            fill(field, corners({chunk.x, chunk.y + 1, chunk.z, chunk.level}));
        int shared = 0;
        int wrong = 0;
        for (int a = 0; a < 9; ++a) {
            for (int y = 0; y < 6; ++y) {
                // The east face is the neighbor's west face, and so on.
                wrong +=
                    bits(here[corner_index(8, y, a)]) != bits(east[corner_index(0, y, a)]) ? 1 : 0;
                wrong +=
                    bits(here[corner_index(a, y, 8)]) != bits(south[corner_index(a, y, 0)]) ? 1 : 0;
                shared += 2;
            }
            for (int b = 0; b < 9; ++b) {
                // The top corners and the apron above them are the
                // neighbor's two bottom layers.
                wrong +=
                    bits(here[corner_index(a, 4, b)]) != bits(above[corner_index(a, 0, b)]) ? 1 : 0;
                wrong +=
                    bits(here[corner_index(a, 5, b)]) != bits(above[corner_index(a, 1, b)]) ? 1 : 0;
                shared += 2;
            }
        }
        CHECK(shared == 9 * 6 * 2 + 9 * 9 * 2);
        CHECK(wrong == 0);
    }
}

TEST_CASE("climate columns that neighboring chunk columns share are bit-identical at every level") {
    const vx::Fbm2 climate{7, 0.0123, 5, 1.9, 0.55F};
    for (const vx::LodPos chunk : seam_chunks()) {
        CAPTURE(chunk.x);
        CAPTURE(chunk.z);
        CAPTURE(chunk.level);
        const double cell = vx::cell_blocks(chunk.level);
        const double side = vx::chunk_size * cell;
        const auto columns = [&](std::int32_t x, std::int32_t z) {
            const vx::Lattice2 lattice{.x = x * side + 0.5 * cell,
                                       .z = z * side + 0.5 * cell,
                                       .step_x = 4 * cell,
                                       .step_z = 4 * cell,
                                       .nx = 9,
                                       .nz = 9};
            std::vector<float> values(lattice.size());
            climate.fill(lattice, values);
            return values;
        };
        const std::vector<float> here = columns(chunk.x, chunk.z);
        const std::vector<float> east = columns(chunk.x + 1, chunk.z);
        const std::vector<float> south = columns(chunk.x, chunk.z + 1);
        int wrong = 0;
        for (std::size_t a = 0; a < 9; ++a) {
            wrong += bits(here[8 + 9 * a]) != bits(east[0 + 9 * a]) ? 1 : 0;
            wrong += bits(here[a + 9 * 8]) != bits(south[a]) ? 1 : 0;
        }
        CHECK(wrong == 0);
    }
}
