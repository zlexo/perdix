# RFC 0012: The player's body

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M3.5 Body (proposed: open question 1) |
| **Issue** | #341 |

## Summary

The player gets a body: Minecraft Java Edition 26.2's player model, with its skin, posed the way the game poses it while standing, walking, sprinting, sneaking and turning, so that a camera that is not the player's eyes sees the player in the world. The model is twelve textured boxes on a 64 by 64 skin, posed on the CPU each frame and drawn camera-relative by a small pipeline of its own. With Minecraft installed the skin is one of the game's default skins, read from the player's own copy the way RFC 0009 reads block textures and never copied or kept; without it, Perdix's own. Every size, angle and rate is the game's, read from its 26.2 jar. There is no new UI: RFC 0010's noclip camera already leaves the player standing where it was, and now the player is visible there; programs built on the engine, such as a film tool, pose bodies and place cameras through the same renderer API.

## Motivation

On 2026-10-10 the owner asked to see the player from outside: "i want to have the camera look at the player from somewhere", explicitly "not in the f5 view sense", and "we should build the feature in perdix of viewing another player, no ui yet". It is for a video series in which "the 'player' is loose in this world and we add more and more features each time and viewers can suggest what to add": each episode films the player in Perdix's world from cameras placed around it, cutting to the player's own view only now and then.

Today the player has no body. In RFC 0010's noclip mode the fly camera leaves the player standing, a 0.6 by 1.8 box that collides with blocks but draws nothing. A camera anywhere but the eyes sees an empty world.

Close is not enough, for the same reason RFC 0010 gives for movement: players know this figure. The look comes from details the game fixes to the pixel and the tick:

- **The model is not the hitbox.** It is 1.875 blocks tall against a 1.8 box, 0.9375 blocks across the arms against 0.6, drawn at a scale of 0.9375 and lifted 1.501 blocks inside its frame, so its soles float a millimeter above the ground.
- **The walk swing follows distance moved, not keys held.** Each tick the swing's speed eases 40% of the way toward four times the distance moved, capped at 1: walking settles at 0.863, sprinting reaches the cap, sneaking stays at 0.259. Legs swing 1.4 times that in radians, arms 1.0 times it: about 69 degrees each way for the legs when walking and 80 when sprinting. Pushing into a wall swings nothing.
- **The body lags the head.** The head turns with the look every tick; the body eases 30% of the way toward the direction of movement each tick and is never more than 50 degrees from the look. Standing still, the body moves only once the look passes 50 degrees; strafing, it ends 50 degrees off the look; walking backward, it keeps facing the look.
- **The light is not the blocks'.** Entities are lit by two fixed lights, not by the blocks' face shades: a side facing east is 0.497 against a block's 0.6, a bottom 0.4 against 0.5. Drawn with the blocks' shades, the body would look pasted on.

## Goals and non-goals

Goals:

- The player's body drawn wherever the camera is not its eyes: in RFC 0010's noclip mode, and for any program that hands the renderer a pose.
- 26.2's player model: head, body, two arms and two legs, each with its outer layer (hat, jacket, sleeves, pants); wide and slim arms; the game's texel for every face of every box.
- 26.2's posing: the walk swing, the arms' idle sway, the leg splay, the head's yaw and pitch, the body's yaw with its lag and its 50-degree limit, the sneaking pose, all interpolated between ticks so the body moves smoothly at any frame rate.
- 26.2's entity light.
- The skin: a default skin read from the player's own Minecraft, never copied, kept or published (RFC 0009's rule); Perdix's own skin without the game, under `--no-minecraft`, and in every clip or screenshot a pull request carries.
- Up to 64 bodies a frame, so later players or a crowd of test bodies need no change; one is the case that matters now.
- No allocation in a frame, and posing cheap enough not to show beside the chunks (budget below).

Non-goals:

