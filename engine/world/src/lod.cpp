#include <vx/world/lod.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <unordered_set>
#include <vector>

namespace vx {
namespace {

// The camera's chunk, at level 0.
struct EyeChunk {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};

EyeChunk eye_chunk(const Vec3d& eye) noexcept {
    const auto chunk = [](double v) {
        return static_cast<std::int64_t>(std::floor(v)) >> chunk_bits;
    };
    return {.x = chunk(eye.x), .y = chunk(eye.y), .z = chunk(eye.z)};
}

// The level-0 chunks a chunk of any level spans on one axis, inclusive.
struct Span {
    std::int64_t first = 0;
    std::int64_t last = 0;
};

Span span(std::int32_t coordinate, std::uint8_t level) noexcept {
    const std::int64_t first = std::int64_t{coordinate} * (std::int64_t{1} << level);
    return {.first = first, .last = first + (std::int64_t{1} << level) - 1};
}

// Whether a chunk's columns come within `radius` chunks of the camera's
// column, as RFC 0006's region counts it.
bool reaches(const EyeChunk& eye, LodPos chunk, std::int32_t radius) noexcept {
    if (radius < 0) {
        return false;
    }
    const Span xs = span(chunk.x, chunk.level);
    const Span zs = span(chunk.z, chunk.level);
    const std::int64_t dx = std::clamp(eye.x, xs.first, xs.last) - eye.x;
    const std::int64_t dz = std::clamp(eye.z, zs.first, zs.last) - eye.z;
    return (dx * dx) + (dz * dz) <= std::int64_t{radius} * radius;
}

// The distance from the eye to the nearest point of a box, in blocks.
double nearest_distance(const Vec3d& eye, const LodBox& box) noexcept {
    const auto gap = [](double v, std::int64_t min, std::int64_t max) {
        return std::max({static_cast<double>(min) - v, 0.0, v - static_cast<double>(max)});
    };
    const double x = gap(eye.x, box.min_x, box.max_x);
    const double y = gap(eye.y, box.min_y, box.max_y);
    const double z = gap(eye.z, box.min_z, box.max_z);
    return std::sqrt((x * x) + (y * y) + (z * z));
}

// The floor under unit(), above which chunks drawn side by side differ by
// at most one level (RFC 0011's balance). Say a chunk A of level a is drawn
// beside a chunk B of level b <= a - 2. A is drawn, so its nearest point is
// at least radius(a) >= 2^(b+2) units away. B is drawn, so its parent P was
// refined: P's nearest point is under radius(b+1) = 2^(b+1) units away, or
// P reaches into the near cylinder, so its nearest point is at most the
// cylinder's reach, 32 sqrt(near_distance^2 + near_height^2) blocks, away.
// A touches B inside P, so A's nearest point is at most P's diagonal,
// sqrt(3) x 32 x 2^(b+1) blocks, farther than P's. The first case needs a
// unit under sqrt(3) x 32 blocks; the second, worst at b = 0, under (the
// reach + sqrt(3) x 64) / 4. A block above both, neither can happen.
// Whether any of a chunk's blocks are in the 32-bit world.
bool in_world(const LodBox& box) noexcept {
    constexpr std::int64_t edge = std::int64_t{1} << 31;
    return box.max_x > -edge && box.min_x < edge && box.max_y > -edge && box.min_y < edge &&
           box.max_z > -edge && box.min_z < edge;
}

double balance_floor(std::int32_t near_distance, std::int32_t near_height) noexcept {
    constexpr double root3 = std::numbers::sqrt3;
    const auto squares = static_cast<double>((std::int64_t{near_distance} * near_distance) +
                                             (std::int64_t{near_height} * near_height));
    const double reach = chunk_size * std::sqrt(squares);
    return std::max(root3 * chunk_size, (reach + (root3 * 2 * chunk_size)) / 4.0) + 1.0;
}

} // namespace

double LodRings::unit() const noexcept {
    const double from_screen =
        static_cast<double>(framebuffer_height) / (2.0 * pixels_per_cell * std::tan(fov_y / 2.0));
    return std::max(from_screen, balance_floor(near_distance, near_height));
}

double LodRings::radius(std::uint8_t level) const noexcept {
    return level == 0 ? 0.0 : unit() * static_cast<double>(std::int64_t{1} << level);
}

bool LodRings::near(const Vec3d& eye, LodPos chunk) const noexcept {
    const EyeChunk at = eye_chunk(eye);
    const Span rows = span(chunk.y, chunk.level);
    return rows.first <= at.y + near_height && rows.last >= at.y - near_height &&
           reaches(at, chunk, near_distance);
}

std::uint8_t LodRings::level_at(const Vec3d& eye, LodPos chunk) const noexcept {
    if (near(eye, chunk)) {
        return 0;
    }
    const double distance = nearest_distance(eye, box_of(chunk));
    const double base = unit();
    std::uint8_t level = 0;
    while (level < max_lod_level &&
           distance >= base * static_cast<double>(std::int64_t{1} << (level + 1))) {
        ++level;
    }
    return level;
}

bool LodRings::in_view(const Vec3d& eye, LodPos chunk) const noexcept {
    return reaches(eye_chunk(eye), chunk, view_distance);
}

bool LodRings::drawn(const Vec3d& eye, LodPos chunk) const noexcept {
    if (!in_view(eye, chunk) || level_at(eye, chunk) < chunk.level) {
        return false;
    }
    return chunk.level == max_lod_level || level_at(eye, parent(chunk)) <= chunk.level;
}

std::int32_t LodRings::stream_distance(std::uint8_t level) const noexcept {
    if (view_distance < 0) {
        return -1;
    }
    // Two things hold a drawn chunk near, horizontally, and it must meet
    // both. With s = 2^level, a chunk whose nearest point lies d blocks from
    // the eye is under d / (32 s) + sqrt(2) of the camera's chunk away: on
    // each axis the eye's chunk adds less than one.
    constexpr double root2 = std::numbers::sqrt2;
    const double span = std::ldexp(static_cast<double>(chunk_size), level);
    // In view: one of its level-0 columns is within the view distance of the
    // camera's, which halves with each level as the columns join, rounding
    // as above; exact at level 0.
    const double view = level == 0
                            ? static_cast<double>(view_distance)
                            : (std::ldexp(static_cast<double>(view_distance), -level) + root2);
    if (level == max_lod_level) {
        return static_cast<std::int32_t>(std::ceil(view));
    }
    // Its parent's place calls for a finer level: the parent's nearest point
    // is under radius(level + 1) away, or it reaches into the near cylinder,
    // whose columns lie within 32 x near_distance blocks. Each child of the
    // parent lies within a chunk's diagonal of any point of it, so the
    // chunk's nearest point is under sqrt(2) chunks farther.
    const double near = chunk_size * static_cast<double>(std::max(near_distance, 0));
    const double ring =
        (std::max(radius(static_cast<std::uint8_t>(level + 1)), near) / span) + (2.0 * root2);
    return static_cast<std::int32_t>(std::ceil(std::min(view, ring)));
}

namespace {

// The top level's chunks over the terrain disc's square and the rows, from
// which lod_cover and lod_select go down.
std::vector<LodPos> top_chunks(const LodRings& rings, const Vec3d& eye, std::int32_t min_y,
                               std::int32_t max_y) {
    std::vector<LodPos> top;
    const EyeChunk at = eye_chunk(eye);
    const std::int64_t reach = std::max(rings.view_distance, 0);
    const auto shifted = [](std::int64_t chunk) {
        return static_cast<std::int32_t>(chunk >> max_lod_level);
    };
    for (std::int32_t y = shifted(min_y); y <= shifted(max_y); ++y) {
        for (std::int32_t z = shifted(at.z - reach); z <= shifted(at.z + reach); ++z) {
            for (std::int32_t x = shifted(at.x - reach); x <= shifted(at.x + reach); ++x) {
                top.push_back({.x = x, .y = y, .z = z, .level = max_lod_level});
            }
        }
    }
    return top;
}

// Whether a chunk is part of the drawn region: in the rows, the 32-bit world
// and the terrain disc.
bool in_region(const LodRings& rings, const Vec3d& eye, std::int32_t min_y, std::int32_t max_y,
               LodPos chunk) noexcept {
    const Span rows = span(chunk.y, chunk.level);
    return rows.last >= min_y && rows.first <= max_y && in_world(box_of(chunk)) &&
           rings.in_view(eye, chunk);
}

// Chunks in a hash set.
struct LodHash {
    std::size_t operator()(const LodPos& pos) const noexcept {
        std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
        for (const std::uint64_t part :
             {std::uint64_t{static_cast<std::uint32_t>(pos.x)},
              std::uint64_t{static_cast<std::uint32_t>(pos.y)},
              std::uint64_t{static_cast<std::uint32_t>(pos.z)}, std::uint64_t{pos.level}}) {
            hash = (hash ^ part) * 0x100'0000'01B3ULL;
        }
        return static_cast<std::size_t>(hash);
    }
};
using LodSet = std::unordered_set<LodPos, LodHash>;

// lod_select's walk: whether a chunk's part of the region can be drawn,
// appending the chunks that draw it; when not, appending nothing. Its
// recursion goes a level down per call, so at most 6 deep.
class Selection {
public:
    Selection(const LodRings& rings, const Vec3d& eye, std::int32_t min_y, std::int32_t max_y,
              const std::function<LodReady(LodPos)>& ready, std::span<const LodPos> before,
              std::vector<LodPos>& out)
        : rings_(&rings), eye_(&eye), min_y_(min_y), max_y_(max_y), ready_(&ready), out_(&out),
          before_(before.begin(), before.end()) {
        // Every chunk with something drawn below it, to find what was drawn
        // inside a chunk without walking all of it.
        for (LodPos chunk : before) {
            while (chunk.level < max_lod_level) {
                chunk = parent(chunk);
                if (!above_.insert(chunk).second) {
                    break;
                }
            }
        }
    }

