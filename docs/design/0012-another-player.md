# RFC 0012: Another player

| | |
|---|---|
| **Status** | Accepted (2026-10-10) |
| **Milestone** | M3.5 Body (settled: open question 1) |
| **Issue** | #341 |

## Summary

The world gets a second player, named Perdix, who lives in it on his own: he strolls to a spot nearby every few seconds, looks around, looks at you when you come close, and climbs one-block steps the way the game's auto-jump climbs them, with a name tag over his head. Both players get bodies: Minecraft Java Edition 26.2's player model, skins, posing and light, so a camera anywhere in the world sees them. You stay yourself, in first person, as before; step out of your body with the free camera (F4) and you see yourself too, in your own skin. Every size, angle, rate and color the player sees is the game's, read from its 26.2 jar. The bodies are twelve textured boxes posed on the CPU each frame; the name tag is text drawn in the world with the game's font, read from the player's own jar like the block textures (RFC 0009), or Perdix's own. The app gains its first panel, the god panel on G, to set Perdix's mode, bring him to you and choose both skins; and nothing is bound to the F row any more: the overlay moves from F3 to O and the free camera from F4 to V.

In this RFC "Perdix" is the character; the engine is "the engine".

## Motivation

On 2026-10-10 the owner asked to see a player from outside, "not in the f5 view sense, i want to have the camera look at the player from somewhere", and to "build the feature in perdix of viewing another player, no ui yet". It is for a video series in which "the 'player' is loose in this world and we add more and more features each time and viewers can suggest what to add". The player has a name: "i was thinking of naming the other player Perdix ... we zoom into his face and we give him a name and we add the name tag and then we give him a skin and he will be our main guy in this series". He is not the owner: "i still want to see the body walk inside the app, just not my body ... for now i want to be myself and be able to see this player", and he has a life of his own: "by default it should live its own life, we are developing its universe so we are god". The app may grow UI for it, but not on the F row: "feel free to start adding ui to the app. i don't like using the f keys because i have a 60% keyboard so bind stuff not to f keys".

Today there is one player, and it has no body. RFC 0010's noclip camera leaves it standing, a 0.6 by 1.8 box that collides with blocks but draws nothing, and the engine draws no text in the world at all.

Close is not enough, for the same reason RFC 0010 gives for movement: players know this figure and these habits. They come from details the game fixes to the pixel and the tick:

- **The model is not the hitbox.** It is 1.875 blocks tall against a 1.8 box, 0.9375 blocks across the arms against 0.6, drawn at a scale of 0.9375 and lifted 1.501 blocks inside its frame, so its soles float a millimeter above the ground.
- **The walk swing follows distance moved, not keys held.** Each tick the swing's speed eases 40% of the way toward four times the distance moved, capped at 1: walking settles at 0.863, sprinting reaches the cap, sneaking stays at 0.259. Legs swing 1.4 times that in radians, arms 1.0 times it: about 69 degrees each way for the legs walking, 80 sprinting. Pushing into a wall swings nothing.
- **The body lags the head.** The body eases 30% of the way toward the direction of movement each tick and is never more than 50 degrees from the look.
- **The light is not the blocks'.** Entities are lit by two fixed lights: a side facing east is 0.497 against a block's 0.6, a bottom 0.4 against 0.5.
- **The name tag shows through walls.** It floats 2.3 blocks above the feet, drawn twice: once at half white over a 25% black box with no depth test, so it is seen through blocks, then fully white where nothing is in front.
- **An idle creature's rhythm.** Mobs stroll to a spot within 10 blocks about every 6 seconds, glance around now and then for one to two seconds, and look at a player within 6 blocks for two to four seconds, the head turning at most 10 degrees a tick. That rhythm is what makes a creature in the game look alive rather than scripted.

## Goals and non-goals

Goals:

- **Two players.** You, in first person as today; Perdix, a second player in the world, moving by RFC 0010's movement, driven by his own life instead of keys.
- **Bodies**, drawn wherever the camera is not that player's eyes: Perdix always; you in the free camera; any body a program hands the renderer.
- **26.2's player model**: head, body, two arms and two legs, each with its outer layer; wide and slim arms; the game's texel for every face.
- **26.2's posing**: the walk swing, the idle sway, the leg splay, head yaw and pitch, the body's yaw with its lag and limit, the sneaking pose, interpolated between ticks.
- **26.2's entity light.**
- **Skins**: Perdix's own, the character's look, made for the engine; the owner's own, `Zlex`, which you wear; the game's 18 default skins, read from the player's jar, never copied, kept or published; and any skin file in the game's format.
- **Perdix's life**, the game's idle rhythm: strolling, looking around, looking at you, climbing steps by the auto-jump rule; and two other modes, following you and standing still.
- **Name tags** as the game draws a player's, with the game's font from the jar or Perdix's own font: over Perdix, and over you when the free camera sees you.
- **The god panel** (G): Perdix's mode, bringing him to you, both players' skins. **No key on the F row**: the overlay on O and the free camera on V, instead of F3 and F4.
- Up to 64 bodies and tags a frame; no allocation in a frame; posing, life and tags cheap enough not to show beside the chunks.

