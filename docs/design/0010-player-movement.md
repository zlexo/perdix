# RFC 0010: Player movement

| | |
|---|---|
| **Status** | Accepted (2026-10-08) |
| **Milestone** | M2.75 Walk |
| **Issue** | #196 |

## Summary

The player walks, sprints, sneaks, jumps and flies the way Minecraft Java Edition 26.2's player does: the same speeds, the same acceleration and drag, the same jump arc and sprint-jump boost, the same box against the same blocks, at the game's 20 ticks a second. Movement is a small simulation in `world`, with no GPU or window code: it advances the player's body one tick at a time from the keys held during that tick, clipping its box against the blocks' boxes one axis at a time. The camera follows the body between ticks, so movement is smooth at any frame rate. Every number is the game's, taken from published formulas and checked against the speeds players measure: walking 4.317 blocks a second, sprinting 5.612, sprint-jumping 7.127. RFC 0001's fly camera stays, as a noclip mode for engine work.

## Motivation

On 2026-10-08 the owner asked for Minecraft's movement: "player speed, sprinting, jump sprinting". Perdix moves today only with RFC 0001's fly camera: 10 blocks a second, 40 with Ctrl, no inertia, no gravity, no collision, stepped once per frame.

Close is not enough. Players' hands know the game's movement: when to jump, how far a sprint-jump carries, where a sneaking player stops at an edge. The numbers come out of the order the game does things in, not out of one speed setting:

