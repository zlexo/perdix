#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vx {

// Where a head turns, as Minecraft 26.2's look control turns a mob's (RFC
// 0012; numbers from its jar). Given a point, the head's yaw turns toward it
// by at most 10 degrees a tick and its pitch goes straight to the point's,
// held within 40 degrees up or down; the point holds for the tick it is
// given and the next. With none, the pitch is level and the yaw turns back
// toward the body's by at most 10 degrees a tick. While walking at a target
// the head is then held within 75 degrees of the body. Yaws are the
// engine's (RFC 0010), in radians, from -pi to pi; the pitch is positive
// looking up.
class LookControl {
public:
    explicit LookControl(float yaw = 0.0F) noexcept;

    // Looks at `point` from this tick to the next.
    void look_at(Vec3d point) noexcept;
    // Turns the head for one tick, its eyes at `eye`, `walking` at a target
    // or not.
    void tick(Vec3d eye, float body_yaw, bool walking = false) noexcept;

    [[nodiscard]] float yaw() const noexcept { return yaw_; }
    [[nodiscard]] float pitch() const noexcept { return pitch_; }

private:
    Vec3d point_;
    float yaw_;
    float pitch_ = 0.0F;
    int hold_ = 0; // ticks the point still holds
};

// The life's random numbers (RFC 0012): SplitMix64 from a seed, small and
// fast; the same seed gives the same numbers.
class LifeRandom {
public:
    explicit LifeRandom(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept;
    // One of 0 up to `bound`, which is more than 0.
    [[nodiscard]] int below(int bound) noexcept;
    // From 0 up to 1.
    [[nodiscard]] float unit() noexcept;
    [[nodiscard]] double unit_double() noexcept;

private:
    std::uint64_t state_;
};

// A spot for Perdix to stroll to from `feet` (RFC 0012), picked as
// Minecraft 26.2 picks a cow's (from its jar: WaterAvoidingRandomStrollGoal,
// LandRandomPos, RandomPos): up to 10 tries at the block holding a point a
// whole number of blocks away, up to 10 across each way and 7 up or down,
// standing on a full block; all but 1 pick in 1,000 lift a block inside the
// ground to the top of it, at most 16 blocks. The first try whose block has
// room for his standing body and lies within 24 blocks of `leash` gives the
// spot, the middle of its block's floor. Room for his body stands in for
// the game's pathfinder, which Perdix does not have; the leash is our rule.
[[nodiscard]] std::optional<Vec3d> stroll_spot(Vec3d feet, Vec3d leash, LifeRandom& random,
                                               const BlockBoxes& world, std::vector<Box>& scratch);

// Another player as Perdix sees one: where it stands, how high its eyes are
// over its feet (RFC 0010's eye_height() of its pose), and whether it
// sneaks, which hides it a little.
struct Onlooker {
    Vec3d feet{};
    double eye_height = 1.62;
    bool sneaking = false;
};

// The goals of Perdix's life (RFC 0012), for whatever shows what he does.
enum class LifeGoal : std::uint8_t { stroll, look_at_player, look_around };

// One tick of Perdix's life: what RFC 0010's tick moves his body with, the
// keys and the walking yaw, and where his head looks, for RFC 0012's
// animate(): yaws the engine's, in radians, the pitch positive looking up.
struct LifeTick {
    TickInput input{};
    float look_yaw = 0.0F;
    float look_pitch = 0.0F;
};

// Perdix's life (RFC 0012): Minecraft 26.2's idle mob goals, from its jar,
// driving a player. The goals claim his legs or his eyes or both, and are
// chosen again every second tick in their order: a goal that is not running
// starts when its chance comes and what it claims is free or held by a goal
// after it, which stops; a running one stops once it can no longer go on.
// Strolling claims the legs: a chance of 1 in 60 at each choice, about every
// 6 seconds, to pick a spot within 10 blocks across and 7 up or down that
// stands on a full block with room for him, and no farther than 24 blocks
// from you (the leash), then to walk straight at it, the walking yaw turning
// by at most 90 degrees a tick, until he is within a block of it or has
// covered less than a block in 100 ticks: stuck, as against a wall too high
// to climb. Looking at you claims the eyes: a chance of 2 in 100 at each
// choice while
// you are within 6 blocks (4.8 while you sneak) and in his line of sight
// from eye to eye, then your eyes for 40 to 80 ticks, while you stay within
// 6, wall or no wall. Looking around claims both: 2 in 100 at each choice,
// so only when nothing holds either, at a point one block away at his eye
// height in a random direction, for 22 to 40 ticks. With nothing running he
// stands, his head easing back to his body. The same seed, world and
// positions give the same life, and a tick allocates nothing beyond what
// the world's boxes take in `scratch`.
//
// Each world tick, for him:
//
//     const LifeTick t = life.tick(perdix.player(), motion.body_yaw, you, world, scratch);
//     perdix.tick(t.input, world);  // RFC 0010's move
//     animate(motion, moved, t.look_yaw, t.look_pitch);
class Life {
public:
    // A life from `seed`, its head and walking yaw at `facing`.
    Life(std::uint64_t seed, float facing) noexcept;

    // One tick's choices for `body`, whose body faces `body_yaw` (RFC
    // 0012's BodyMotion), with `you` about, among the world's blocks.
    [[nodiscard]] LifeTick tick(const Player& body, float body_yaw, const Onlooker& you,
                                const BlockBoxes& world, std::vector<Box>& scratch);

    [[nodiscard]] bool running(LifeGoal goal) const noexcept;
    // How many times `goal` has started.
    [[nodiscard]] std::uint64_t starts(LifeGoal goal) const noexcept;
    // Where he walks, while he walks at a target.
    [[nodiscard]] std::optional<Vec3d> walk_target() const noexcept { return target_; }

private:
    static constexpr std::size_t goal_count = 3;
    // What a goal claims: his legs, his eyes.
    enum class Claim : std::uint8_t { legs, eyes };
    static constexpr std::size_t claim_count = 2;

    [[nodiscard]] bool can_use(LifeGoal goal, const Player& body, const Onlooker& you,
                               const BlockBoxes& world, std::vector<Box>& scratch);
    [[nodiscard]] bool can_go_on(LifeGoal goal, const Player& body, const Onlooker& you) const;
    void start(LifeGoal goal, const Player& body);
    void stop(LifeGoal goal);
    void tick_goal(LifeGoal goal, const Player& body, const Onlooker& you);
    void choose(const Player& body, const Onlooker& you, const BlockBoxes& world,
                std::vector<Box>& scratch);
    // The tick's walk at the target, if there is one: the keys, and the
    // walking yaw turned toward it.
    [[nodiscard]] MovementKeys walk(const Player& body);

    LifeRandom random_;
    std::uint64_t ticks_ = 0;
    std::array<bool, goal_count> running_{};
    std::array<std::uint64_t, goal_count> starts_{};
    // The goal that last took each claim; it frees the claim once stopped.
    std::array<std::optional<LifeGoal>, claim_count> holders_{};
    LookControl look_;
    float walking_yaw_;
    int look_time_ = 0;   // choices left looking at you
    int around_time_ = 0; // ticks left looking around
    Vec3d around_;        // where looking around looks, from him
    Vec3d spot_;          // the stroll's spot, between its choice and its start
    // The walk at a target, and the place and tick it was last checked for
    // being stuck.
    std::optional<Vec3d> target_;
    Vec3d stuck_from_;
    std::uint64_t stuck_checked_ = 0;
};

} // namespace vx
