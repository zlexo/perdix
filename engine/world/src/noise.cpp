#include <vx/world/noise.hpp>

#include <vx/core/random.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vx {
namespace {

// The reference's constants, as OpenSimplex2.java writes them (KdotJPG's
// OpenSimplex2 at 4cd120d, 2024-01-29, CC0). Every expression below keeps
// the reference's operations and their order, so each rounds as the
// reference's does: Java's float and double arithmetic is IEEE 754 with no
// fused multiply-add, which RFC 0008's float rules give us too.
constexpr std::uint64_t prime_x = 0x5205'402B'9270'C86FULL;
constexpr std::uint64_t prime_y = 0x598C'D327'0038'17B5ULL;
constexpr std::uint64_t prime_z = 0x5BCC'226E'9FA0'BACBULL;
constexpr std::uint64_t hash_multiplier = 0x53A3'F72D'EEC5'46F5ULL;
constexpr std::uint64_t seed_flip_3d =
    0ULL - 0x52D5'47B2'E96E'D629ULL; // the reference's -0x52D5...

constexpr double skew_2d = 0.366025403784439;
constexpr double unskew_2d = -0.21132486540518713;
// The reference's value, which differs from std::numbers::inv_sqrt3 in
// its last bit.
constexpr double root3_over_3 = 0.577350269189626; // NOLINT(modernize-use-std-numbers)
constexpr double rotate_3d_orthogonalizer = unskew_2d;

constexpr int grads_2d_exponent = 7;
constexpr int grads_3d_exponent = 8;
constexpr std::size_t grads_2d = std::size_t{1} << grads_2d_exponent;
constexpr std::size_t grads_3d = std::size_t{1} << grads_3d_exponent;

constexpr double normalizer_2d = 0.01001634121365712;
constexpr double normalizer_3d = 0.07969837668935331;

constexpr float rsquared_2d = 0.5F;
constexpr float rsquared_3d = 0.6F;

// The reference's float casts of double expressions, computed in double as
// it computes them.
constexpr auto a1_t_2d = static_cast<float>(2 * (1 + (2 * unskew_2d)) * ((1 / unskew_2d) + 2));
constexpr auto a1_base_2d = static_cast<float>(-2 * (1 + (2 * unskew_2d)) * (1 + (2 * unskew_2d)));
constexpr auto d1_2d = static_cast<float>(1 + (2 * unskew_2d));
constexpr auto unskew_2d_f = static_cast<float>(unskew_2d);
constexpr auto unskew_2d_plus_1_f = static_cast<float>(unskew_2d + 1);

// The reference's gradients, copied from its source by script, a row per
// gradient as the reference lays them out.
// clang-format off
constexpr std::array<float, 48> grad2_source{
    0.38268343236509F, 0.923879532511287F,
    0.923879532511287F, 0.38268343236509F,
    0.923879532511287F, -0.38268343236509F,
    0.38268343236509F, -0.923879532511287F,
    -0.38268343236509F, -0.923879532511287F,
    -0.923879532511287F, -0.38268343236509F,
    -0.923879532511287F, 0.38268343236509F,
    -0.38268343236509F, 0.923879532511287F,
    0.130526192220052F, 0.99144486137381F,
    0.608761429008721F, 0.793353340291235F,
    0.793353340291235F, 0.608761429008721F,
    0.99144486137381F, 0.130526192220051F,
    0.99144486137381F, -0.130526192220051F,
    0.793353340291235F, -0.60876142900872F,
    0.608761429008721F, -0.793353340291235F,
    0.130526192220052F, -0.99144486137381F,
    -0.130526192220052F, -0.99144486137381F,
    -0.608761429008721F, -0.793353340291235F,
    -0.793353340291235F, -0.608761429008721F,
    -0.99144486137381F, -0.130526192220052F,
    -0.99144486137381F, 0.130526192220051F,
    -0.793353340291235F, 0.608761429008721F,
    -0.608761429008721F, 0.793353340291235F,
    -0.130526192220052F, 0.99144486137381F,
};

constexpr std::array<float, 192> grad3_source{
    2.22474487139F, 2.22474487139F, -1.0F, 0.0F,
    2.22474487139F, 2.22474487139F, 1.0F, 0.0F,
    3.0862664687972017F, 1.1721513422464978F, 0.0F, 0.0F,
    1.1721513422464978F, 3.0862664687972017F, 0.0F, 0.0F,
    -2.22474487139F, 2.22474487139F, -1.0F, 0.0F,
    -2.22474487139F, 2.22474487139F, 1.0F, 0.0F,
    -1.1721513422464978F, 3.0862664687972017F, 0.0F, 0.0F,
    -3.0862664687972017F, 1.1721513422464978F, 0.0F, 0.0F,
    -1.0F, -2.22474487139F, -2.22474487139F, 0.0F,
    1.0F, -2.22474487139F, -2.22474487139F, 0.0F,
    0.0F, -3.0862664687972017F, -1.1721513422464978F, 0.0F,
    0.0F, -1.1721513422464978F, -3.0862664687972017F, 0.0F,
    -1.0F, -2.22474487139F, 2.22474487139F, 0.0F,
    1.0F, -2.22474487139F, 2.22474487139F, 0.0F,
    0.0F, -1.1721513422464978F, 3.0862664687972017F, 0.0F,
    0.0F, -3.0862664687972017F, 1.1721513422464978F, 0.0F,
    -2.22474487139F, -2.22474487139F, -1.0F, 0.0F,
    -2.22474487139F, -2.22474487139F, 1.0F, 0.0F,
    -3.0862664687972017F, -1.1721513422464978F, 0.0F, 0.0F,
    -1.1721513422464978F, -3.0862664687972017F, 0.0F, 0.0F,
    -2.22474487139F, -1.0F, -2.22474487139F, 0.0F,
    -2.22474487139F, 1.0F, -2.22474487139F, 0.0F,
    -1.1721513422464978F, 0.0F, -3.0862664687972017F, 0.0F,
    -3.0862664687972017F, 0.0F, -1.1721513422464978F, 0.0F,
    -2.22474487139F, -1.0F, 2.22474487139F, 0.0F,
    -2.22474487139F, 1.0F, 2.22474487139F, 0.0F,
    -3.0862664687972017F, 0.0F, 1.1721513422464978F, 0.0F,
    -1.1721513422464978F, 0.0F, 3.0862664687972017F, 0.0F,
    -1.0F, 2.22474487139F, -2.22474487139F, 0.0F,
    1.0F, 2.22474487139F, -2.22474487139F, 0.0F,
    0.0F, 1.1721513422464978F, -3.0862664687972017F, 0.0F,
    0.0F, 3.0862664687972017F, -1.1721513422464978F, 0.0F,
    -1.0F, 2.22474487139F, 2.22474487139F, 0.0F,
    1.0F, 2.22474487139F, 2.22474487139F, 0.0F,
    0.0F, 3.0862664687972017F, 1.1721513422464978F, 0.0F,
    0.0F, 1.1721513422464978F, 3.0862664687972017F, 0.0F,
    2.22474487139F, -2.22474487139F, -1.0F, 0.0F,
    2.22474487139F, -2.22474487139F, 1.0F, 0.0F,
    1.1721513422464978F, -3.0862664687972017F, 0.0F, 0.0F,
    3.0862664687972017F, -1.1721513422464978F, 0.0F, 0.0F,
    2.22474487139F, -1.0F, -2.22474487139F, 0.0F,
    2.22474487139F, 1.0F, -2.22474487139F, 0.0F,
    3.0862664687972017F, 0.0F, -1.1721513422464978F, 0.0F,
    1.1721513422464978F, 0.0F, -3.0862664687972017F, 0.0F,
    2.22474487139F, -1.0F, 2.22474487139F, 0.0F,
    2.22474487139F, 1.0F, 2.22474487139F, 0.0F,
    1.1721513422464978F, 0.0F, 3.0862664687972017F, 0.0F,
    3.0862664687972017F, 0.0F, 1.1721513422464978F, 0.0F,
};
// clang-format on

// As the reference builds its tables: each value divided by the normalizer
// in double and rounded to float, the list repeated to fill the table.
template <std::size_t Size, std::size_t SourceSize>
constexpr std::array<float, Size> gradients(const std::array<float, SourceSize>& source,
                                            double normalizer) noexcept {
    std::array<float, Size> table{};
    for (std::size_t i = 0; i < Size; ++i) {
        table[i] = static_cast<float>(static_cast<double>(source[i % SourceSize]) / normalizer);
    }
    return table;
}

constexpr auto gradients_2d = gradients<grads_2d * 2>(grad2_source, normalizer_2d);
constexpr auto gradients_3d = gradients<grads_3d * 4>(grad3_source, normalizer_3d);

// Java's >> on a long, an arithmetic shift, which C++20 defines for signed
// values. Its sign bits reach the bits the gradient index takes.
constexpr std::uint64_t java_shift_right(std::uint64_t value, int shift) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value) >> shift);
}