- **Minecraft's F5 views** (the camera behind or in front of the head). Not asked for; a later RFC if wanted.
- **The first-person arm and hand**: M5 Hands, with breaking and placing, which also brings the arm's attack swing.
- **Held items, armor, capes, elytra, name tags, shadows.** Each arrives with the system it belongs to.
- **Riding, swimming, gliding, sleeping and dying poses**: Perdix has none of those states.
- **Light levels on the body**: M6 Light. Until then the body is drawn at full light, as the blocks are.
- **The hurt tint and the hurt kick of the swing**: nothing hurts yet (RFC 0010's open question 8).
- **Custom skins.** Loading a skin file is small and can follow; downloading skins from Mojang's servers is out, as downloading the game's jar is in RFC 0009.
- **Other players and multiplayer**: after M9. The API takes many bodies so that they need no new draw path.
- **A player that moves by itself.** The series' player is driven by scripted keys in its film tool, outside the engine; any behavior of its own would be its own RFC.

## Prior art

- **Minecraft Java Edition 26.2** is the reference. Its classes are readable in 26.x, so every number below was read from its jar with `javap` (sources in the research notes kept with the vault: `PlayerModel`, `HumanoidModel`, `ModelPart`, `AvatarRenderer`, `LivingEntityRenderer`, `LivingEntity`, `WalkAnimationState`, `AnimationUtils`, `DefaultPlayerSkin`, `Lighting` and the entity shaders). As with RFC 0010, Perdix takes the numbers and the behavior, never the code.
- **ClassiCube** draws the same humanoid from the same 64 by 64 skins in a small C engine: twelve boxes and a box unwrap are all a player model needs, with no model files or skeletal animation.
- **Luanti (formerly Minetest)** draws its player from a mesh file with a 64 by 32 texture in the old Minecraft layout. A mesh needs a loader and an asset pipeline, and the skin layout is defined on boxes anyway; Perdix keeps boxes.

## Design

### Where it lives

Each layer keeps its job (ADR 0003):

- **`world`**: the body's animation as a tick advances it: the walk swing and the body's yaw, from the distance moved and the look; and the `Skin` and `Arms` types that `minecraft`, `mesh` and `render` share. Plain functions and value types, tested without a window or GPU, beside RFC 0010's movement.
- **`mesh`**: the model, its texel layout and the posing: a pose in, posed triangles out. Pure, like chunk meshing.
- **`minecraft`**: the default skins from the player's jar, through RFC 0009's archive reader.
- **`render`**: a body pipeline and its shaders, the skins in RFC 0001's texture table, and `FrameInputs::players`.
- **`app`**: the noclip camera sees the player; the world thread's frames carry the animation.

### Units and axes

The model is defined in the game's model space, in its pixels: 1 px is 1/16 block, +y points down, −z is the face and −x the player's right. Angles of parts are the game's, in radians, with its signs: a positive pitch of a hanging limb swings it backward, toward +z. A part's rotation is Rz · Ry · Rx, applied to a vertex x first, about the part's pivot.

The posed model is then placed in Perdix's world. Perdix's yaw differs from the game's (RFC 0010: yaw 0 looks down −z and positive turns left, where the game's yaw 0 looks down +z and positive turns right), so this RFC writes the placement in Perdix's terms. A model point m, in pixels, goes to the body's space

> b = 0.9375 · (−m.x / 16, 1.501 − m.y / 16, m.z / 16)

in blocks, facing −z, its soles at y = 0.0009; then

> world = feet + crouch + R(ψ) · b

where feet is the player's position (the bottom center of its box, RFC 0010), crouch is (0, −2/16, 0) while crouching and 0 otherwise, ψ is the body's yaw and R(ψ) turns −z toward −x as ψ grows, as RFC 0010's yaw does. The map from m to b is a rotation half a turn about z, not a mirror, so faces keep their winding. A game angle maps across as follows: the game's yaw θ is 180° − ψ; turns about the body's vertical axis keep their size and flip their sign; pitches of parts are used as the game gives them, in model space.

Key points of the standing model, at yaw 0, in blocks above the feet: soles 0.0009, hips 0.7041 (0.1113 either side), shoulders 1.2900 (0.2930 either side), neck 1.4072, the face's eye row 1.6416 (RFC 0010's eye height is 1.62), the head's top 1.8759, the hat's 1.9052; the pants layer dips 0.0137 below the soles.

### The model

Twelve boxes, in pixels: pivot, the box's lowest corner relative to the pivot, its size, its texel offset on the skin, and how far its outer layer is pushed out on every side.