Non-goals:

- **Minecraft's F5 views**: not asked for.
- **The first-person arm and hand, and the arm's swing**: M5 Hands.
- **Held items, armor, capes, elytra, shadows.** The owner's profile has a cape; capes arrive with their own RFC.
- **Riding, swimming, gliding, sleeping and dying poses**; the hurt tint.
- **Light levels on bodies and tags**: M6 Light. Until then they are drawn at full light, as the blocks are.
- **Pathfinding.** Perdix walks straight at his stroll target and climbs what auto-jump climbs; a two-block wall ends the stroll early. Finding a way around is its own RFC, and a good episode.
- **Interacting with Perdix**: talking, pushing, giving, hurting, commanding. The owner wants to meet him "as god or as a normal player"; each interaction is its own feature.
- **Saving Perdix**: M8 Save. He appears afresh each run.
- **Downloading skins** from Mojang's servers: never at run time. The owner's skin was fetched once, with his permission, and is a file in the repository.
- **More characters and multiplayer**: the design draws many bodies and runs Perdix as one more player; more come later.

## Prior art

- **Minecraft Java Edition 26.2** is the reference. Its classes are readable in 26.x, so every number below was read from its jar with `javap`, behavior and numbers only, never code: the model (`PlayerModel`, `HumanoidModel`, `ModelPart`), its placement and animation (`AvatarRenderer`, `LivingEntityRenderer`, `LivingEntity`, `WalkAnimationState`, `AnimationUtils`), skins (`DefaultPlayerSkin`), light (`Lighting`, the entity shaders), name tags (`EntityRenderer`, `SubmitNodeCollection`, `Font`, `BitmapProvider`, the text pipelines), the idle life (`RandomStrollGoal`, `RandomLookAroundGoal`, `LookAtPlayerGoal`, `LookControl`, `BodyRotationControl`, `GoalSelector`) and auto-jump (`LocalPlayer`). The research notes with every source are kept with the owner's notes.
- **ClassiCube** draws the same humanoid from 64 by 64 skins in a small C engine: twelve boxes and a box unwrap are all a player model needs.
- **Luanti (formerly Minetest)** draws its player from a mesh file with a 64 by 32 texture in the old Minecraft layout; a mesh needs a loader, and the skin layout is defined on boxes anyway.
- **NPC plugins for Minecraft servers** (Citizens and others) make characters out of player-shaped bodies for the same reason this RFC does: players read a player instantly. Their behavior is their own; Perdix's life takes the game's idle mobs' numbers instead.

## Design

### Where it lives

- **`world`**: the second player's life (pure: a seeded random, the world's boxes, the players' positions in, a `TickInput` out); the body's animation per tick; the `Skin` and `Arms` types. Tested without a window or GPU, beside RFC 0010's movement.
- **`mesh`**: the model, its texel layout and posing; the name tag's glyph quads from a font's metrics.
- **`minecraft`**: the default skins and the default font from the player's jar, through RFC 0009's reader.
- **`render`**: the body pipeline and the text pipeline, skins and font atlases in RFC 0001's texture table, `FrameInputs::players` and `FrameInputs::tags`.
- **`app`**: both players on the world thread, Perdix's mode, which bodies and tags each frame draws, the startup report.

### Units and axes

The model is defined in the game's model space, in its pixels: 1 px is 1/16 block, +y points down, −z is the face and −x the player's right. Angles of parts are the game's, in radians, with its signs: a positive pitch of a hanging limb swings it backward, toward +z. A part's rotation is Rz · Ry · Rx, applied to a vertex x first, about the part's pivot.