// A lattice index times a prime, wrapping as Java's long multiply does. The
// index is 64-bit: the reference's is a 32-bit int.
constexpr std::uint64_t times(std::int64_t index, std::uint64_t prime) noexcept {
    return static_cast<std::uint64_t>(index) * prime;
}

float grad2(std::uint64_t seed, std::uint64_t xsvp, std::uint64_t ysvp, float dx,
            float dy) noexcept {
    std::uint64_t hash = seed ^ xsvp ^ ysvp;
    hash *= hash_multiplier;
    hash ^= java_shift_right(hash, 64 - grads_2d_exponent + 1);
    const auto gi = static_cast<std::size_t>(hash & ((grads_2d - 1) << 1U));
    return (gradients_2d[gi] * dx) + (gradients_2d[gi | 1U] * dy);
}

float grad3(std::uint64_t seed, std::uint64_t xrvp, std::uint64_t yrvp, std::uint64_t zrvp,
            float dx, float dy, float dz) noexcept {
    std::uint64_t hash = (seed ^ xrvp) ^ (yrvp ^ zrvp);
    hash *= hash_multiplier;
    hash ^= java_shift_right(hash, 64 - grads_3d_exponent + 2);
    const auto gi = static_cast<std::size_t>(hash & ((grads_3d - 1) << 2U));
    return (gradients_3d[gi] * dx) + (gradients_3d[gi | 1U] * dy) + (gradients_3d[gi | 2U] * dz);
}

