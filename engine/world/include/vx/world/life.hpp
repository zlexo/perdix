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
// toward the body's by at most 10 degrees a tick. Yaws are the engine's (RFC
// 0010), in radians, from -pi to pi; the pitch is positive looking up.
class LookControl {
public:
    explicit LookControl(float yaw = 0.0F) noexcept;

    // Looks at `point` from this tick to the next.
    void look_at(Vec3d point) noexcept;
    // Turns the head for one tick, its eyes at `eye`.
    void tick(Vec3d eye, float body_yaw) noexcept;

    [[nodiscard]] float yaw() const noexcept { return yaw_; }
    [[nodiscard]] float pitch() const noexcept { return pitch_; }

private:
    Vec3d point_;
    float yaw_;
    float pitch_ = 0.0F;
    int hold_ = 0; // ticks the point still holds
};

// Another player as Perdix sees one: where it stands, how high its eyes are
// over its feet (RFC 0010's eye_height() of its pose), and whether it
// sneaks, which hides it a little.
struct Onlooker {
    Vec3d feet{};
    double eye_height = 1.62;
    bool sneaking = false;
};

// The goals of Perdix's life (RFC 0012), for whatever shows what he does.
enum class LifeGoal : std::uint8_t { look_around };

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
// Looking around claims both: a chance of 2 in 100 at each choice, at a
// point one block away at his eye height in a random direction, for 22 to
// 40 ticks. With nothing running he stands, his head easing back to his
// body. The same seed, world and positions give the same life, and a tick
// allocates nothing beyond what the world's boxes take in `scratch`.
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

private:
    static constexpr std::size_t goal_count = 1;
    // What a goal claims: his legs, his eyes.
    enum class Claim : std::uint8_t { legs, eyes };
    static constexpr std::size_t claim_count = 2;

    [[nodiscard]] bool can_use(LifeGoal goal, const Player& body, const Onlooker& you,
                               const BlockBoxes& world, std::vector<Box>& scratch);
    [[nodiscard]] bool can_go_on(LifeGoal goal, const Player& body, const Onlooker& you) const;
    void start(LifeGoal goal);
    void stop(LifeGoal goal);
    void tick_goal(LifeGoal goal, const Player& body, const Onlooker& you);
    void choose(const Player& body, const Onlooker& you, const BlockBoxes& world,
                std::vector<Box>& scratch);
    // A random number from the life's seed.
    [[nodiscard]] std::uint64_t next() noexcept;

    std::uint64_t random_;
    std::uint64_t ticks_ = 0;
    std::array<bool, goal_count> running_{};
    std::array<std::uint64_t, goal_count> starts_{};
    // The goal that last took each claim; it frees the claim once stopped.
    std::array<std::optional<LifeGoal>, claim_count> holders_{};
    LookControl look_;
    float walking_yaw_;
    int around_time_ = 0; // ticks left looking around
    Vec3d around_;        // where looking around looks, from him
};

} // namespace vx
