#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vx {

// RFC 0008's noise: OpenSimplex2, ported from KdotJPG's CC0 reference (its
// faster variant, OpenSimplex2.java), giving the same bits as the reference
// wherever the reference's 32-bit lattice indices reach. Here they are
// 64-bit, so no position the engine can ask for overflows them.
//
// Positions are blocks, in double, scaled and rotated into noise space in
// double; only the offset inside a noise cell becomes float. A value
// depends only on its seed and position, never on who asks or in what
// order, and the code is arithmetic, floor and nothing else: no sin, exp or
// pow, whose libraries disagree (RFC 0008's float rules).

// 2D, in the reference's standard orientation, the one it recommends for
// height and continent maps: RFC 0008's climate and shape. In -1 to 1.
[[nodiscard]] float simplex2(std::uint64_t seed, double x, double z) noexcept;

// 3D in the reference's ImproveXZ form, with y up: planes of constant y look
// alike in every direction, which the reference recommends for terrain. In
// -1 to 1.
[[nodiscard]] float simplex3(std::uint64_t seed, double x, double y, double z) noexcept;

// Evenly spaced points, in blocks: the first at (x, z), then step_x and
// step_z apart, nx by nz of them. Filled with x fastest, then z.
struct Lattice2 {
    double x = 0;
    double z = 0;
    double step_x = 1;
    double step_z = 1;
    int nx = 0;
    int nz = 0;

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return static_cast<std::size_t>(nx) * static_cast<std::size_t>(nz);
    }
};

// The same in 3D, filled with x fastest, then z, then y, as local_index
// orders a chunk's blocks. A chunk's density corners are 9 by 6 by 9 of
// them, 4 x 2^L by 8 x 2^L by 4 x 2^L blocks apart at level L.
struct Lattice3 {
    double x = 0;
    double y = 0;
    double z = 0;
    double step_x = 1;
    double step_y = 1;
    double step_z = 1;
    int nx = 0;
    int ny = 0;
    int nz = 0;

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) *
               static_cast<std::size_t>(nz);
    }
};

// fBm: octaves of noise summed, each `lacunarity` times the frequency of the
// last and `gain` times its amplitude, each with its own seed drawn from the
// field's seed (stream_seed with "noise.octave" and the octave's index).
// The sum is divided by the sum of every octave's amplitude, so it stays in
// -1 to 1 as the noise does. Values are summed in float from the longest
// octave to the shortest, in that order, at every position.
class Octaves {
public:
    static constexpr int max_octaves = 16;

    // `count` octaves, clamped to 1 to max_octaves; the first has `frequency`
    // cycles per block.
    Octaves(std::uint64_t seed, double frequency, int count, double lacunarity = 2.0,
            float gain = 0.5F) noexcept;

    [[nodiscard]] int count() const noexcept { return count_; }

    struct Octave {
        std::uint64_t seed = 0;
        double frequency = 0;
        float amplitude = 0; // already divided by the sum of every octave's
    };

    // The first `octaves` of them, the longest, clamped to 0 to count().
    // The division stays the full set's, so leaving the short octaves out
    // (RFC 0008's coarse levels, each level's count from the parameters in
    // #456) only takes their detail away and never scales what is left.
    [[nodiscard]] std::span<const Octave> first(int octaves) const noexcept;

private:
    std::array<Octave, max_octaves> octave_{};
    int count_ = 0;
};

class Fbm2 : public Octaves {
public:
    using Octaves::Octaves;

    [[nodiscard]] float sample(double x, double z, int octaves) const noexcept;
    [[nodiscard]] float sample(double x, double z) const noexcept { return sample(x, z, count()); }

    // Every point of `lattice` into `out`, which holds lattice.size()
    // values. Each equals sample() at its position bit for bit, so points
    // that two lattices share agree bit for bit.
    void fill(const Lattice2& lattice, std::span<float> out, int octaves) const noexcept;
    void fill(const Lattice2& lattice, std::span<float> out) const noexcept {
        fill(lattice, out, count());
    }
};

class Fbm3 : public Octaves {
public:
    using Octaves::Octaves;

    [[nodiscard]] float sample(double x, double y, double z, int octaves) const noexcept;
    [[nodiscard]] float sample(double x, double y, double z) const noexcept {
        return sample(x, y, z, count());
    }

    void fill(const Lattice3& lattice, std::span<float> out, int octaves) const noexcept;
    void fill(const Lattice3& lattice, std::span<float> out) const noexcept {
        fill(lattice, out, count());
    }
};

} // namespace vx