// The reference's fastFloor and fastRound, returning 64 bits. Positions in
// noise space stay far inside them: the engine's 32-bit block positions
// times a frequency of at most a few cycles per block.
std::int64_t fast_floor(double x) noexcept {
    const auto xi = static_cast<std::int64_t>(x);
    return x < static_cast<double>(xi) ? xi - 1 : xi;
}

// Halves round away from zero, and 0.49999999999999994 rounds to 1, as in
// the reference, whose bits this keeps.
std::int64_t fast_round(double x) noexcept {
    // NOLINTNEXTLINE(bugprone-incorrect-roundings)
    return x < 0 ? static_cast<std::int64_t>(x - 0.5) : static_cast<std::int64_t>(x + 0.5);
}

// noise2_UnskewedBase.
float simplex2_base(std::uint64_t seed, double xs, double ys) noexcept {
    const std::int64_t xsb = fast_floor(xs);
    const std::int64_t ysb = fast_floor(ys);
    const auto xi = static_cast<float>(xs - static_cast<double>(xsb));
    const auto yi = static_cast<float>(ys - static_cast<double>(ysb));

    const std::uint64_t xsbp = times(xsb, prime_x);
    const std::uint64_t ysbp = times(ysb, prime_y);

    const float t = (xi + yi) * unskew_2d_f;
    const float dx0 = xi + t;
    const float dy0 = yi + t;

    float value = 0;
    const float a0 = rsquared_2d - (dx0 * dx0) - (dy0 * dy0);
    if (a0 > 0) {
        value = (a0 * a0) * (a0 * a0) * grad2(seed, xsbp, ysbp, dx0, dy0);
    }

    const float a1 = (a1_t_2d * t) + (a1_base_2d + a0);
    if (a1 > 0) {
        const float dx1 = dx0 - d1_2d;
        const float dy1 = dy0 - d1_2d;
        value += (a1 * a1) * (a1 * a1) * grad2(seed, xsbp + prime_x, ysbp + prime_y, dx1, dy1);
    }

    if (dy0 > dx0) {
        const float dx2 = dx0 - unskew_2d_f;
        const float dy2 = dy0 - unskew_2d_plus_1_f;
        const float a2 = rsquared_2d - (dx2 * dx2) - (dy2 * dy2);
        if (a2 > 0) {
            value += (a2 * a2) * (a2 * a2) * grad2(seed, xsbp, ysbp + prime_y, dx2, dy2);
        }
    } else {
        const float dx2 = dx0 - unskew_2d_plus_1_f;
        const float dy2 = dy0 - unskew_2d_f;
        const float a2 = rsquared_2d - (dx2 * dx2) - (dy2 * dy2);
        if (a2 > 0) {
            value += (a2 * a2) * (a2 * a2) * grad2(seed, xsbp + prime_x, ysbp, dx2, dy2);
        }
    }
    return value;
}