The posed model is then placed in the world. The engine's yaw differs from the game's (RFC 0010: yaw 0 looks down −z and positive turns left, where the game's yaw 0 looks down +z and positive turns right), so placement is written in the engine's terms. A model point m, in pixels, goes to the body's space

> b = 0.9375 · (−m.x / 16, 1.501 − m.y / 16, m.z / 16)

in blocks, facing −z, its soles at y = 0.0009; then

> world = feet + crouch + R(ψ) · b

where feet is the player's position (the bottom center of its box, RFC 0010), crouch is (0, −2/16, 0) while crouching and 0 otherwise, ψ is the body's yaw and R(ψ) turns −z toward −x as ψ grows, as RFC 0010's yaw does. The map from m to b is a rotation half a turn about z, not a mirror, so faces keep their winding. A game angle maps across as follows: the game's yaw θ is 180° − ψ; turns about the vertical axis keep their size and flip their sign; pitches of parts are used as the game gives them, in model space.

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

Every side reads unmirrored from outside with the texture's top at the top, and the top face's front edge meets the front face's top edge, as they meet on the skin. Each box is 6 faces of 2 triangles; 12 boxes are 432 vertices, unindexed, as RFC 0001's cube pulls its 36.

### The pose

```cpp
namespace vx {

// In world: shared by minecraft, mesh and render.
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
    Vec3d feet;                  // the bottom center of the player's box
    float body_yaw = 0.0F;       // radians, the engine's yaw
    float head_yaw = 0.0F;       // radians, the engine's yaw
    float head_pitch = 0.0F;     // radians, the engine's sign: positive looks up
    double limb_position = 0.0;  // the walk swing's phase
    double limb_amount = 0.0;    // 0 to 1
    double age = 0.0;            // ticks, for the idle sway
    bool crouching = false;
    Arms arms = Arms::wide;      // the skin's
    std::uint32_t skin = 0;      // which of the renderer's skins
};

// One posed vertex, as the body pipeline pulls it. Plain floats: mesh, like
// world, has no glm.
struct BodyVertex {
    std::array<float, 3> position; // blocks, relative to the body's block
    std::array<float, 2> texel;    // on the skin, 0 to 1
    std::array<float, 3> normal;   // outward, posed
};

// Poses one body into `out`: positions relative to `origin`, the block the
// body stands in, texels on its skin, and outward normals.
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
    float body_yaw = 0.0F;  // radians, the engine's yaw
    float head_yaw = 0.0F;  // the look's, set each tick
    float head_pitch = 0.0F;
};

// One tick's animation, after its move: `moved` is how far the body went
// over the tick, `look_yaw` and `look_pitch` where it looked.
void animate(BodyMotion& motion, Vec3d moved, float look_yaw, float look_pitch) noexcept;

} // namespace vx
```

Per tick, as the game does it for a player:

- **The walk swing.** d is the horizontal length of the move, y ignored; the target is min(4 d, 1); the speed moves 0.4 of the way to the target; the position adds the speed. It follows the move the body made, not the keys: walking into a wall leaves the limbs still, flying swings them.
- **The head** takes the tick's look.
- **The body.** If the body moved more than 0.05 blocks horizontally this tick, its target is the direction it moved, turned half a turn when that direction is between 95 and 265 degrees from the look (walking backward); otherwise its target is where it already faces. It turns 0.3 of the wrapped angle to its target each tick, then is held within 50 degrees of the look.

Between ticks, with t the fraction of the tick: the limb position is position − speed · (1 − t), the limb amount min(lerp(previous speed, speed, t), 1), the yaws interpolated along the shorter way round. For you, the pitch is the live look's, as RFC 0010's camera has it; for Perdix, interpolated. RFC 0010's `WalkingFrame` carries the previous and current `BodyMotion` with the position it already carries; `Walking` runs `animate` after each tick's move, as it updates the bob. A new body starts facing its look with the swing at rest.

### The skins

```cpp
namespace vx::minecraft {

// One of the game's default player skins from the player's jar: `name` is
// one of alex, ari, efe, kai, makena, noor, steve, sunny, zuri; slim arms read
// slim/<name>.png, wide arms wide/<name>.png.
[[nodiscard]] Result<Skin> default_skin(const Archive& jar, std::string_view name, Arms arms);

} // namespace vx::minecraft

namespace vx {

// A skin in the game's file format: a 64 by 64 PNG, or a legacy 64 by 32 one
// made square the game's way.
[[nodiscard]] Result<Skin> read_skin(std::span<const std::byte> png, Arms arms);

} // namespace vx
```

Three kinds, all `Skin`s by the time the renderer sees them:

- **Perdix's own**, `assets/core/textures/entity/player/perdix.png`, wide arms: the character's look, original art made for the engine (open question 3). It is what Perdix wears unless told otherwise, what `--no-minecraft` draws on everyone, and what every published clip shows.
- **The owner's**, `assets/core/textures/entity/player/zlex.png`, slim arms: the skin of the Minecraft account `Zlex`, fetched once on 2026-10-10 with the owner's permission (552 bytes; its SHA-256 is the hash in its texture URL) and committed as an asset. It is what you wear.
- **The game's 18 defaults**: `assets/minecraft/textures/entity/player/{wide,slim}/<name>.png` in the 26.2 jar, 64 by 64 palette PNGs with a transparent entry, alpha 0 or 255 only, their hat layers empty. Read through RFC 0009's reader; nothing of the game's is written anywhere. A missing, unreadable, non-PNG or wrong-sized entry is a warning, and that player falls back to Perdix's own skin.

`read_skin` follows the game's rules for a skin file: a 64 by 64 image as it is; a legacy 64 by 32 one made 64 by 64 by copying its right arm and leg, mirrored, into the left limbs' places; in both, the base layer's regions (0, 0 to 32, 16; 0, 16 to 64, 32; 16, 48 to 48, 64) forced opaque. Any other size is refused. The god panel chooses each player's skin from the list (Perdix's own, `zlex`, the 18 defaults); at startup `--skin <name>[:slim]` sets yours and `--perdix-skin <name>[:slim]` his, where a name is one of those or a path to a PNG.

### Two players

Both players run on the world thread, as RFC 0010's player does since M3: each a `Walking` against the streamed chunks, held in place when its move could touch a chunk not loaded yet. The main thread receives each one's `WalkingFrame` after every tick and draws between the last two. Perdix appears when the world starts, on the ground 5 blocks in front of where you spawn, facing you (open question 8), and again there if he ever falls 64 blocks below the lowest block, as RFC 0010 respawns you.

Perdix's `TickInput` comes from his mode, set in the god panel or at startup with `--perdix`:

- **life**, the default: his life below.
- **follow**: as a tamed wolf follows its owner, he walks toward you once you are more than 10 blocks away and stops within 2 (the distances to be confirmed against the jar's `FollowOwnerGoal` in the pull request), climbing by auto-jump. Unlike the wolf he never teleports: beyond 16 blocks he sprints instead, our choice.
- **still**: he stands where he is; his eyes still follow you as in life.

He stays near you, so his chunks stay loaded and you can find him: in life, a stroll target farther than 24 blocks from you is refused and another picked (open question 7); if a hole or a wall leaves him more than 48 blocks away, he walks back toward you as in follow until within 24.

### Perdix's life

The game's idle mobs run a list of goals, each claiming the legs (MOVE) or the eyes (LOOK) or both, re-chosen every second tick in priority order; a cow's idle list is stroll, look at a player, look around. Perdix runs the same three, with the game's numbers, from a seeded random (the same seed, world and positions give the same life):

| Goal | Claims | Starts | Does | Ends |
|---|---|---|---|---|
| **stroll** | legs | 1 chance in 60, checked every second tick: about every 6 seconds | picks a target: up to 10 tries of a random offset within ±10 blocks across and ±7 up and down, the first that stands on a full block's top with room for his box, within his leash; walks at it | within 1 block of it, or stuck: less than 1 block covered in 100 ticks |
| **look at player** | eyes | 2 chances in 100, every second tick, while you are within 6 blocks and in his line of sight (4.8 while you sneak) | looks at your eyes | after 40 to 79 ticks (2 to 4 seconds), or once you are farther than 6 blocks |
| **look around** | legs and eyes | 2 chances in 100, every second tick, when neither of the others runs | looks at a point one block away at his own eye height, in a random direction | after 20 to 39 ticks |

With nothing running he stands, looking ahead. **Walking at a target** is the game's movement control without its pathfinding: the walking yaw turns toward the target by at most 90 degrees a tick, the keys hold forward at walking speed, and steps are climbed by the auto-jump rule below. **Where he looks** turns his head as the game's look control turns a mob's: the yaw toward its target by at most 10 degrees a tick, the pitch straight to the target's within ±40 degrees, a target held 2 ticks after its last update, and with none, the head back toward the body at 10 degrees a tick. His body follows the player rules above (open question 6): he is a player, and players' bodies lag that way.

**Auto-jump**, as the game's option does it for a player (it is off by default for people; Perdix walks with it on): after each move on the ground with forward held, not walking backward (the move and the look within the dot-product limit of −0.15), with no collision in the two blocks above his head, two probe lines 0.51 blocks above his feet and 0.3 either side reach ahead by the larger of 7 times his speed and this tick's step; the first block box either crosses that rises more than 0.5 and at most 1.2 above his feet, with nothing solid above it, makes him jump on the next tick: RFC 0010's ground jump, 0.42 up, and its 10-tick cooldown.

### The name tag

```cpp
namespace vx {

// In mesh: one line of text to float over a body this frame.
struct Tag {
    Vec3d anchor;           // the top of the text, before the lift
    std::string_view text;  // printable ASCII for now
    bool discreet = false;  // the sneaking player's: one pass, not seen through walls
};

} // namespace vx
```

As the game draws a player's:

- **Where**: at the player's box height above the feet, plus 0.5: 2.3 blocks standing, and 1.5 + 0.5 − 0.125 = 1.875 sneaking. The point is the top edge of the text; the letters hang below it.
- **How it faces**: it turns with the camera, yaw and pitch, never rolled; scaled 0.025 blocks to the font's pixel, its pixel rows counting down; centered by half its width, as a fraction, so an odd width lands on half a pixel.
- **How it looks**: white letters, no shadow, on a box 25% black (alpha 63 of 255) from 1 pixel left of the first letter to the end of the last letter's advance, and from 1 pixel above the line to 9 below it; the box sits just behind the letters.
- **Seen through walls**: unless the player sneaks, it is drawn twice. First with no depth test, so walls do not hide it: the box and the letters at 50% white (alpha 128). Then depth-tested: the letters fully white, where nothing is in front of them. A sneaking player's tag is drawn once, depth-tested, the box and the letters at 50%, and hidden beyond 32 blocks.
- **When**: within 64 blocks of the camera; never the camera's own player, so you never see your own tag while you are in your body; over you, `Zlex`, when the free camera sees you.

**The font** is the game's default font from the player's jar, where the engine can find it, otherwise Perdix's own (open question 10). The game's ASCII glyphs are `assets/minecraft/textures/font/ascii.png`, 128 by 128 pixels in 16 by 16 cells of 8 by 8; a glyph's advance is its ink width plus 1 (the rightmost column with any ink, plus one, then plus one for spacing), a space is 4, a line is 9. `Perdix` is 6 + 6 + 6 + 6 + 2 + 6 = 32 pixels wide, its box 33 by 10: 0.825 by 0.25 blocks. The ink of each glyph is white on transparent; the tag's color is the vertex color.

### Drawing

```cpp
namespace vx::render {

struct RendererDesc {
    // ... as before, and:
    std::span<const Skin> skins;  // uploaded at startup; a pose's `skin` indexes them
    std::optional<Font> font;     // none draws no text
};

// An ASCII bitmap font: a 128 by 128 RGBA8 atlas of 16 by 16 cells, white
// ink on transparent, and each character's advance in pixels.
struct Font {
    std::span<const std::byte> atlas;
    std::array<std::uint8_t, 128> advances{};
};

struct FrameInputs {
    // ... as before, and:
    std::span<const PlayerPose> players; // at most Renderer::max_players, 64
    std::span<const Tag> tags;           // at most 64
};

} // namespace vx::render
```

**Bodies.** Each frame the renderer poses every body into its frame slot's vertex buffer, rewritten every frame as the cubes' origins are: 432 vertices a body, each its position relative to the body's block, its texel and its normal, and one origin a body, its block relative to the camera's block. Positions are posed in doubles and stored as floats relative to the block the body stands in, so they are exact to well under a texel; the camera's fraction is subtracted in the vertex shader, as the cube's is. The body pipeline draws in the opaque pass with the chunks' reversed-Z depth:

- texels from the skin in RFC 0001's texture table, with its nearest sampler;
- texels below alpha 0.1 discarded: the game blends, but no skin it ships has partial alpha, so discarding draws the same pixels without sorting (settled: open question 5);
- no back-face culling, as in the game: through a transparent texel of an outer layer, the inside of the box behind shows;
- light per vertex, the game's: two lights fixed in the world, L0 = normalize(0.2, 1, −0.7) and L1 = normalize(−0.2, 1, 0.7), brightness min(1, 0.6 · (max(0, L0 · n) + max(0, L1 · n)) + 0.4), with n the posed normal reversed for a back face: up 1.0, down 0.4, north or south 0.7395, east or west 0.497.

**Tags.** Each frame the renderer lays out each tag's glyph quads and box, with the camera's rotation, into the frame slot's text buffer, and draws them after the translucent pass, nearest last: the see-through pass with depth testing and writing off, then the normal pass depth-tested, both blended by alpha, texels below 0.1 discarded.

### In the app

- **Walking**, as today, you are the camera: your body and tag are not drawn. Perdix is.
- **The free camera**, on V (was F4), leaves your body standing where it was, as today, and now you see it, in your skin, with `Zlex` over it; and Perdix, wherever he is.
- **The overlay**, on O (was F3), as before. No key is on the F row: a 60% keyboard reaches it only through a function layer.
- **The god panel**, on G: a Dear ImGui panel like the overlay's, that frees the mouse while it is open, as the game's inventory does, and gives it back on a click in the world. It sets Perdix's mode (life, follow, still), brings him to you (5 blocks in front of you, on the ground), and chooses his skin and yours; a skin change applies at once, the skins being in the texture table from startup.
- **At startup**, `--perdix life|follow|still`, `--skin` and `--perdix-skin` set the same things.
- **The startup report** gains the skins and the font: `Skins: Perdix's own (Perdix), zlex (you)`, `Font: Minecraft 26.2, from <the jar>` or `Font: Perdix's own`.

A film tool, or any program on the engine, does the same with its own cameras: it runs the players, poses their bodies from their frames, adds their tags, and places the camera where it likes.

## Alternatives considered

- **Minecraft's F5 views.** What "third person" means in the game; not what was asked for.
- **A model file** with skeletal animation: a loader and bones to describe twelve boxes the skin format already fixes.
- **Posing on the GPU**: worth it for hundreds of bodies (mobs), its own RFC then; 432 vertices on the CPU cost microseconds.
- **RFC 0001's cubes**: whole blocks on the grid with one texture, unrotated.
- **The game's blending with sorted quads** for bodies: exact only for partial alpha, which no shipped skin has.
- **Mob body turning for Perdix** (the body pinned to the walking direction and catching up with the head after 10 still ticks): what mobs do, but Perdix is a player, and players' bodies lag the way RFC 0010's does; one body rule for both players.
- **Pathfinding now**: walking straight and climbing steps gives the game's idle rhythm in open terrain; a path search over streamed chunks is a feature of its own, with its own budget.
- **Tags in the overlay** (screen-space text from Dear ImGui): simple, but never hidden by walls, never sized by distance, and not the game's look.
- **A scripted path instead of a life**: what the film tool does for a shot; in the app the owner asked for a life.
- **F-row keys**, as the game's F3 and F5: out of reach on a 60% keyboard without its function layer.

## Performance budget

On the reference machine, Release build:

| What | Budget | Measured by |
|---|---|---|
| Posing one body | under 5 µs | a bench posing 100,000 bodies, median of 15 runs |
| Perdix's life, one tick | under 5 µs, auto-jump's probes included | a bench of 100,000 ticks over a synthetic world |
| Laying out one tag | under 2 µs | the same bench style |
| 64 bodies and 64 tags, GPU | under 0.1 ms | the frame's GPU time with and without them, Tracy |
| Allocations per frame and per tick | 0 | `vx_render_allocation_tests` and the tick test, with bodies, tags and Perdix |

## Test plan

**In `vx_mesh_tests`**, against tables written from this RFC, not from the code:

- **The layout**: every face of every box, wide and slim, reads the texel rectangle in the table, in the stated direction; outer layers use the unpushed size.
- **The rest pose**: the key points to 1e-4 blocks; the width across the arms 0.9375 wide, 0.8203 slim.
- **Placement**: at yaw π/2 the face looks down −x; crouching moves every point of the legs 2/16 block lower than standing, on top of the crouch pose; every face's normal points out of its box.
- **The swing**: at limb position 0 and amount 1 the right leg is pitched back 1.4 radians about its hip and the left forward 1.4, the right arm forward 1.0 and the left back 1.0; at π / 0.6662 they trade places; at amount 0 nothing swings.
- **The sway**: at age 0 both arms rolled 0.1 outward; at age 34.9 about 0.
- **Tags**: `Perdix` is 32 pixels wide and its box 33 by 10 from (−17, −1); the anchor of a standing player is 2.3 above the feet and a sneaking one's 1.875; the quads face the camera at any yaw and pitch.

**In `vx_world_tests`**:

- **The swing and the body**, flat ground, 20 ticks of walking, sprinting and sneaking: speeds toward 0.863, 1.0 and 0.259; walking into a wall keeps the speed at 0; walking forward from a body 40 degrees off closes 0.3 a tick; strafing settles at 50 degrees; walking backward keeps the body on the look; standing while the look turns 70 degrees moves the body only by the 20 beyond 50.
- **The life**, seeded: over 12,000 ticks of standing alone, strolls start about once every 120 ticks (inside the 99% interval for 1 chance in 60 every second tick); every target is within ±10 and ±7 of where he stood, on a full block's top, inside his leash; with you 5 blocks away in his line of sight he looks at you within a few hundred ticks and for 40 to 79 ticks, and never with a wall between you or at 7 blocks; his head turns no more than 10 degrees in a tick; replaying the seed replays the life.
- **Auto-jump**: walking at a 1-block step he jumps the tick after he reaches it and stands on top; at a 1.5-block wall he never jumps and the stroll ends stuck; with a block over the step's top or over his head he does not jump; walking backward he does not.
- **Follow and still**: in follow he starts after you beyond 10 blocks and stops within 2; in still he never moves.
- **Controls**, as for every item: mutating 0.6662, 1.4, 0.4, 4, 0.3, 50, 60, 10, 7, 0.02, 6, 10 degrees a tick, 0.51, 1.2, a texel offset or a tag offset turns a test red.

**In `vx_minecraft_tests`**, with a synthetic jar from the existing fixture script: a palette skin with a transparent entry decodes to the expected RGBA; a missing, wrong-sized or broken skin falls back with its message; a legacy 64 by 32 skin is made square with its left limbs mirrored and its base opaque; the font's advances come out of a synthetic `ascii.png` as the rule says.

**In `vx_render_tests`**, headless: one body in Perdix's own skin from a fixed camera, front and side, with checked pixels on the face, the arm and the background and its silhouette inside the projected key points; one tag behind a wall, visible at half white, and in the open at full white; validation clean.

**In `app`**, with SDL's dummy video driver and no window shown: O, V and G toggle the overlay, the free camera and the god panel; pressing every F key changes nothing; the panel frees the mouse and a click in the world takes it back.

**By eye**, the owner sets the engine and the game side by side: bodies standing, walking, sprinting, sneaking, strafing, walking backward and turning past 50 degrees; a tag in the open and behind a wall; a cow and Perdix idling side by side, their rhythm compared.

## Pull request plan

1. **The model** (`mesh`): the twelve boxes, wide and slim, the texel layout, posing a `PlayerPose` into triangles with normals; the layout, rest, placement, swing and sway tests.
2. **The animation** (`world`): `BodyMotion` and `animate`; the swing and body vectors.
3. **The skins** (`world`, `minecraft`, `assets`): `Skin`, `read_skin` with the game's rules; Perdix's own and the owner's skins in the core pack; `default_skin` through RFC 0009's reader; the tests.
4. **Drawing bodies** (`render`): `RendererDesc::skins`, `FrameInputs::players`, the body pipeline and its shaders, posing into the frame's buffer; the headless render test and the allocation test with bodies.
5. **Your body** (`app`): `WalkingFrame` carries `BodyMotion`; the free camera sees your body in your skin; the keys off the F row, the overlay on O and the free camera on V; the skin options; the startup report's skin line.
6. **A second player** (`app`, `world`): Perdix on the world thread, spawned before you, his frames to the main thread, the still and follow modes, the leash, `--perdix`, and the god panel on G with his mode and bringing him to you.
7. **Perdix's life** (`world`): the three goals, the look control, walking at a target, auto-jump, the stuck check; the seeded vectors and the controls.
8. **Text** (`minecraft`, `mesh`, `render`): the font from the jar and Perdix's own, the tag layout, the text pipeline with its two passes; the tag tests.
9. **Name tags and skins in the panel** (`app`): Perdix's tag, yours in the free camera, both skins in the god panel, the font line in the startup report. Then As built.

## Open questions

All settled on 2026-10-10 with the owner, as recommended unless noted:

1. **When**: right after M3 closes, as **M3.5 Body**, before M9's and M4's code.
2. **Skins** (changed by the owner): Perdix wears Perdix's own skin; you wear the owner's, `Zlex`, kept in the repository; the game's defaults by choice.
3. **Perdix's own skin**: original art made the way the core pack's eleven textures were, in the owner's texture tool on the Resurrect 64 palette, wide arms. It is the character's look.
4. **Seeing a body move in the app** (changed by the owner): yes, Perdix's, living on his own; yours in the free camera.
5. **Exact trigonometry** rather than the game's sine table, and **discarding** below alpha 0.1 rather than blending, for bodies.
6. **Perdix's body**: the player rules (0.3 a tick, within 50 degrees), not a mob's; he is a player.
7. **The leash**: strolls within 24 blocks of you; beyond 48 he walks back.
8. **Where he appears**: 5 blocks in front of where you spawn, on the ground, facing you.
9. **Choosing** (changed by the owner): UI is welcome, and nothing goes on the F row: the god panel on G for his mode, bringing him over and both skins; the overlay moves to O and the free camera to V; the startup options stay.
10. **The font without the game**: Perdix's own 8 by 8 ASCII font, made in the owner's texture tool like the textures and the skin; until it exists, no tag is drawn without the game.

## As built

Every item landed on 2026-10-10 and 2026-10-11, merged on the local CI (`scripts/ci-local.sh`) and a release build of each stack's top while GitHub's runners were unavailable. Minecraft 26.2's jar was read for numbers and behavior only, never for code.

Item 1, the model, landed as two pull requests: the model at rest (#360); the walk swing, the idle sway and the crouch (#361).

- **Posing a walking body takes 0.87 µs** in a release build on the reference machine, against the budget's 5.
- **The head turns and pitches by exact trigonometry**, as open question 5 settled.

Item 2, the animation, landed as two pull requests: the walk swing and the body between two ticks (#371); the body's turn (#372).

Item 3, the skins, landed as four pull requests: `vx::Skin` and `vx::Arms` (#359); the game's defaults from the player's jar (#363); skin files and the owner's skin (#364); Perdix's own skin (#366).

- **`read_skin` is in `vx::minecraft`**, beside `default_skin`, not in `vx`: `world` has no image decoder.
- **The owner's skin was fetched once** from Mojang's texture server with the owner's permission (open question 2): 64 by 64, 552 bytes, its SHA-256 the hash in its texture URL. Nothing downloads a skin at run time.
- **Perdix's own skin is a placeholder** made by code on the Resurrect 64 palette, with the texture studio's `generate-skin.js`, the way the core pack's textures were: chestnut hair, a slate tunic barred like a partridge's flanks, a rust scarf on the jacket layer, a gold compass pendant. The owner may repaint it.

Item 4, drawing bodies, landed as one pull request (#362).

- **The entity light is the game's** (its entity shader): two lights fixed in the world, 0.6 of each one's cosine plus 0.4, at most 1. A face seen from inside its box, through an outer layer's clear texels, is lit as if it faced the other way, as the game's per-face lighting does it.
- **A skin that was not given draws the missing texture**, magenta and black in squares of 4 texels on the base layer, its outer layer clear.

Item 5, your body, landed as three pull requests: the overlay on O and the free camera on V (#367); skins and the font at startup (#370); your body in the free camera (#383).

- **The options are `--skin=NAME` and `--perdix-skin=NAME`**, with `=` like every other option the app takes.
- **The startup report** reads `Skins: perdix (Perdix), zlex (you), 18 of Minecraft 26.2's` and `Font: Minecraft 26.2, from <the jar>`; without the game, `Font: none, so no name tags without the player's Minecraft`, since Perdix's own font is not made yet (open question 10).

Item 6, a second player, landed as five pull requests: Perdix starts over where he is put (#384); Perdix on the world thread (#385); Perdix in the app (#386); G opens the god panel (#387); the god panel (#388).

- **Bring him here puts Perdix 5 blocks ahead of the camera**, which in the free camera is not where your body stands: the first spot from 4 blocks above that point to 64 below where his standing body fits on something, so a camera high over the ground brings him down to it; with no such spot he stays. His life starts over there (`Life::start_over`): run in the app, his head first turned back for a tick to where his life still looked.
- **G closing the panel gives the mouse back only when the panel took it**, so a run never clicked never grabs the mouse; a click in the world captures it and closes the panel; Escape frees it and closes the panel. The rules are `control()`'s, tested without a window.
- **Perdix may not fly**: his auto-jump presses Space, and the game never counts those toward a double tap.
- **He walks through you**: nothing pushes two players apart yet (interacting with Perdix is a non-goal), so a stroll can end inside your body.

Item 7, Perdix's life, landed as eight pull requests: the look control (#373); the goals and looking around (#374); looking at you (#375); where he strolls to (#376); strolling (#377); the game's auto-jump (#378); climbing steps (#379); follow, still and the leash (#380).

- **Looking around lasts 22 to 40 ticks, and looking at you 40 to 80**: the game counts down every tick, or in choices, but ends a goal only at a choice, so the RFC's 20 to 39 and 40 to 79 come out so in its jar.
- **The stuck check takes the cow's one block in 100 ticks.** The jar's rule scales with the mob's speed, which for a player's 0.1 would be a quarter block.
- **Auto-jump keeps the game's quirks:** strafing climbs; walking backward climbs while looking down past 81.4 degrees; a diagonal probe meets everything in the rectangle its ends span; a farther wall two blocks high vetoes a nearer step walking toward -z, not toward +z.
- **Follow's distances are the jar's 10 and 2**, as the RFC guessed. The wolf aims again every 10 ticks, which Perdix does, and teleports from 12 blocks, which Perdix replaces with the sprint beyond 16; RFC 0010's sprint lasts while forward is held, so once sprinting he sprints until he stops.
- **A tick of Perdix** (his life, the move, the look ahead, the animation) is benched in `bench/world/life_bench.cpp`.

Item 8, text, landed as three pull requests: the game's font from the player's jar (#365); the name tag's layout (#368); name tags drawn as the game draws them (#369).

- **`Font` is an owning plain type in `world`**, beside `Skin`, not a span in `render`, so the `minecraft` module makes one and Perdix's own font will be the same type. Its atlas is fixed at 128 by 128, so its scale is always 1, where the game would scale a larger pack's.
- **Code points 0 to 31 and 127 draw the missing glyph**, as the game draws them from its jar's fonts.
- **A tag's anchor is the text's top edge, already lifted**: 2.3 above a standing player's feet, 1.875 above a crouching one's.
- **The 0.1 discard changes no pixel today**: tags are drawn last, the farthest first, so nothing lies behind a discarded texel. It is kept to match the game's text pipeline.
- **The box blends in linear light**, between the sRGB swapchain's conversions: over the sky it comes out (105, 147, 225) where the game's would be about (90, 126, 192). It is RFC 0009's note on shading again, and lighting (M6) owns it.

Item 9, name tags and skins in the panel: the tags over Perdix and over your body in the free camera came with items 5 and 6, both skins in the god panel with item 6, and the font's line with the startup report. Perdix's own 8 by 8 font, to be made in the owner's texture tool, is still to come; until it exists no tag is drawn without the game (open question 10).