    bool pick(LodPos chunk) { // NOLINT(misc-no-recursion): a level down per call
        if (!in_region(chunk)) {
            return true; // nothing of the region to draw
        }
        const LodReady state = (*ready_)(chunk);
        const bool finer = chunk.level > 0 && rings_->level_at(*eye_, chunk) < chunk.level;
        if (finer && state != LodReady::outside) {
            // Its children once all can be drawn.
            const std::size_t mark = out_->size();
            bool all = true;
            for (const LodPos child : children(chunk)) {
                all = pick(child);
                if (!all) {
                    break;
                }
            }
            if (all) {
                return true;
            }
            out_->resize(mark);
            // Until then itself, unless it draws nothing where finer chunks
            // drawn before still can be.
            if (state == LodReady::empty && keep(chunk)) {
                return true;
            }
        }
        if (state != LodReady::waiting) {
            take(chunk, state);
            return true;
        }
        return keep(chunk);
    }

private:
    [[nodiscard]] bool in_region(LodPos chunk) const noexcept {
        return vx::in_region(*rings_, *eye_, min_y_, max_y_, chunk);
    }

    void take(LodPos chunk, LodReady state) {
        if (state == LodReady::mesh) {
            out_->push_back(chunk);
        }
    }

    // What was drawn inside `chunk` before, if all of it can still be: none
    // when nothing inside it was, as when a chunk around it drew it.
    bool keep(LodPos chunk) { // NOLINT(misc-no-recursion): a level down per call
        if (before_.contains(chunk)) {
            const LodReady state = (*ready_)(chunk);
            if (state == LodReady::waiting) {
                return false;
            }
            take(chunk, state);
            return true;
        }
        if (!above_.contains(chunk)) {
            return false;
        }
        // Something below it was drawn, so nothing around it was: each child
        // with nothing drawn inside had nothing to draw.
        const std::size_t mark = out_->size();
        bool all = true;
        for (const LodPos child : children(chunk)) {
            if (in_region(child) && (before_.contains(child) || above_.contains(child))) {
                all = keep(child);
                if (!all) {
                    out_->resize(mark);
                    break;
                }
            }
        }
        return all;
    }