// noise2: the skew onto the A2* lattice.
float simplex2_at(std::uint64_t seed, double x, double y) noexcept {
    const double s = skew_2d * (x + y);
    return simplex2_base(seed, x + s, y + s);
}

// One of the two cubic lattices noise3_UnrotatedBase sums: its closest
// point to the position and the offset from it.
struct CubicLattice {
    std::uint64_t xp; // the closest point's indices times the primes
    std::uint64_t yp;
    std::uint64_t zp;
    float x; // the offset from it
    float y;
    float z;
    float ax; // the offset's absolute values
    float ay;
    float az;
    int x_sign; // -1 where the offset is positive or zero, 1 where negative
    int y_sign;
    int z_sign;
};

// The second closest point of a lattice: one step from the closest along
// the axis of the largest offset. Its share of the value, or zero.
float second_closest(std::uint64_t seed, float a, const CubicLattice& c) noexcept {
    if (c.ax >= c.ay && c.ax >= c.az) {
        float b = a + c.ax + c.ax;
        if (b > 1.0F) {
            b -= 1.0F;
            return (b * b) * (b * b) *
                   grad3(seed, c.xp - times(c.x_sign, prime_x), c.yp, c.zp,
                         c.x + static_cast<float>(c.x_sign), c.y, c.z);
        }
    } else if (c.ay > c.ax && c.ay >= c.az) {
        float b = a + c.ay + c.ay;
        if (b > 1.0F) {
            b -= 1.0F;
            return (b * b) * (b * b) *
                   grad3(seed, c.xp, c.yp - times(c.y_sign, prime_y), c.zp, c.x,
                         c.y + static_cast<float>(c.y_sign), c.z);
        }
    } else {
        float b = a + c.az + c.az;
        if (b > 1.0F) {
            b -= 1.0F;
            return (b * b) * (b * b) *
                   grad3(seed, c.xp, c.yp, c.zp - times(c.z_sign, prime_z), c.x, c.y,
                         c.z + static_cast<float>(c.z_sign));
        }
    }
    return 0;
}

// noise3_UnrotatedBase: two cubic lattices, offset by half a cell, form a
// body-centered cubic one.
float simplex3_base(std::uint64_t seed, double xr, double yr, double zr) noexcept {
    const std::int64_t xrb = fast_round(xr);
    const std::int64_t yrb = fast_round(yr);
    const std::int64_t zrb = fast_round(zr);
    CubicLattice c{};
    c.x = static_cast<float>(xr - static_cast<double>(xrb));
    c.y = static_cast<float>(yr - static_cast<double>(yrb));
    c.z = static_cast<float>(zr - static_cast<double>(zrb));

    c.x_sign = static_cast<int>(-1.0F - c.x) | 1;
    c.y_sign = static_cast<int>(-1.0F - c.y) | 1;
    c.z_sign = static_cast<int>(-1.0F - c.z) | 1;

    c.ax = static_cast<float>(c.x_sign) * -c.x;
    c.ay = static_cast<float>(c.y_sign) * -c.y;
    c.az = static_cast<float>(c.z_sign) * -c.z;

    c.xp = times(xrb, prime_x);
    c.yp = times(yrb, prime_y);
    c.zp = times(zrb, prime_z);

    float value = 0;
    float a = (rsquared_3d - (c.x * c.x)) - ((c.y * c.y) + (c.z * c.z));
    for (int l = 0;; ++l) {
        // The closest point of this lattice, then the second closest.
        if (a > 0) {
            value += (a * a) * (a * a) * grad3(seed, c.xp, c.yp, c.zp, c.x, c.y, c.z);
        }
        // The reference adds the second's share only where it is positive,
        // and the sum's bits do not change by adding zero: value is never
        // -0 here, since the shares are added to a value that starts at +0.
        value += second_closest(seed, a, c);

        if (l == 1) {
            break;
        }

        // Over to the other lattice.
        c.ax = 0.5F - c.ax;
        c.ay = 0.5F - c.ay;
        c.az = 0.5F - c.az;

        c.x = static_cast<float>(c.x_sign) * c.ax;
        c.y = static_cast<float>(c.y_sign) * c.ay;
        c.z = static_cast<float>(c.z_sign) * c.az;

        a += (0.75F - c.ax) - (c.ay + c.az);

        // The reference's (sign >> 1) & prime: the prime where the sign is -1.
        c.xp += c.x_sign < 0 ? prime_x : 0;
        c.yp += c.y_sign < 0 ? prime_y : 0;
        c.zp += c.z_sign < 0 ? prime_z : 0;

        c.x_sign = -c.x_sign;
        c.y_sign = -c.y_sign;
        c.z_sign = -c.z_sign;

        seed ^= seed_flip_3d;
    }
    return value;
}