| Part | Pivot | Corner | Size | Texels | Outer layer |
|---|---|---|---|---|---|
| head | 0, 0, 0 | −4, −8, −4 | 8, 8, 8 | 0, 0 | hat: 32, 0, pushed 0.5 |
| body | 0, 0, 0 | −4, 0, −2 | 8, 12, 4 | 16, 16 | jacket: 16, 32, pushed 0.25 |
| right arm | −5, 2, 0 | −3, −2, −2 | 4, 12, 4 | 40, 16 | sleeve: 40, 32, pushed 0.25 |
| left arm | 5, 2, 0 | −1, −2, −2 | 4, 12, 4 | 32, 48 | sleeve: 48, 48, pushed 0.25 |
| right leg | −1.9, 12, 0 | −2, 0, −2 | 4, 12, 4 | 0, 16 | pants: 0, 32, pushed 0.25 |
| left leg | 1.9, 12, 0 | −2, 0, −2 | 4, 12, 4 | 16, 48 | pants: 0, 48, pushed 0.25 |

An outer layer is a second box the same as its part's, moved with it, its corners pushed out by its amount; its texels use the unpushed size, so its texels stretch over the slightly larger faces. **Slim arms** are 3 px wide: the right arm's corner is at x = −2, the left's at −1, with the same pivots (y = 2, not the 2.5 some references give) and texel offsets. No box is mirrored in 26.2: the left limbs have texels of their own.

**The texel layout** of a box at (U, V) of size (dx, dy, dz), with u to the right and v down on the skin, the end exclusive, divided by 64 with no inset:

| Face | Texels | Read |
|---|---|---|
| top | U + dz, V to U + dz + dx, V + dz | from above, the front at the rectangle's lower edge |
| bottom | U + dz + dx, V to U + dz + 2dx, V + dz | as if from above, so mirrored from below |
| right side | U, V + dz to U + dz, V + dz + dy | from outside, back to front |
| front | U + dz, V + dz to U + dz + dx, V + dz + dy | from outside |
| left side | U + dz + dx, V + dz to U + 2dz + dx, V + dz + dy | from outside, front to back |
| back | U + 2dz + dx, V + dz to U + 2dz + 2dx, V + dz + dy | from outside |

Every side reads unmirrored from outside with the texture's top at the top, and the top face's front edge meets the front face's top edge, as they meet on the skin.

Each box is 6 faces of 2 triangles, 12 boxes are 432 vertices, unindexed, as RFC 0001's cube pulls its 36.

### The pose

```cpp
namespace vx {

// In world, beside RFC 0010's movement: shared by minecraft, mesh and render.
enum class Arms : std::uint8_t { wide, slim };

// A player skin: 64 by 64 texels, RGBA8, rows from the top, and the arms it
// is drawn on.
struct Skin {
    std::array<std::uint8_t, 64 * 64 * 4> rgba{};
    Arms arms = Arms::wide;
};

// In mesh: everything that places and poses one body for one frame, already
// interpolated between its last two ticks.
struct PlayerPose {
    Vec3d feet;             // the bottom center of the player's box
    float body_yaw = 0.0F;  // radians, Perdix's yaw
    float head_yaw = 0.0F;  // radians, Perdix's yaw: the look's
    float head_pitch = 0.0F; // radians, Perdix's sign: positive looks up
    double limb_position = 0.0; // the walk swing's phase
    double limb_amount = 0.0;   // 0 to 1
    double age = 0.0;       // ticks, for the idle sway
    bool crouching = false;
    Arms arms = Arms::wide; // the skin's
    std::uint32_t skin = 0; // which of the renderer's skins
};

// One posed vertex, as the body pipeline pulls it. Plain floats: mesh, like
// world, has no glm.
struct BodyVertex {
    std::array<float, 3> position; // blocks, relative to the body's block
    std::array<float, 2> texel;    // on the skin, 0 to 1
    std::array<float, 3> normal;   // outward, posed
};

// Poses one body into `out`, 432 vertices: positions relative to `origin`,
// the block the body stands in, texels on its skin, and outward normals.
void pose_player(const PlayerPose& pose, BlockPos origin, std::span<BodyVertex, 432> out) noexcept;

} // namespace vx
```

