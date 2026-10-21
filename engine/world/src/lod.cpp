#include <vx/world/lod.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

// The distance from the eye to the nearest point of a box's column,
// across, in blocks: the height left out.
double across_distance(const Vec3d& eye, const LodBox& box) noexcept {
    const auto gap = [](double v, std::int64_t min, std::int64_t max) {
        return std::max({static_cast<double>(min) - v, 0.0, v - static_cast<double>(max)});
    };
    const double x = gap(eye.x, box.min_x, box.max_x);
    const double z = gap(eye.z, box.min_z, box.max_z);
    return std::sqrt((x * x) + (z * z));
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
    return reaches(eye_chunk(eye), chunk, near_distance);
}

std::uint8_t LodRings::level_at(const Vec3d& eye, LodPos chunk) const noexcept {
    if (near(eye, chunk)) {
        return 0;
    }
    const double distance = across_distance(eye, box_of(chunk));
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

// A column of one level's chunks between the rows: lod_select's unit. A
// column changes level whole, every row at once (RFC 0011, as built): a
// coarse level's surface can lie a row below the finer one's, so rows
// swapped one at a time could leave the ground drawn by neither.
struct LodColumn {
    std::int32_t x = 0;
    std::int32_t z = 0;
    std::uint8_t level = 0;

    friend constexpr bool operator==(const LodColumn&, const LodColumn&) = default;
};

constexpr LodColumn column_of(LodPos chunk) noexcept {
    return {.x = chunk.x, .z = chunk.z, .level = chunk.level};
}

constexpr LodColumn parent_column(LodColumn column) noexcept {
    return {.x = column.x >> 1,
            .z = column.z >> 1,
            .level = static_cast<std::uint8_t>(column.level + 1)};
}

// The 4 columns one level finer inside `column`, which is above level 0, x
// fastest, as children() orders them.
constexpr std::array<LodColumn, 4> child_columns(LodColumn column) noexcept {
    std::array<LodColumn, 4> out{};
    const auto level = static_cast<std::uint8_t>(column.level - 1);
    for (std::int32_t i = 0; i < 4; ++i) {
        out[static_cast<std::size_t>(i)] = {
            .x = (column.x << 1) | (i & 1), .z = (column.z << 1) | ((i >> 1) & 1), .level = level};
    }
    return out;
}

struct LodColumnHash {
    std::size_t operator()(const LodColumn& column) const noexcept {
        return LodHash{}({.x = column.x, .y = 0, .z = column.z, .level = column.level});
    }
};
using LodColumnSet = std::unordered_set<LodColumn, LodColumnHash>;

// The top level's columns over the terrain disc's square, from which
// lod_select goes down.
std::vector<LodColumn> top_columns(const LodRings& rings, const Vec3d& eye) {
    std::vector<LodColumn> top;
    const EyeChunk at = eye_chunk(eye);
    const std::int64_t reach = std::max(rings.view_distance, 0);
    const auto shifted = [](std::int64_t chunk) {
        return static_cast<std::int32_t>(chunk >> max_lod_level);
    };
    for (std::int32_t z = shifted(at.z - reach); z <= shifted(at.z + reach); ++z) {
        for (std::int32_t x = shifted(at.x - reach); x <= shifted(at.x + reach); ++x) {
            top.push_back({.x = x, .z = z, .level = max_lod_level});
        }
    }
    return top;
}

// lod_select's walk, a column at a time: whether a column's part of the
// region can be drawn, appending the chunks that draw it; when not,
// appending nothing. Its recursion goes a level down per call, so at most 6
// deep.
class Selection {
public:
    Selection(const LodRings& rings, const Vec3d& eye, std::int32_t min_y, std::int32_t max_y,
              const std::function<LodReady(LodPos)>& ready, std::span<const LodPos> before,
              std::vector<LodPos>& out)
        : rings_(&rings), eye_(&eye), min_y_(min_y), max_y_(max_y), ready_(&ready), out_(&out),
          before_(before.begin(), before.end()) {
        // Every column drawn before, and every column with one drawn
        // somewhere below it, to find what was drawn inside a column
        // without walking all of it.
        for (const LodPos chunk : before) {
            LodColumn column = column_of(chunk);
            drawn_.insert(column);
            while (column.level < max_lod_level) {
                column = parent_column(column);
                if (!above_.insert(column).second) {
                    break;
                }
            }
        }
    }

    bool pick(LodColumn column) { // NOLINT(misc-no-recursion): a level down per call
        if (!in_region(column)) {
            return true; // nothing of the region to draw
        }
        const Scan scan = scan_rows(column);
        const bool finer = column.level > 0 &&
                           rings_->level_at(*eye_, at(column, first_row(column))) < column.level;
        if (finer && !scan.outside) {
            // Its children once all can be drawn.
            const std::size_t mark = out_->size();
            bool all = true;
            for (const LodColumn child : child_columns(column)) {
                all = pick(child);
                if (!all) {
                    break;
                }
            }
            if (all) {
                return true;
            }
            out_->resize(mark);
            // Until then what was drawn inside it before, while all of that
            // can still be: itself again, or finer columns, which its own
            // rows may not cover (a coarse surface can lie a row lower).
            // Itself only where nothing was.
            if (keep(column)) {
                return true;
            }
        }
        if (!scan.waiting) {
            take(column);
            return true;
        }
        return keep(column);
    }

private:
    // What a column's rows offer, from the last scan_rows of its level.
    struct Scan {
        bool waiting = false; // a row's mesh is not ready
        bool faces = false;   // a row has faces to draw
        bool outside = true;  // every row is outside its level's terrain
    };

    [[nodiscard]] static LodPos at(LodColumn column, std::int32_t row) noexcept {
        return {.x = column.x, .y = row, .z = column.z, .level = column.level};
    }
    // The column's rows that meet the rows lod_select draws.
    [[nodiscard]] std::int32_t first_row(LodColumn column) const noexcept {
        return min_y_ >> column.level;
    }
    [[nodiscard]] std::int32_t last_row(LodColumn column) const noexcept {
        return max_y_ >> column.level;
    }
    [[nodiscard]] bool in_region(LodColumn column) const noexcept {
        return vx::in_region(*rings_, *eye_, min_y_, max_y_, at(column, first_row(column)));
    }

    // Each row's state, kept for take(): a column's children are a level
    // down, so they keep theirs apart.
    Scan scan_rows(LodColumn column) {
        std::vector<LodReady>& states = states_.at(column.level);
        states.clear();
        Scan scan;
        for (std::int32_t row = first_row(column); row <= last_row(column); ++row) {
            const LodReady state = (*ready_)(at(column, row));
            states.push_back(state);
            scan.waiting = scan.waiting || state == LodReady::waiting;
            scan.faces = scan.faces || state == LodReady::mesh;
            scan.outside = scan.outside && state == LodReady::outside;
        }
        return scan;
    }

    // The column's rows with faces, as its level's last scan found them.
    void take(LodColumn column) {
        const std::vector<LodReady>& states = states_.at(column.level);
        for (std::size_t row = 0; row < states.size(); ++row) {
            if (states[row] == LodReady::mesh) {
                out_->push_back(at(column, first_row(column) + static_cast<std::int32_t>(row)));
            }
        }
    }

    // What was drawn inside `column` before, if all of it can still be:
    // none when nothing inside it was, as when a column around it drew it.
    bool keep(LodColumn column) { // NOLINT(misc-no-recursion): a level down per call
        const std::size_t mark = out_->size();
        if (drawn_.contains(column)) {
            // The column again, once every row drawn before can be: with
            // its rows that have faces now.
            for (std::int32_t row = first_row(column); row <= last_row(column); ++row) {
                const LodPos chunk = at(column, row);
                const LodReady state = (*ready_)(chunk);
                if (state == LodReady::waiting && before_.contains(chunk)) {
                    out_->resize(mark);
                    return false;
                }
                if (state == LodReady::mesh) {
                    out_->push_back(chunk);
                }
            }
            return true;
        }
        if (!above_.contains(column)) {
            return false;
        }
        // Something below it was drawn, so nothing around it was: each child
        // with nothing drawn inside had nothing to draw.
        bool all = true;
        for (const LodColumn child : child_columns(column)) {
            if (in_region(child) && (drawn_.contains(child) || above_.contains(child))) {
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
    LodSet before_;      // chunks drawn before
    LodColumnSet drawn_; // their columns
    LodColumnSet above_; // columns with one drawn before somewhere below
    std::array<std::vector<LodReady>, lod_levels> states_; // each level's last scan
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
    for (const LodColumn top : top_columns(rings, eye)) {
        selection.pick(top);
    }
    return drawn;
}

} // namespace vx