// noise3_ImproveXZ: an orthonormal rotation, no skew, that points y up the
// lattice's main diagonal, so planes of constant y cross the cubes at no
// face.
float simplex3_at(std::uint64_t seed, double x, double y, double z) noexcept {
    const double xz = x + z;
    const double s2 = xz * rotate_3d_orthogonalizer;
    const double yy = y * root3_over_3;
    const double xr = x + s2 + yy;
    const double zr = z + s2 + yy;
    const double yr = (xz * -root3_over_3) + yy;
    return simplex3_base(seed, xr, yr, zr);
}

} // namespace

float simplex2(std::uint64_t seed, double x, double z) noexcept {
    return simplex2_at(seed, x, z);
}

float simplex3(std::uint64_t seed, double x, double y, double z) noexcept {
    return simplex3_at(seed, x, y, z);
}

Octaves::Octaves(std::uint64_t seed, double frequency, int count, double lacunarity,
                 float gain) noexcept
    : count_(std::clamp(count, 1, max_octaves)) {
    const std::uint64_t name = salt("noise.octave");
    double octave_frequency = frequency;
    float amplitude = 1;
    float total = 0;
    for (int i = 0; i < count_; ++i) {
        octave_.at(static_cast<std::size_t>(i)) = {
            .seed = stream_seed(seed, name, i),
            .frequency = octave_frequency,
            .amplitude = amplitude,
        };
        total += amplitude;
        octave_frequency *= lacunarity;
        amplitude *= gain;
    }
    for (int i = 0; i < count_; ++i) {
        octave_.at(static_cast<std::size_t>(i)).amplitude /= total;
    }
}

std::span<const Octaves::Octave> Octaves::first(int octaves) const noexcept {
    return std::span{octave_}.first(static_cast<std::size_t>(std::clamp(octaves, 0, count_)));
}

float Fbm2::sample(double x, double z, int octaves) const noexcept {
    float value = 0;
    for (const Octave& octave : first(octaves)) {
        value +=
            octave.amplitude * simplex2_at(octave.seed, x * octave.frequency, z * octave.frequency);
    }
    return value;
}

void Fbm2::fill(const Lattice2& lattice, std::span<float> out, int octaves) const noexcept {
    assert(out.size() >= lattice.size());
    std::size_t index = 0;
    for (int k = 0; k < lattice.nz; ++k) {
        // Each position from the lattice's first point, never by adding
        // steps up, so a point two lattices share is the same double in
        // both.
        const double z = lattice.z + (static_cast<double>(k) * lattice.step_z);
        for (int i = 0; i < lattice.nx; ++i) {
            const double x = lattice.x + (static_cast<double>(i) * lattice.step_x);
            out[index++] = sample(x, z, octaves);
        }
    }
}

float Fbm3::sample(double x, double y, double z, int octaves) const noexcept {
    float value = 0;
    for (const Octave& octave : first(octaves)) {
        value += octave.amplitude * simplex3_at(octave.seed, x * octave.frequency,
                                                y * octave.frequency, z * octave.frequency);
    }
    return value;
}

void Fbm3::fill(const Lattice3& lattice, std::span<float> out, int octaves) const noexcept {
    assert(out.size() >= lattice.size());
    std::size_t index = 0;
    for (int j = 0; j < lattice.ny; ++j) {
        const double y = lattice.y + (static_cast<double>(j) * lattice.step_y);
        for (int k = 0; k < lattice.nz; ++k) {
            const double z = lattice.z + (static_cast<double>(k) * lattice.step_z);
            for (int i = 0; i < lattice.nx; ++i) {
                const double x = lattice.x + (static_cast<double>(i) * lattice.step_x);
                out[index++] = sample(x, y, z, octaves);
            }
        }
    }
}

} // namespace vx