Posing follows the game's order: every part from rest, then the head, the walk swing, the leg splay, the crouch, the idle sway. With p the limb position, a the limb amount, all angles in radians in model space:

- **Head**: yaw by the head's turn from the body, wrapped to ±π, converted to the game's sense; pitch by the look's pitch, the game's sign (positive looks down), about the neck.
- **Walk swing**: right arm pitch cos(0.6662 p + π) · a, left arm cos(0.6662 p) · a; right leg cos(0.6662 p) · 1.4 · a, left leg cos(0.6662 p + π) · 1.4 · a. Each arm swings with the opposite leg.
- **Leg splay**, always: yaw and roll of +0.005 on the right leg and −0.005 on the left.
- **Crouching**: the body pitched 0.5 and moved 3.2 px down; both arms pitched a further 0.4 and moved 3.2 px down; both legs moved 4 px back; the head moved 4.2 px down; and the whole body 2/16 block lower in the world.
- **Idle sway**, last, with age = ticks + the fraction of the tick, s = +1 for the right arm and −1 for the left: roll += s · (0.05 cos(0.09 · age) + 0.05) and pitch += s · 0.05 sin(0.067 · age). Both arms drift outward, never in, over about 70 and 94 ticks.

Sprinting and jumping have no pose of their own in 26.2; sprinting only takes the swing to its cap.

### The animation

```cpp
namespace vx {

// The body's state a tick leaves, in world: what the camera needs to pose
// it between ticks.
struct BodyMotion {
    double limb_speed = 0.0;
    double limb_position = 0.0;
    float body_yaw = 0.0F; // radians, Perdix's yaw
    float head_yaw = 0.0F; // the look's, set each tick
};

// One tick's animation, after its move: `moved` is how far the body went
// over the tick, `look_yaw` the yaw the tick's input carried.
void animate(BodyMotion& motion, Vec3d moved, float look_yaw) noexcept;

} // namespace vx
```

Per tick, as the game does it:

- **The walk swing.** d is the horizontal length of the move, y ignored; the target is min(4 d, 1); the speed moves 0.4 of the way to the target; the position adds the speed. It follows the move the body made, not the keys: walking into a wall leaves the limbs still, flying swings them.
- **The head** takes the tick's look yaw.
- **The body.** If the body moved more than 0.05 blocks horizontally this tick, its target is the direction it moved, turned half a turn when that direction is between 95 and 265 degrees from the look (walking backward); otherwise its target is where it already faces. It turns 0.3 of the wrapped angle to its target each tick, then is held within 50 degrees of the look.

Between ticks, with t the fraction of the tick: the limb position is position − speed · (1 − t), the limb amount min(lerp(previous speed, speed, t), 1), the body's and the head's yaws are interpolated along the shorter way round, and the pitch is the live look's. RFC 0010's `WalkingFrame` carries the previous and current `BodyMotion` to the main thread with the position it already carries; `Walking` runs `animate` after each tick's move, as it updates the bob. A new body starts with its body and head facing its look and the swing at rest.

### The skin

```cpp
namespace vx::minecraft {

// The game's default player skins, as RGBA8, from the player's jar.
// `name` is one of alex, ari, efe, kai, makena, noor, steve, sunny, zuri;
// slim arms read slim/<name>.png and wide arms wide/<name>.png.
[[nodiscard]] Result<Skin> default_skin(const Archive& jar, std::string_view name, Arms arms);

} // namespace vx::minecraft
```

The 26.2 jar holds 18 default skins, `assets/minecraft/textures/entity/player/{wide,slim}/<name>.png`: 64 by 64 palette PNGs with a transparent entry, alpha 0 or 255 only, their hat layers empty. The reader is RFC 0009's: an entry missing, unreadable, not a PNG or not 64 by 64 is a warning, and the body falls back to Perdix's own skin. Nothing of the game's is written anywhere. The game gives a player with no skin one of the 18 by its account's UUID; Perdix has no account, so the skin is chosen (open question 2).

**Perdix's own skin** is a 64 by 64 PNG in the core pack, `assets/core/textures/entity/player.png`, wide arms, original art (open question 3). It is what `--no-minecraft` draws, what tests read, and what every published clip shows.

### Drawing