    const LodRings* rings_;
    const Vec3d* eye_;
    std::int32_t min_y_;
    std::int32_t max_y_;
    const std::function<LodReady(LodPos)>* ready_;
    std::vector<LodPos>* out_;
    LodSet before_; // drawn before
    LodSet above_;  // with a chunk drawn before somewhere below
};

} // namespace

std::vector<LodPos> lod_cover(const LodRings& rings, const Vec3d& eye, std::int32_t min_y,
                              std::int32_t max_y) {
    std::vector<LodPos> drawn;
    if (min_y > max_y) {
        return drawn;
    }
    // The top level's chunks, then down through every chunk whose place
    // calls for a finer level.
    std::vector<LodPos> pending = top_chunks(rings, eye, min_y, max_y);
    while (!pending.empty()) {
        const LodPos chunk = pending.back();
        pending.pop_back();
        if (!in_region(rings, eye, min_y, max_y, chunk)) {
            continue;
        }
        if (chunk.level == 0 || rings.level_at(eye, chunk) >= chunk.level) {
            drawn.push_back(chunk);
            continue;
        }
        for (const LodPos child : children(chunk)) {
            pending.push_back(child);
        }
    }
    return drawn;
}

std::vector<LodPos> lod_select(const LodRings& rings, const Vec3d& eye, std::int32_t min_y,
                               std::int32_t max_y, const std::function<LodReady(LodPos)>& ready,
                               std::span<const LodPos> drawn_before) {
    std::vector<LodPos> drawn;
    if (min_y > max_y) {
        return drawn;
    }
    Selection selection(rings, eye, min_y, max_y, ready, drawn_before, drawn);
    for (const LodPos top : top_chunks(rings, eye, min_y, max_y)) {
        selection.pick(top);
    }
    return drawn;
}

} // namespace vx