- **Sprint-jumping is faster than sprinting because air drag is weaker than ground drag.** On the ground the game keeps 54.6% of horizontal speed each tick; in the air it keeps 91%. A sprint-jump adds 0.2 blocks a tick along the facing and then coasts, losing only 9% a tick, so the speed carried from one jump to the next stays above sprinting's top speed. Holding W, Ctrl and Space averages 7.127 blocks a second against sprinting's 5.612. A model with one friction value cannot produce both numbers.
- **The jump is an arc of exactly 12 ticks on flat ground**, peaking at 1.2522 blocks on the 6th: the jump's 0.42, gravity's 0.08 subtracted, then 2% drag, every tick. A 1-block step needs a jump; a 4-block gap is a sprint-jump. Both depend on that arc to a hundredth of a block.
- **Moving diagonally is 2% faster** (W and A together give 4.405 blocks a second against W's 4.317), and sneaking diagonally 41% faster, because the game shrinks the input only when it is longer than 1. Mojang removed this in a 1.21.5 snapshot and put it back two snapshots later, because the change hurt parkour; 26.2 has it.
- **Stopping is not instant.** Releasing W at walking speed slides 0.256 blocks over 7 ticks before a threshold zeroes what is left.

A simulation stepped per frame, with frame time as its step, would land differently at 30, 60 and 120 frames a second. The game's is stepped per tick, and so is this one: the same keys on the same blocks give the same path, which is also what makes these numbers testable.

## Goals and non-goals

Goals:

- Walking, sprinting (Ctrl or double-tapped W), sneaking (slower, 0.3 lower, held back at edges, crouched under low ceilings), jumping (with its cooldown) and sprint-jumping, as 26.2 does them: the test vectors below agree with the game's formulas to 1e-5 blocks per tick.
- Creative flight, toggled by double-tapping Space, with the game's flight speeds.
- The player's box collides with blocks' boxes the game's way: Y first, then the larger horizontal axis, then the other; landing, ceilings, step-up of 0.6, and wall hits that end a sprint only when steeper than 8 degrees.
- Slipperiness per block state, so an ice-like block slides as ice does.
- 20 ticks a second, keys read once per tick, the mouse turning the camera every frame, the camera's position interpolated between the last two ticks; the field of view widening when sprinting and flying, and the eye lowering smoothly when crouching.
- The same start, keys and blocks give the same positions, bit for bit, with one build on one machine.
- No allocation in a tick once the first has run.
- RFC 0001's fly camera kept as noclip flight.

Non-goals:

- **Liquids**: swimming, water's drag and currents. M7 (Water & Sky).
- **Blocks that do something to movement**: slime's bounce, honey, soul sand, cobwebs, powder snow, ladders, vines, scaffolding, beds, bubble columns. A block behaviors RFC; slipperiness is the one block property here, because it is part of every step on the ground.
- **Shapes other than cubes.** This design collides boxes, so slabs, stairs and fences only add boxes; the shapes come with the block models RFC. Until then every block in the core pack is a full cube, and step-up, which only matters with surfaces between whole blocks, is exercised by tests alone.
- **Gameplay**: health, fall damage, food, status effects (Speed, Jump Boost, Slow Falling, Blindness), using items, riding, elytra, knockback, entities pushing the player. Each arrives with its system; the movement numbers they change are data here (Design), not constants in code.
- **Crawling and swimming poses.** With full cubes, a gap under 1.5 blocks cannot be entered.
- **Multiplayer**: client-side prediction and the server's correction.
- **Options the game has around movement**: auto-jump (off by default since 1.19.4), toggle-to-sprint and toggle-to-sneak, the field of view slider. Small later additions.
- **The game's float arithmetic and sine table** (open question 4).

## Prior art

- **Minecraft Java Edition 26.2** is the model, and what this RFC copies is its behavior, never its code. The client runs the local player's physics at 20 ticks a second. Each tick it reads the keys, updates sprinting, applies the momentum threshold and the jump, adds acceleration (0.1 blocks per tick per tick on the ground, 30% more sprinting; 0.02 in the air, 0.026 sprinting), moves the box with collision, then subtracts gravity (0.08) and applies drag (vertical 0.98; horizontal 0.91, times the block's slipperiness 0.6 after a tick that started on the ground). The renderer interpolates the camera's position between ticks and turns it with the mouse every frame. Version history matters, because most write-ups describe 1.8: 1.9 lowered the momentum threshold from 0.005 to 0.003 (which raised the jump's peak from 1.2492 to 1.2522), 1.14 made the larger horizontal axis collide first, 1.18 let a sprinter graze a wall at under 8 degrees, 1.20 took slipperiness from the block the player stands on rather than the one under its center, and 1.21.5 made the horizontal threshold a test on speed rather than on each axis. Sources: minecraft.wiki (Sprinting, Sneaking, Walking, Jumping, Flying, Transportation, Player, Entity, Attribute, Options, and each version's changelog), Mojang's bug tracker (MC-271065 on diagonals, MC-241951, MC-184530 and MC-239705 on the threshold, MC-152728 on sprinting while sneaking, MC-278349).
- **mcpk.wiki**, the parkour community's wiki, gives the formulas behind every distance players jump, with closed forms for speed and height. Its pages describe 1.8, and some quote that version's code; we take the formulas and constants, check each against the version history above, and take no code.
- **Grim**, an anticheat that predicts each player's movement to catch cheats, keeps a re-implementation of the movement rules current to 26.2. It is the only public description of a few post-1.21 details, such as how step-up picks its height. It is GPL-3.0 code, so its use here is open question 3.
- **"Fix Your Timestep!"** (Glenn Fiedler, Gaffer on Games, 2004) is the standard account of a fixed simulation step with the renderer interpolating between the last two states, so that rendering at any rate never changes the simulation. Minecraft does this, and RFC 0005's world thread already ticks at a fixed 20 a second.
- **Quake III Arena**'s player movement (GPL-2.0) caps speed only along the direction the keys ask for, slides along the planes it hits, and steps up by retrying a blocked move 18 units higher. The cap is what lets players gain speed by turning in the air (strafe-jumping). Minecraft adds the same acceleration whatever the speed, and its sprint-jump gain comes from weaker drag in the air, so turning gains nothing. That is the movement first-person shooters feel like, not Minecraft's; we take nothing from it but the contrast.
- **Luanti** (formerly Minetest), a voxel engine, keeps its movement numbers as settings (walk speed, jump speed, gravity, accelerations on the ground and in the air) that games and mods change at run time. We take that idea: the numbers live in a struct that later effects and attributes change, not in the code.

## Design

### Where it lives

Movement is simulation, so it lives in `world` (ADR 0003): plain functions and value types, tested on CI without a window or GPU. RFC 0005 puts it on the world thread, which owns the chunks it collides with; `app` reads the keys, sends them, and draws the camera from the states that come back. Until the world thread exists (M3), `app` runs the same tick on the main thread against the demo chunk it already owns (open question 1).

### Units and axes

Distances are blocks, time is ticks of 50 ms, velocities are blocks per tick. The position is the bottom center of the player's box, in doubles, like RFC 0001's camera. Perdix's axes and turning differ from the game's: yaw 0 looks down −z and positive yaw turns left, seen from above. Movement only uses the directions the yaw gives, so every rule below is written with Perdix's:

- forward f = (−sin yaw, 0, −cos yaw)
- left l = (−cos yaw, 0, sin yaw)

Pitch never affects walking, the air or flight.

### The body

| Pose | Width | Height | Eye height |
|---|---|---|---|
| standing | 0.6 | 1.8 | 1.62 |
| crouching | 0.6 | 1.5 | 1.27 |

The box spans x and z by 0.3 either side of the position, and y from the position up by the height.

### The numbers

The game makes several of these attributes that effects and items change; here they are one struct with the game's values, so later systems change data rather than code.

| Name | Value | Meaning |
|---|---|---|
| `walk_acceleration` | 0.1 | ground acceleration on default ground (the game's movement speed) |
| `sprint_factor` | 1.3 | on ground and air acceleration while sprinting |
| `air_acceleration` | 0.02 | in the air, whatever the block below |
| `input_scale` | 0.98 | on each key axis |
| `sneak_factor` | 0.3 | on each key axis while crouching |
| `default_slipperiness` | 0.6 | air, and every block that sets none |
| `air_drag` | 0.91 | horizontal speed kept after a tick that started in the air |
| `gravity` | 0.08 | subtracted from vertical speed after each move |
| `vertical_drag` | 0.98 | vertical speed kept, after gravity |
| `jump_speed` | 0.42 | vertical speed a jump sets |
| `sprint_jump_boost` | 0.2 | added along the facing when jumping while sprinting |
| `jump_cooldown` | 10 ticks | between jumps while Space stays held |
| `step_height` | 0.6 | highest surface stepped onto without jumping |
| `momentum_threshold` | 0.003 | speeds below it become 0 |
| `sprint_input_threshold` | 0.8 | forward input needed to start sprinting |
| `double_tap_ticks` | 7 | window for double-tapping W (sprint) and Space (flight) |
| `minor_collision_degrees` | 8 | a wall hit at a smaller angle keeps a sprint |
| `fly_acceleration` | 0.05 | horizontal, doubled while sprinting |
| `fly_vertical_speed` | 0.15 | added to vertical speed by Space or Shift while flying |
| `fly_vertical_drag` | 0.6 | vertical speed kept after a flying move, in place of gravity and drag |

### Input

Each tick takes:

```cpp
struct MovementKeys {
    bool forward = false, back = false, left = false, right = false;
    bool jump = false, sneak = false, sprint = false;
};

struct TickInput {
    MovementKeys held;  // down when the tick runs
    float yaw = 0.0F;   // the camera's, in radians, when the tick runs
};
```

Keys are read once a tick, as the game reads them. A key counts as pressed on the first tick it is held, so a tap that starts and ends between two ticks is not seen at all. The mouse turns the camera every frame on the main thread; a tick uses the yaw the camera has when the tick runs, never one interpolated.

The input vector: forward = W − S and strafe = A − D (left positive), each times 0.98, and times 0.3 while crouching. If the vector is longer than 1 it is scaled to length 1; shorter, it stays as it is. So W gives 0.98, W and A give exactly 1 (the 2%), and sneaking diagonally gives 0.416 against sneaking straight's 0.294. The crouch multiplier follows the pose, which each tick sets for the next, so pressing Shift slows the player from the tick after.

### One tick

On the ground and in the air, in this order:

1. **Timers.** The double-tap windows count down by one.
2. **Sprinting** (below).
3. **Flight toggle** (below).
4. **Threshold and jump.** The cooldown counts down. If the horizontal speed carried from the last tick is under 0.003 (that is, vx² + vz² < 9 × 10⁻⁶), both horizontal components become 0; if |vy| < 0.003, vy becomes 0. Then, if the last move ended on the ground, Space is held and the cooldown is 0: vy becomes 0.42, a sprinting player gains 0.2 × f, and the cooldown becomes 10. Space up resets the cooldown to 0. With Space held on flat ground the player jumps again on the tick after it lands, every 12 ticks.
5. **Acceleration.** On the ground (the last move ended on the ground): a = 0.1 × (1.3 if sprinting) × (0.6 / s)³, with s the slipperiness of the block the player stands on, and the factor never above 1. In the air: a = 0.02, or 0.026 sprinting. The velocity gains a × (forward × f + strafe × l). On a jump tick the player is still on the ground, so the jump tick accelerates as on the ground.
6. **Move** the box by the velocity, with collision (below). Each component a collision stopped becomes 0.
7. **Gravity and drag.** vy = (vy − 0.08) × 0.98. The horizontal components are multiplied by s × 0.91 if the tick started on the ground, 0.546 on default ground, or by 0.91 if it started in the air.
8. **Pose, for the next tick.** The player crouches if Shift is held and it is not flying, or if the standing box does not fit where it now is and the crouching one does; otherwise it stands. The next tick's box, speed and eye height follow it.
9. **Keys.** This tick's held keys are kept, so the next tick can tell a fresh press.

Standing on the ground, the move cancels gravity's last step every tick, so vy settles at −0.0784 and the box presses on the block below; that is how the next move knows it is on the ground. Falling, vy approaches −3.92 blocks a tick (78.4 a second).

### Sprinting

- **Starts** when the sprint key is held, on the ground or in the air, or when W is pressed twice within the window on the ground: a fresh press opens a 7-tick window, and the window counts down before it is checked, so the second press must come within 6 ticks. Either way W must be held and the player not crouching: the game asks for a forward input of at least 0.8, which W gives (1, with or without A or D) and crouching does not (0.3). A sprint that starts speeds the player up the same tick, on the ground and in the air.
- **Stops** when the forward input goes (W released, or W and S together), or after a tick whose move hit a wall at more than 8 degrees: the angle between the direction the keys asked for and the direction the player actually moved. Grazing a wall keeps the sprint; running into one ends it on the next tick.
- **Survives crouching.** Pressing Shift while sprinting keeps the sprint: the player moves at the crouching speed times the sprint factor, 1.684 blocks a second. 26.2 does this: 1.21.4 made crouching end a sprint, and the snapshots for 1.21.5 reverted it.
- In the game, hunger, Blindness and using an item also stop or prevent sprinting. Perdix has none of them yet (open question 8).

### Collision

The box moves one axis at a time:

1. **Gather** the boxes of every block the box could touch: the box grown by the wanted motion on each axis, and by the step height when stepping can happen.
2. **Clip Y**, then the horizontal axis with the larger wanted motion (x on a tie), then the other. For each axis, the motion shrinks to the largest that keeps the box out of every block box overlapping it on the other two axes, with a tolerance of 10⁻⁷ blocks, so a box resting against a face is not inside it. The box moves by the clipped amount before the next axis.
3. **Flags.** `vertical_collision` is set when Y was clipped, `on_ground` when Y was clipped moving down, `horizontal_collision` when x or z was. The sprint test reads whether a horizontal hit was minor (under 8 degrees).

Because Y goes first, the tick the player walks off an edge still ends on the ground, so a jump the tick after works. Because axes go one at a time, a corner stops a diagonal move differently from each side.

**Step-up.** When the move hit something horizontally and the player started the tick on the ground or landed during the move, it may rise onto a surface. Every top or bottom face of the gathered boxes that lies between 0 and 0.6 above the box's bottom is a candidate height, lowest first, skipping the height the plain move reached. The move is tried again at each candidate height, with the same clipping, and the first one that goes further horizontally than the plain move wins; the player ends standing on that surface. A full block, 1 above, is never stepped onto: it takes a jump. With full cubes only, the box's bottom is always on a whole block when stepping could happen, so no candidate exists; slabs and stairs bring them.

**Edge guard.** While Shift is held, on the ground or within 0.6 above it, not flying and not moving up, the wanted x motion shrinks in steps of 0.05 (to 0 once under 0.05) until the box moved by it would still have a block box within 0.6 below its bottom; then z the same way; then both together. The guard does not cancel velocity, as a wall would, so speed builds against the edge, and releasing Shift there drops the player off at once.

**Slipperiness** comes from the block 0.5 below the player's feet, in the column of the block that supports the player: among the blocks whose boxes touch the bottom of the player's box, the one whose center is nearest the player's position (ties: the lower y, then x, then z). On default ground it is 0.6; ice is 0.98. A block file gives it (below).

### Creative flight

- **Toggles** when Space is pressed twice within 7 ticks; landing on a block ends it.
- **Horizontal**: the same input vector; acceleration 0.05, 0.1 sprinting; drag 0.91; no gravity. Flying with W reaches 10.89 blocks a second, sprint-flying 21.78.
- **Vertical**: Space adds 0.15 to vy before the move and Shift subtracts it; after the move vy is multiplied by 0.6, in place of gravity and the 0.98. Holding Space climbs at 7.5 blocks a second; releasing it stops the climb within a few ticks.
- While flying the player stands, Shift descends instead of crouching, and the edge guard is off.

### Blocks

RFC 0004's block files gain two keys:

```toml
# assets/core/blocks/packed_ice.toml (an example; not in the core pack)
collision = "full"    # or "none": nothing to collide with, as for a flower
slipperiness = 0.98   # default 0.6

[textures]
all = "packed_ice"
```

A block without `collision` collides as a full cube, so the core pack does as it looks; `core:air` collides with nothing. Errors: an unknown `collision` value, and a `slipperiness` that is not above 0 and at most 1 (ground acceleration divides by it). The registry keeps both per state, beside RFC 0004's other per-state data.

Movement reads the world through one function that lists block boxes:

```cpp
// The collision boxes of every block that overlaps `region`, appended to
// `out`, and the slipperiness of the block at a position.
class BlockBoxes {
public:
    virtual ~BlockBoxes() = default;
    virtual void collect(const Box& region, std::vector<Box>& out) const = 0;
    [[nodiscard]] virtual float slipperiness(BlockPos block) const = 0;
};
```

On the world thread it reads the chunk map and the registry; tests give worlds of boxes directly.

### The player

```cpp
namespace vx {

struct Vec3d { double x = 0.0, y = 0.0, z = 0.0; };
struct Box { Vec3d min, max; };

enum class Pose : std::uint8_t { standing, crouching };

struct Player {
    Vec3d position;          // bottom center of the box, in blocks
    Vec3d velocity;          // blocks per tick
    Pose pose = Pose::standing;
    bool on_ground = false;
    bool horizontal_collision = false;
    bool minor_horizontal_collision = false;
    bool sprinting = false;
    bool flying = false;
    bool may_fly = true;     // creative; false would turn the toggle off
    MovementKeys last_held;  // the last tick's, to tell fresh presses
    std::uint8_t jump_cooldown = 0;
    std::uint8_t sprint_window = 0;
    std::uint8_t fly_window = 0;
};

// Advances the player by one tick (50 ms) with the input held during it.
// `scratch` holds the gathered boxes between ticks, so a tick allocates
// nothing once the first has run.
void tick(Player& player, const TickInput& input, const BlockBoxes& world,
          const MovementNumbers& numbers, std::vector<Box>& scratch);

} // namespace vx
```

`Vec3d` goes in `core`, with the math ADR 0003 puts there; the world layer does not use glm today, and `app` converts at the camera.

### Ticks, threads and the camera

**Until M3**, the main thread keeps a 20-a-second clock, as RFC 0005's world thread will: each frame it runs every tick that is due, reading the keys for each, and skips ahead if more than a second behind. The demo chunk is the world; falling 64 blocks below its lowest block returns the player to its spawn on the grass with no velocity.

**With M3**, the tick runs in the world thread's `tick()`. The main thread sends the latest `TickInput` with RFC 0005's other messages (its "player's position" becomes the player's input), and the world thread sends back each tick's position, pose and flags, stamped with the tick's time. Chunks the move could touch that are not loaded yet hold the player in place, keeping its velocity, until they are (open question 7).

**The camera** is drawn at the interpolated position, lerp(the last tick's position, this tick's, α) plus the eye height, with α the time since the last tick over 50 ms, clamped to 0 to 1. Yaw and pitch are the live, per-frame values. The eye height moves halfway to its target each tick (1.62 standing, 1.27 crouching) and is interpolated too. The field of view is RFC 0001's 70 degrees times a factor that moves halfway to its target each tick and is interpolated per frame: 1 walking, 1.15 sprinting ((1.3 + 1) / 2), 1.1 flying, and 1.265 sprint-flying.

**Modes.** Walking is the default. Double-tapped Space toggles creative flight. A key of its own (F4 proposed) switches to RFC 0001's fly camera, which has no collision, no inertia and its own speeds, and back; it is what engine work and the M3 flythrough benchmark use (open question 5).

**The overlay** (F3) shows the position, speed in blocks a second, vertical speed, pose, whether the player is on the ground, sprinting or flying, and the tick count.

## Alternatives considered

- **A physics library** (Jolt, Bullet, PhysX). These simulate rigid bodies, usually a capsule, with continuous collision. Minecraft's feel comes from an axis-aligned box clipped one axis at a time with its own order of acceleration, collision and drag; reproducing that inside a general engine means fighting it, for features (rotation, impulses, joints) the player does not use.
- **Quake-style movement**: accelerate toward a wished velocity, slide along hit planes. A different feel, as Prior art says, and not what the owner asked for.
- **Stepping per frame, with the frame's time.** Breaks the tick-based numbers (a jump's height depends on the step) and makes movement depend on the frame rate.
- **A faster tick (60 a second) with scaled constants.** Discrete steps do not scale exactly: the jump's arc, the 12-tick airtime and the sprint-jump's cycle all change. The interpolated camera already moves smoothly at 20.
- **Bit-exact replication** of the game's float casts and 65,536-entry sine table: open question 4.
- **Movement on the main thread for good.** The main thread never reads a chunk (RFC 0005), and collision reads chunks; it runs there only until M3.

## Performance budget

On the reference machine, Release build:

| What | Budget | Measured by |
|---|---|---|
| One tick, walking on flat ground | under 2 µs | a bench of 100,000 ticks over a synthetic world, median of 15 runs |
| One tick, sprint-flying through a field of single blocks | under 20 µs | the same bench |
| Allocations per tick, after the first | 0 | the allocation counter (`allocation_profiling`), in a test |
| Camera per frame | interpolation only | |

A tick is a small fraction of RFC 0005's 50 ms tick, so the budget's point is that movement never shows up beside chunk work, and that it does not allocate.

## Test plan

**Vectors**, in `vx_world_tests`, against worlds of boxes, each checked to 1e-5 blocks per tick. The values come from the game's formulas, computed independently of the engine, and reproduce the speeds the wiki publishes to four figures (walk 4.3172, sprint 5.6123, sprint-jump 7.1268 blocks a second). Unless a vector says otherwise: flat default ground at y = 0, yaw facing +z, standing still on the ground.

- **From rest**: walking, sprinting and sneaking, 12 ticks each. Walking moves 0.098, 0.151508, 0.180723, 0.196675 blocks in its first 4 ticks and approaches 0.215859 blocks a tick; 95% of top speed after 5 ticks, 99% after 8, for all three.
- **Standing jump**: y = 0.42, 0.7532, 1.001336, 1.166109, 1.249187, 1.252203 (the peak, tick 6), 1.176759, 1.024424, 0.796736, 0.495201, 0.121297, then 0 on the ground at tick 12.
- **Standing sprint-jump**: the jump tick moves 0.3274 (0.1274 + 0.2), the next 0.20424 (0.3274 × 0.546 + 0.02548), landing at tick 12 at z = 2.875829, and the first ground tick after it moves 0.357082.
- **Continuous sprint-jumping**: a 12-tick cycle of 4.276059 blocks, 7.1268 a second, its first tick 0.612183.
- **Onto a 1-block step** at z = 3, sprinting from rest: jumping on any tick from 4 to 9 lands on top without touching the face (on tick 7: lands at tick 15, z = 4.111729); 1 to 3 lands short; 10 or 11 hits the face, loses its speed and the sprint, and still climbs on.
- **Onto a slab** (a box 0.5 tall at z ≥ 1): walking steps up at tick 5 without a horizontal collision or any speed lost.
- **Releasing W**: walking slides 0.255846 blocks over 7 ticks, sprinting 0.3326.
- **The threshold**: walking at yaw 3 degrees off an axis keeps its small sideways speed (0.005128, 0.007927, ... 0.011287 by tick 12), which the per-axis rule of earlier versions erased every tick.
- **Creative flight**: from rest with W, 0.049, 0.09359, 0.134167, 0.171092, 0.204694 blocks a tick, toward 0.544444; climbing 0.15, 0.24, 0.294, 0.3264, toward 0.375.
- **Rules**: double-tapping W 6 ticks apart starts a sprint and 7 does not; a sprint survives a wall grazed at 5 degrees and ends after one hit at 45; pressing Shift keeps the sprint; sneaking stops at a pillar's edge with W held and falls off without Shift; the player crouches by itself in a 1.6-tall gap; the cooldown makes a held jump under a 2-block ceiling wait 10 ticks where re-pressing jumps every 3; double-tapping Space toggles flight and landing ends it.
- **Properties**, over random worlds of boxes and random key sequences: after every tick the box overlaps no block box, every number is finite, and replaying the same keys gives identical states.
- **Controls**, as for every item: each rule mutated (the drag order, the axis order, the threshold's form, the 8-degree test, the cooldown) must turn a test red.

**In `app`**, with no window: the demo chunk's player, spawned above the grass, lands on it and stays there.

**By eye**, the owner plays Perdix and the game side by side: sprinting, a sprint-jump across a 4-block gap, a sprint-jump onto a 1-block step, sneaking to an edge, flight. That is also where the rules with no Mojang or wiki source (open question 3) get checked.

## Pull request plan

1. **Boxes and clipping** (`world`, with `Vec3d` in `core`): `Box`, the per-axis clip, the collision pass with its axis order and flags, `BlockBoxes` over a world of boxes. Landing, ceilings, corners from both sides, the edge-of-a-block tick.
2. **Step-up and the edge guard**, with the slab and step vectors and the sneaking edge cases.
3. **The tick on the ground and in the air**: the numbers, the input vector, acceleration with slipperiness, drag, gravity, the threshold, the jump, its cooldown and the sprint-jump boost. The from-rest, jump, sprint-jump, release and threshold vectors.
4. **Sprinting and crouching**: the sprint key, double-tapped W, the stops, the 8-degree test, the crouching pose, forced crouching and the crouch speed. The step vectors and the rule tests.
5. **Creative flight**: the toggle, horizontal and vertical movement, landing.
6. **Blocks**: `collision` and `slipperiness` in block files, `BlockBoxes` over chunks and the registry, the supporting block.
7. **Walking in the app**: the main thread's 20-a-second clock, keys per tick, the camera from the player (interpolation, eye height, field of view), the three modes, respawn, the overlay's lines, and the window-free demo test.
8. **View bobbing**, if open question 6 keeps it, judged by eye against the game; then As built.

## Open questions

All settled on 2026-10-08: every recommendation below was accepted.

1. **When is it built?** The roadmap puts walking in M5 Hands, after M3 (streaming) and M4 (terrain). Items 1 to 6 need nothing from either; item 7 needs a world, and the demo chunk is one: 32 by 32 blocks of ground with a beach a step lower, a tree, logs and a hut. Recommended: now, as **M2.75 Walk**, between M2.5 and M3, walking on the demo chunk with the main thread's clock; M3 moves the tick onto the world thread with the rest of the simulation, and M5 keeps breaking and placing. The alternative is M5, where streaming and terrain make more of a world to walk in.
2. **Which game?** Recommended: 26.2 exactly, quirks included: the 2% diagonal, the 41% diagonal sneak, sprinting while crouching, the speed-based threshold, the 8-degree graze. They are what players' hands expect; Mojang reverted its own attempt to remove the first two.
3. **The rules no Mojang or wiki source gives.** A few come only from Grim's re-implementation: how step-up picks its height since 1.21; the crouch applying from the next tick; the factor (0.6 / s)³ capped at 1; flight's vertical rule (with no source at all for its exact order); double-tapping Space within 7 ticks (stated by a mod's page). And mcpk.wiki contradicts itself once: its version history says the larger horizontal axis collides first, its collisions page says second; this RFC follows the version history. Recommended: take them as facts of the game's behavior, write the code from this RFC alone, never from Grim's code, quote none of it, and check each by eye in the game (the test plan's last step). The alternative is to leave them out until measured in the game, which would leave flight without a vertical rule.
4. **Float casts and the sine table?** The game rounds through floats in places (its 0.42 is 0.41999998688697815) and reads sine and cosine from a 65,536-entry table. Recommended: doubles and exact trigonometry. The differences are about 10⁻⁶ blocks a tick, and 2.4 × 10⁻⁴ of a sine at a few degrees off an axis: invisible in play, and the vectors at the axes agree to 1e-5. Bit-exactness would matter only for replaying recordings of the game's own runs.
5. **Flight modes.** Recommended: the game's creative flight on double-tapped Space, and RFC 0001's fly camera kept as noclip on a key of its own (F4), for engine work and the M3 benchmark. The alternative drops the old camera.
6. **View bobbing?** The camera's sway while walking is on by default in the game, and it is part of how walking feels, but no public source gives its formula. Recommended: build it last (item 8), on by default with an option to turn it off, tuned by eye against the game. The field of view change is sourced, and so is the crouch's smoothness (its rate is set by eye); both come in item 7 either way.
7. **Chunks that have not loaded (from M3).** Recommended: hold the player in place, keeping its velocity, until every chunk its move could touch has loaded. The alternatives: treat them as solid (the player stands on air at the edge of the loaded world) or as empty (it falls through the world).
8. **What Perdix does not have yet.** Hunger, Blindness and using items stop or prevent sprinting in the game; effects change speed and jumps; falling hurts. Recommended: sprinting is always allowed, the numbers stay at the game's defaults, and nothing hurts, until each system arrives with its rules.
9. **Keys.** Recommended: the game's defaults: W, A, S and D, Space to jump, Left Shift to crouch, Left Ctrl to sprint, double-tapped W to sprint and double-tapped Space to fly; in flight, Space and Shift climb and descend, as RFC 0001's do. Ctrl in the noclip camera still means faster.

## As built

Pull request plan item 1, boxes and clipping, landed as two pull requests: `Vec3d` with boxes and the per-axis clip; the collision pass and `move_box`.

- **Overlap counts only past 1e-7 blocks on the other two axes:** a box less than that into a floor slides over block seams, and a box resting against a face is not inside it; one a hair into a wall is stopped, not pushed back.
- **Each axis reports its own collision** (`x_collision`, `vertical_collision`, `z_collision` on `Move`), so the tick zeroes only the part of the velocity a block stopped.
- **`collect` gathers the boxes that overlap or touch a region**, so a box resting on a block still gathers it; a move that is not finite does nothing.
- **The coyote tick falls out of Y first:** the tick that walks off an edge still ends on the ground.

Item 2, step-up and the edge guard, landed as two pull requests.

- **Step-up counts the floor at 0 as a candidate unless the plain move reached it,** so a jump into a doorway two blocks high walks in rather than bumping the lintel.
- **The edge guard probes only the space under the feet,** down to the drop and a little more, so a block at chest height past an edge is not ground.
- **`MoveRules`** carries the step height, whether the tick began on the ground, and the guard's drop; rules that are not finite do nothing, and negative ones are none.

Item 3, the tick, landed as four pull requests: the body, the numbers and slipperiness; the tick; tests for the rules the vectors leave unchecked, from the review's mutants; jumping.

- **A yaw that is not finite makes the tick do nothing,** as a move that is not finite does: a NaN facing would turn the velocity NaN for good.
- **The 0.6 in the ground acceleration is the formula's own** (its 0.216 is 0.6 cubed), kept apart from the default slipperiness.
- **The jump set 0.42, as above;** with item 5 it came to keep a faster rise, as 26.2's does (from the jar): the vertical speed becomes 0.42 or stays what it was, whichever is more. Nothing pushes a player up yet.

Item 7, walking in the app, landed before items 4 to 6, as three pull requests: `ChunkBoxes` over one chunk; the walking clock; the client.

- **The clock is a fixed step with interpolation:** at most a second of catch-up, the elapsed time clamped before it is added, the camera between the last two ticks at eye height, the field of view eased and blended the same way.
- **Until item 4, the app started sprints itself** (Ctrl with W, without S).
- **F4 back to walking refuses to land the player inside a block,** where it could not move.
- **The overlay's line** shows walking or flying, the speed and the vertical speed in blocks a second, sprinting, crouching, in the air, and the ticks run.

From item 4 on, a source joined the wiki, mcpk.wiki and the research notes: Minecraft 26.2's jar, read for numbers and behavior only, never for code to port. Each number taken from it is marked below.

Item 4, sprinting and crouching, landed as four pull requests: sprinting; tests for the sprint's rules; crouching and the tick's bench; the app.

- **The 8-degree graze is measured against the horizontal motion after the collision** (the research notes, 5.2), and a box stopped dead counts as a real hit. So in the 1-block step vector a jump on tick 10, which meets the face moving straight at it and clears it on the next tick, keeps its sprint; only tick 11 loses it. The vector above has both losing it because the research simulator ended a sprint on any hit.
- **The double tap follows the jar,** not the account above: it works in the air too; a fresh press is the push forward rising from none, so letting go of S with W held counts; the window runs on after a double tap; Shift held the tick before or S held closes it. A sprint starts on any push forward with the player not crouched: 26.2 has no 0.8 threshold, and `MovementNumbers` drops `sprint_input_threshold`.
- **The pose rule runs every tick,** one gather more, so a block placed into a standing player's head will crouch it (M5) without a special case.
- **The tick's bench had no item and landed here:** about 96 ns a tick walking on a chunk's floor and 92 ns sprint-jumping, release build, reference machine, against the budget's 2 µs.

Item 5, creative flight, landed as three pull requests: the jump that keeps a faster rise; the tick; the app.

- **Flight follows 26.2's order, from the jar:** the second press of Space toggles wherever the player is, and taking off from the ground jumps as well; Space's and Shift's 0.15 go on before the threshold; the jump does not ask whether the player flies, since a flying player is on the ground only on the tick it takes off.
- **A player may fly by default,** as in creative, so Space pressed again within 7 ticks flies; the cooldown test under a ceiling uses a player that may not.
- **After a flying move the vertical speed is 0.6 of what it was before the move,** a collision or not, as the research notes give the rule; "multiplied by 0.6" above left that open. Zeroing it on a landing left the next tick off the ground, losing a Space press. A landing ends flight after the move, and that tick keeps flight's drags.
- **The flying bench runs in a synthetic slot** between a floor and a ceiling of single blocks, the box against both, sprinting at 98% of top speed and gathering about 16 boxes a tick: a single chunk let the flyer leave it, and a field of blocks in its way cost it its sprint. About 107 ns a tick, release build, against the budget's 20 µs.

Item 6, blocks, landed as four pull requests: the block-file keys and the registry's per-state physics; slipperiness underfoot and collision from the registry; tests for the supporting block's rules; the property tests.

- **`BlockBoxes::slipperiness` returns a double** (the sketch above says float), as every other movement number is one.
- **`ChunkBoxes` stays one chunk,** now over the registry's `BlockPhysics`; without them every block but air is a full cube on default ground. The map of chunks comes with M3.
- **The supporting block is 26.2's, from the jar,** chosen after each move that ends on the ground rather than at the start of a tick: of the blocks whose boxes overlap the slab from 1e-6 below the box's bottom up to it, the one whose center is nearest the player's position, measured in three dimensions, ties to the greater y, then z, then x. With none there, the same search under where the box came from (the move's horizontal motion taken back), unless the last search on the ground found none as well; off the ground, none. So a player that walks off an edge keeps the edge's block for the tick it still stands.
- **A tick begun on the ground slides on the block 0.500001 below the feet** (from the jar), in the supporting block's column, or under the player's position when no block has been chosen yet, as on a first tick: on a slab, the block under the slab; on anything taller than half a block, that block. 26.2's exception for walls and fence gates has no block to apply to yet.
- **The ground acceleration never exceeds the plain one:** 26.2 applies the formula only above a slipperiness of 0.6 (from the jar), as the formula's `min(1, ...)` already did.
- **The supporting block's search costs the walking tick about 40 ns:** about 141 ns a tick on a chunk's floor, from 99, release build, reference machine; sprint-jumping 102, from 93; flying, never on the ground, unchanged.
- **The property tests run in closed-in random worlds** (walls and a ceiling): in an open chunk every walk left it and tested only falling. They also run in rooms of boxes from an eighth of a block to a block and a half tall, so step-up and the edge guard work between whole blocks. A coverage probe: flying 20% of ticks, crouching 13%, a wall hit 17%, on the ground 44%, thousands of ticks on ice.

After item 6, one pull request fixed item 5 from the jar:

- **The tick that takes off from the ground moves on the ground's terms:** its acceleration and drag are the ground's, as for any tick begun on the ground, and flight's apply from the next tick. A flying player begins a tick on the ground only when it takes off. Item 5 had given that tick flight's terms: taking off with W held moved 0.049, then 0.09359, where it now moves 0.098, then 0.102508.

Item 8, view bobbing, landed with the game's own numbers, as three pull requests: the camera's roll; the bob; the mouse.

- **RFC 0001's camera gained a roll,** 0 by default, which leaves every earlier image unchanged.
- **View bobbing is 26.2's, from the jar** (no public source gives it): an amount that eases 0.4 of the way each tick toward the horizontal speed carried into the tick, at most 0.1, and toward 0 off the ground; a walk distance of 0.6 per block moved, read a fraction of a tick ahead; the scene shifted 0.5 times the amount sideways and the amount up and down, turned 3 degrees per unit around the view's z axis and 5 around its x axis, the tip's phase 0.2 behind. The drawn camera undoes the game's turn and shift exactly. The overlay has the game's View Bobbing switch, on by default.
- **The mouse turns 0.15 degrees per count, from the jar:** 26.2's turn at its default sensitivity (the setting, 0.5, times 0.6 plus 0.2, cubed, times 8, times 0.15 degrees). RFC 0001's camera turned 0.143.
- **The eye height and the field of view already matched the jar:** both ease halfway to their targets each tick, and the field of view's targets are 1.15 sprinting, 1.1 flying, and 1.265, both, sprint-flying.