```cpp
namespace vx::render {

struct RendererDesc {
    // ... as before, and:
    // Player skins, uploaded at startup; a pose's `skin` indexes them.
    // Empty uses a magenta checker.
    std::span<const Skin> skins;
};

struct FrameInputs {
    // ... as before, and:
    // The bodies to draw: at most Renderer::max_players.
    std::span<const PlayerPose> players;
};

} // namespace vx::render
```

Each frame the renderer poses every body into its frame slot's vertex buffer, rewritten every frame as the cubes' origins are: 432 vertices a body, each its position relative to the body's block, its texel and its normal, and one origin a body, its block relative to the camera's block. Positions are posed in doubles and stored as floats relative to a block the body stands in, a few blocks at most, so they are exact to well under a texel; the camera's fraction is subtracted in the vertex shader, as the cube's is, and the GPU never sees a world position.

The body pipeline draws in the opaque pass with the chunks' reversed-Z depth:

- **Texels** from the skin in RFC 0001's texture table, with its nearest sampler.
- **Alpha**: texels below 0.1 are discarded. The game blends entities, but its default skins have no alpha but 0 and 255, so discarding draws the same pixels without sorting (open question 6).
- **No back-face culling**, as in the game: through a transparent texel of an outer layer, the inside of the box behind shows.
- **Light** per vertex, the game's: two lights fixed in the world, L0 = normalize(0.2, 1, −0.7) and L1 = normalize(−0.2, 1, 0.7), and brightness min(1, 0.6 · (max(0, L0 · n) + max(0, L1 · n)) + 0.4), with n the posed normal; a back face uses the normal reversed. A box facing up is 1.0, down 0.4, north or south 0.7395, east or west 0.497. As the light follows the posed normal, a swinging arm shades as it swings.

### In the app

In walking mode nothing changes: the camera is the eyes and no body is drawn. In noclip mode the main thread hands the renderer the player's pose each frame, from the world thread's last two frames, with the look the player had when noclip began: today the still input that noclip sends resets the yaw to 0, so it keeps the last yaw instead, and the main thread keeps the last pitch. The startup report's textures line gains the skin: `Skin: Minecraft 26.2 steve (wide), from <the jar>` or `Skin: Perdix's own`.

A film tool, or any program on the engine, does the same with its own cameras: it runs `Walking` with scripted keys, poses the body from its frames and places the camera where it likes. Nothing in the engine changes for it.

## Alternatives considered

- **Minecraft's F5 views.** The camera behind or in front of the head is what "third person" means in the game, and it is not what the owner asked for; it can follow on this RFC's body.
- **A model file** (glTF, or B3D as Luanti uses) with skeletal animation. A loader, an exporter and bones, to describe twelve boxes whose layout the skin format already fixes.
- **Posing on the GPU**, a static mesh and a matrix a part. For one body, posing 432 vertices on the CPU costs microseconds; GPU posing pays when there are hundreds of bodies, which would be its own RFC (mobs).
- **RFC 0001's cubes.** They are whole blocks on the block grid with one texture, unrotated; a body needs rotated boxes of any size with a texel per face, a new path either way.
- **The game's blending with sorted quads.** Exact for skins with partly transparent texels, which no default skin has; a sort for nothing until custom skins arrive.
- **Drawing the body in first person too**, so the camera sees the player's own feet. The game does not, and its first-person hand is M5's.

## Performance budget

On the reference machine, Release build:

| What | Budget | Measured by |
|---|---|---|
| Posing one body | under 5 µs | a bench posing 100,000 bodies, median of 15 runs |
| 64 bodies, GPU | under 0.1 ms | the frame's GPU time with and without them, Tracy |
| Allocations per frame | 0 | `vx_render_allocation_tests`, with bodies |
| Animation per tick | under 0.2 µs | the RFC 0010 tick bench, with and without `animate` |

## Test plan

**In `vx_mesh_tests`**, against tables written from this RFC, not from the code:

- **The layout**: every face of every box, wide and slim, reads the texel rectangle in the table above, in the stated direction; the outer layers' texels use the unpushed size.
- **The rest pose**: the key points above to 1e-4 blocks; the body's width across the arms 0.9375, wide, and 0.8203, slim.
- **Placement**: at yaw π/2 the face looks down −x; crouching moves every point of the legs 2/16 block lower than standing does, on top of the crouch pose; every face's normal points out of its box (winding).
- **The swing**: at limb position 0 and amount 1, the right leg is pitched back 1.4 radians about its hip and the left forward 1.4, the right arm forward 1.0 and the left back 1.0, each foot and hand where that pitch puts it; at limb position π / 0.6662 they trade places, and at amount 0 nothing swings.
- **The sway**: at age 0 both arms are rolled 0.1 outward; at age 69.8 / 2 about 0.

**In `vx_world_tests`**:

- **The swing from rest**, flat ground, 20 ticks each of walking, sprinting and sneaking: the speeds approach 0.863, 1.0 and 0.259; walking into a wall keeps the speed at 0; the target is capped at 1.
- **The body**: walking forward from a body 40 degrees off, the gap shrinks by 0.3 each tick; strafing settles at 50 degrees from the look; walking backward keeps the body on the look; standing while the look turns 70 degrees moves the body only by the 20 beyond 50.
- **Controls**, as for every item: mutating 0.6662, 1.4, 0.4, 4, 0.3, 50, a texel offset or an inflation turns a test red.

**In `vx_minecraft_tests`**, with a synthetic jar written by the existing fixture script: a palette skin with a transparent entry decodes to the expected RGBA; a missing, wrong-sized or broken skin falls back with its message.

**In `vx_render_tests`**, headless: one body in Perdix's own skin, seen from a fixed camera, front and side; checked pixels on the face, the arm and the background, and the silhouette's bounds against the projection of the key points; validation clean.

**By eye**, the owner sets Perdix and the game side by side: the game's F5 front and back views against Perdix's noclip camera placed to match, standing, walking, sprinting, sneaking, strafing, walking backward and turning on the spot past 50 degrees.

## Pull request plan

1. **The model** (`mesh`): the twelve boxes, wide and slim, the texel layout, posing a `PlayerPose` into triangles with normals; the layout, rest, placement, swing and sway tests.
2. **The animation** (`world`): `BodyMotion` and `animate`, the swing and the body's yaw; the tick vectors and the controls.
3. **The skins** (`minecraft`, `assets`): Perdix's own skin in the core pack; `default_skin` through RFC 0009's reader, with its fallback and messages; the synthetic-jar tests.
4. **Drawing** (`render`): `RendererDesc::skins`, `FrameInputs::players`, the body pipeline and its Slang shaders, posing into the frame's buffer; the headless render test and the allocation test with bodies.
5. **The noclip camera sees the player** (`app`): `WalkingFrame` carries `BodyMotion`, noclip keeps the look, the main thread poses the body, the startup report's skin line. Then As built.

## Open questions

1. **When is it built?** M3 has two items left (#256, #258), then its As built and sanitizer pass; the M9 RFC (#334) waits on five answers before any of its code, and M4 comes after it. Recommended: right after M3 closes, as **M3.5 Body**, as RFC 0010 was M2.75 Walk; it needs nothing from M4 or M9, and the series' third episode waits on it.
2. **Which default skin, with Minecraft installed?** The game picks one of 18 by the account's UUID; Perdix has none. Recommended: Steve, wide, the figure everyone knows; a command-line option `--skin <name>[:slim]` to pick any of the 18, with no UI.
3. **Perdix's own skin.** Recommended: original art made the way the core pack's eleven textures were, by code in the owner's texture tool on the Resurrect 64 palette, wide arms, so `--no-minecraft` and every published clip show a body that is Perdix's. The alternative, a plain colored figure, reads as a placeholder.
4. **Seeing the body move in the app.** Noclip leaves the player standing still, so in the app the body is seen but never walks; the film tool walks it by script. Recommended: nothing more in the app for now, as asked ("no ui yet"); a watch mode, a key that keeps the camera where it is and gives the keys and mouse back to the player, is small and can follow.
5. **The game's sine table?** The game reads sine and cosine from a 65,536-entry table, off by about 1e-4. Recommended: exact trigonometry, as RFC 0010 chose; the difference is a fraction of a texel.
6. **Discarding against blending.** Recommended: discard below 0.1 until custom skins arrive, since the default skins hold no partial alpha; blending with sorted quads comes with the first skin that needs it.

## As built

Filled in after implementation.
