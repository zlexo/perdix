# RFC 0004: Block registry and block textures

| | |
|---|---|
| **Status** | Accepted (2026-10-02) |
| **Milestone** | M2 First Chunk |
| **Issue** | #21 |

## Summary

What a block is and how it looks. Blocks are defined in data files under namespaced names such as `core:stone`. Every combination of a block's property values becomes one 16-bit `BlockState` (RFC 0002), numbered deterministically. The registry builds the per-state `BlockVisual` table that RFC 0003's mesher reads, and block textures become one GPU array image (RFC 0001) with mipmaps and filtering made for 16 by 16 pixel art, so blocks stay sharp up close and stable at the distances the north star asks for.

## Motivation

RFC 0002 made every block a 16-bit number, and nothing yet says what a number means. Generation writes these numbers, the mesher reads them for every block it meshes, and saves will store them. That needs three things:

- **Stable numbers.** A test chunk or a saved world has to mean the same blocks tomorrow and on another machine.
- **A one-read lookup.** The mesher asks what each block looks like. A table indexed by state answers in one array read, at 14 bytes per state: under 14 KiB for a thousand states.
- **Textures that hold up far away.** At 1440p with RFC 0001's 70 degree field of view, a 16-texel face seen head-on drops below one pixel per texel at about 64 blocks (74 on the reference display's 1,664 rows). Beyond that, unfiltered textures shimmer, trilinear filtering alone blurs ground seen at a shallow angle, and naive mipmaps darken the edges of leaves and thin them out. Minecraft fixed its dark mipmap edges only in release 1.21.11, in December 2025. At long view distance, most of the screen is that far away.

## Goals and non-goals

Goals:

- Blocks are data: one file per block, and no rebuild to add or change one.
- Names are namespaced (`core:oak_log`), which also keeps the door open to mods, a non-goal for now (see the vision). Every state has a text key (`core:oak_log[axis=x]`) that converts to its id and back.
- Ids are a pure function of the definitions: the same on every machine and in every run, whatever order the file system lists files in. `core:air` is 0.
- One immutable `BlockVisual` table, indexed by state, shared by every mesher thread without locks.
- All block textures in one array image: 16 by 16, five mip levels built for pixel art, sharp up close and free of shimmer far away.
- A bad definition or texture stops startup with every problem listed by file, line and column.

Non-goals:

- Non-cube shapes (slabs, stairs, plants, fences): the block models RFC.
- Behavior, ticking, drops and sounds: later RFCs, which add their own keys to the block files.
- Translucent blocks and water (M7); light emission and how light passes through blocks (M6).
- Biome tinting, per-position texture variation, animated and connected textures, and changing blocks while the game runs (a restart is cheap).

## Prior art

- **Minecraft, Java Edition.** Since 1.13 ("the Flattening"), blocks have namespaced names and states made of boolean, integer and enumerated properties. Each block owns one contiguous range of numeric state ids, with its properties in name order and the last one varying fastest, as the data generator's block report shows. The numbers move between versions; saves store each section's palette by name and properties instead. Blockstate files map property combinations to models turned in 90 degree steps. Release 1.21.11 added per-texture mipmap strategies (a cutout strategy, picked automatically for textures with fully transparent pixels), fixed dark mipmap edges around transparent pixels (MC-114265), and added anisotropic filtering and a shader-based filter (RGSS) as options. We take the names, the state model, the order within a block, names in saves, turns in 90 degree steps, and mipmap handling chosen from the image itself.
- **Minecraft, Bedrock Edition.** Custom blocks are JSON data: states with boolean, number or string values, and permutations whose conditions on those states override components. Above 65,536 permutations a world warns, and the debug log lists the blocks with the most. We take conditional overrides, and naming the largest blocks when the state space overflows.
- **Luanti.** Nodes are registered from Lua with up to six face textures in a fixed order. Numeric content ids hold only while the game runs; each saved map block carries its own name-to-id table.
- **Cubyz.** One data file per block, with side, top and bottom textures and a named rotation mode, plus a migrations file that maps renamed blocks to their new names. We take one file per block; renames are left to the world format (M8).
- **Ignacio Castaño, "Computing Alpha Mipmaps"** (2010): scale each mip level's alpha so the share of texels passing the alpha test matches the full-size texture, so foliage keeps its density at a distance. NVIDIA Texture Tools implements it.
- **Hans-Kristian Arntzen, "Pseudo-bandlimited pixel art filtering in 3D"** (2018): pixel art that stays sharp up close without aliasing, from bilinear fetches at shifted coordinates, handing over to hardware mipmapping and anisotropic filtering when minified.
- **Khronos Vulkan Tutorial**: loads textures with stb_image and builds mipmaps on the GPU with blits. We take stb_image and build mipmaps on the CPU, where the pixel-art rules below can be applied and tested.

## Design

### Block files

The block `core:oak_log` is the file `assets/core/blocks/oak_log.toml`, and the texture `core:oak_log` is `assets/core/textures/blocks/oak_log.png`: the folder is the namespace, the file name is the name. Names, properties and values use `a` to `z`, `0` to `9` and `_` only, so a pack loads the same on macOS's case-insensitive file systems as on Linux. `core:air` is built in, not a file: it is always state 0 (RFC 0002), and nothing can redefine it.

```toml
# assets/core/blocks/oak_log.toml
[properties]
axis = ["y", "x", "z"]        # the first value is the default

[textures]                     # names in the block's namespace unless written ns:name
side = "oak_log"
top = "oak_log_top"
bottom = "oak_log_top"

[[variant]]
when = { axis = "x" }
rotate_x = 90                  # the top turns to the north...
rotate_y = 90                  # ...then the north turns to the east

[[variant]]
when = { axis = "z" }
rotate_x = 90
```

| Key | Meaning | Default |
|---|---|---|
| `opacity` | `opaque`, or `cutout` for leaves and glass | `opaque` |
| `cull_group` | a name; cutout blocks with the same group hide each other's shared faces (RFC 0003, open question 2) | none |
| `properties` | property name to its values: strings, integers or booleans | none |
| `textures` | face to texture: `all`, `side`, `top`, `bottom`, `north`, `south`, `east`, `west` | required |
| `variant` | `when` (property to one value or a list), then `textures`, `rotate_x`, `rotate_y` | none |

Directions follow Minecraft: north is -z, east is +x, top is +y. `rotate_x = 90` turns the block so its top faces north; `rotate_y = 90` turns its north face to the east, clockwise seen from above. For each state, the registry starts from `textures` (a single face beats `side`, which beats `all`), applies the `textures` of every matching variant in file order, then turns the block by the sum of the matching `rotate_x` and then of `rotate_y`. A stone is two lines. A furnace is one `lit` variant for its front and three `rotate_y` variants for its other facings. Unknown keys are errors, so a later RFC that needs a key (light emission, sounds) adds it on purpose.

### States and ids

A block's states are every combination of its property values. Properties are taken in name order and the last one varies fastest, which is Minecraft's order within a block; values keep the order the file lists them in. Ids are assigned in one pass:

1. `core:air` gets 0.
2. Every other block, sorted by name in byte order, gets the next contiguous range, one id per state.
3. More than 65,536 states in total is an error that names the largest blocks.

| Block | Properties | States | Ids |
|---|---|---|---|
| `core:air` | | 1 | 0 |
| `core:furnace` | `facing`: north, east, south, west; `lit`: false, true | 8 | 1 to 8 |
| `core:oak_log` | `axis`: y, x, z | 3 | 9 to 11 |
| `core:stone` | | 1 | 12 |

In this pack, `core:furnace[facing=east,lit=true]` is 1 + (1 × 2 + 1) = 4: the block's first id, plus facing's index times lit's two values, plus lit's index. Changing one property (`with`) is the same arithmetic on the id, with no table of neighbor states.

Ids depend only on the set of definitions, so every run and machine agrees and tests that build chunks are reproducible. They do change when a block is added, removed or renamed. A world therefore stores the key of every id it uses, the way Minecraft and Luanti store names, and the world format (M8) remaps on load when the table differs: a palette of at most 256 entries per paletted chunk, one state per uniform chunk, and only a direct-mode chunk rewrites every block (RFC 0002). M8 also decides what replaces a key that no longer exists. Tests name states by key, never by number, and one golden test pins the numbering of a fixed test pack, so a change to the algorithm shows up as a diff.

### The visual table

The registry fills RFC 0003's `BlockVisual`, one per state, indexed by RFC 0002's `Face` (+x, -x, +y, -y, +z, -z). `Opacity` and `BlockVisual` live in `world` (RFC 0003), because `world` builds them and cannot include `mesh` headers (ADR 0003); `mesh` includes them from there. The cull group joins them: `{ Opacity opacity; std::uint8_t cull_group; std::array<std::uint16_t, 6> texture; }` is still 14 bytes. Group 0 means none, and named groups are numbered in sorted order, like ids.

A texture layer stands for a texture plus a number of clockwise quarter turns. Turning a block turns its textures within each face (a log on its side has its bark running along the log), and every turned texture is baked into a layer of its own at load. The quad and the shader never deal with rotation, and turned faces never merge with unturned ones, because their layers differ. Turned copies of symmetric textures are duplicates; at 1,364 bytes each they are not worth detecting. Layer 0 is a built-in magenta and black checker, so a zeroed or missing entry is visibly wrong instead of quietly showing another block's texture.

Cutout faces need an alpha test. On Apple GPUs, fragments that can be discarded are handled less efficiently by hidden surface removal, and Apple asks for them to be drawn after all opaque geometry. RFC 0003's `ChunkMesh` keeps opaque and cutout quads in separate ranges per face, so RFC 0007 draws them in that order.

### Building the texture array

Every texture is a 16 by 16 PNG, read as sRGB with alpha. Seen from outside the block, every face shows its image unmirrored: on side faces the image's top edge is up, on the top face it points north, on the bottom face south. The world layer only names textures; the render layer turns that list into pixels on the CPU, layer by layer:

1. Decode the PNG to RGBA8 with stb_image and check that it is 16 by 16.
2. Turn it by its quarter turns.
3. Fill every fully transparent texel with the color of the nearest visible texels, spreading outward and wrapping at the edges, because textures repeat. Its alpha stays 0, and filters that reach into it no longer pull in black or white fringes.
4. Build the 8 by 8, 4 by 4, 2 by 2 and 1 by 1 levels, each directly from level 0, averaging in linear light with color weighted by alpha. Where every source texel is fully transparent the weights sum to zero, and the color falls back to the plain average of the colors filled in by step 3, so distant leaves never turn black. Averaging the sRGB bytes would darken contrasty textures: black and white average to 128, which is 22% of the light, instead of 188, which is 50%.
5. If any texel is below 0.5 alpha, where the alpha test discards it, scale each level's alpha so the share of texels that pass matches level 0 (Castaño). Leaves keep their density at a distance.

The result is one `VK_FORMAT_R8G8B8A8_SRGB` 2D array image: 16 by 16, five levels, one layer per texture, 1,364 bytes per layer, so 2,048 layers take 2.7 MiB. A single buffer-to-image copy with five regions, one per level, uploads it. Vulkan guarantees 256 layers on a 1.3 device and 2,048 on a 1.4 device such as KosmicKrisp; startup compares the layer count with `maxImageArrayLayers` and fails with both numbers if the pack needs more.

The array gets its own binding in RFC 0001's global descriptor set, next to the array of separate images. Putting each block texture in that array instead would make the quad's texture a descriptor index that varies within a draw, which needs non-uniform indexing; a layer is just a third texture coordinate.

### Filtering

Nearest filtering, which RFC 0001's cube uses, keeps texels sharp, but their edges alias and crawl as the camera moves, and anisotropic filtering cannot be relied on with a nearest filter: the Vulkan specification notes that vendors interpret such samplers differently. Without anisotropic filtering, ground seen at a shallow angle turns blurry close to the player, and at long view distance most of the screen is ground at a shallow angle.

The chunk shader instead samples with a linear sampler (repeat addressing, trilinear, anisotropic when the device offers `samplerAnisotropy`) and sharpens the coordinate itself:

```glsl
// t: texture coordinate in texels. Inside a texel, sample its center; within
// half a pixel of a texel edge, blend across it. When texels are smaller than
// pixels, t is unchanged and mipmaps and anisotropic filtering do the work.
vec2 seam = floor(t + 0.5);
vec2 sharp = seam + clamp((t - seam) / clamp(fwidth(t), 1e-6, 1.0), -0.5, 0.5);
vec4 color = textureGrad(blocks, vec3(sharp / 16.0, layer), dFdx(t) / 16.0, dFdy(t) / 16.0);
```

`textureGrad` with the unsharpened derivatives keeps mip and anisotropy selection on the true footprint. Up close this looks like nearest filtering with clean edges; far away it is ordinary hardware filtering. Plain nearest stays available as an overlay toggle (open question 3).

### Validation

Loading collects every problem before it stops, and prints them in the format compilers use, so terminals and editors link to the line:

```
assets/core/blocks/oak_log.toml:12:12: error: rotate_x must be 0, 90, 180 or 270, not 45
assets/core/blocks/glass.toml:1:1: error: unknown key 'opacty'; did you mean 'opacity'?
```

Errors: TOML syntax; unknown keys and wrong value types; bad characters in a name, property or value; a file that redefines `core:air`; a property with one value or a repeated value; a variant naming an unknown property or value, turning by other than 90 degree steps, or changing nothing; a face left without a texture; a `cull_group` on a block that is not cutout; more than 65,536 states; a texture file that is missing, is not a PNG or is not 16 by 16. Warnings: transparent texels in a texture an opaque block uses, none in a texture only cutout blocks use, and texture files no block uses. Any error stops startup with a non-zero exit code. The test that loads the shipped pack also fails on any warning, so main stays clean.

### API sketch

```cpp
namespace vx {

// Opacity and BlockVisual are RFC 0003's, declared in world, with the cull group above.
// Diagnostic (core) holds a severity, a file, a 1-based line and column, and a message.

enum class BlockType : std::uint16_t { air = 0 };   // one per block: air, then name order

struct TextureRef {
    std::string name;                 // "core:oak_log"
    std::uint8_t quarter_turns;       // 0 to 3, clockwise
    bool used_opaque, used_cutout;    // by which kinds of block, for the warnings
    std::string first_use;            // "assets/core/blocks/oak_log.toml:6:8", for messages
};

struct SourceFile { std::string path; std::string text; };
struct BlockLoad;   // the registry, or why it could not be built

class BlockRegistry {
public:
    // Every assets/<namespace>/blocks/*.toml under root, or in-memory files for tests.
    [[nodiscard]] static BlockLoad load(const std::filesystem::path& root);
    [[nodiscard]] static BlockLoad load(std::span<const SourceFile> files);

    // "core:furnace[lit=true]": properties left out take their defaults.
    [[nodiscard]] std::optional<BlockState> find(std::string_view key) const;
    // Every property, in name order: "core:furnace[facing=north,lit=true]".
    [[nodiscard]] std::string key(BlockState state) const;
    [[nodiscard]] std::optional<BlockState> with(BlockState state, std::string_view property,
                                                 std::string_view value) const;
    [[nodiscard]] BlockType type_of(BlockState state) const noexcept;
    [[nodiscard]] BlockState default_state(BlockType type) const noexcept;

    [[nodiscard]] std::span<const BlockVisual> visuals() const noexcept;   // indexed by BlockState
    [[nodiscard]] std::span<const TextureRef> textures() const noexcept;   // indexed by layer
};

struct BlockLoad {
    std::optional<BlockRegistry> registry;   // empty when any diagnostic is an error
    std::vector<Diagnostic> diagnostics;     // warnings come back on success too
};

} // namespace vx

namespace vx::render {

// CPU only, tested without a GPU: decode, turn, fill, mipmaps. RGBA8 sRGB,
// every layer of level 0, then every layer of level 1, and so on.
[[nodiscard]] std::vector<std::byte> build_block_textures(std::span<const TextureRef> refs,
                                                          const std::filesystem::path& assets,
                                                          std::vector<Diagnostic>& diagnostics);

} // namespace vx::render
```

The registry is built once at startup and never changes. Every method is `const`, so generator and mesher threads (RFC 0005) share it without locks, and the generator (RFC 0008) looks up the states it places by key once, at startup. `BlockLoad` follows RFC 0000's error convention (open question 1 there): errors are values.

### Dependencies

| Dependency | Version | Used by | Why |
|---|---|---|---|
| toml++ | 3.4.0 | world | TOML 1.0, MIT license, works without exceptions, line and column for every value; compiled once instead of header-only |
| stb_image | 2.30 | render | one header, public domain or MIT; built with `STBI_ONLY_PNG` in its own target, outside our warning flags; pinned to a commit, because stb has no release tags (RFC 0000's rule for libraries without releases) |

## Alternatives considered

- **C++ tables** and **pinned ids**: open questions 1 and 2.
- **JSON**, as Minecraft and Bedrock use: familiar, but no comments and more punctuation in files people edit by hand.
- **Lua definitions**, as in Luanti: a scripting runtime for what is plain data, and scripting is a non-goal.
- **One file per namespace:** easy to read at twenty blocks, but every new block edits the same file. One file per block keeps diffs small, and its path is its name.
- **Rotation in the quad:** two bits per face in `BlockVisual` and in the quad, and work in the shader. A baked layer costs 1,364 bytes per turned texture instead.
- **One image per texture in RFC 0001's array:** an image, a view and a descriptor for every 1,364 bytes of pixels, and non-uniform indexing in the shader.
- **Mipmaps on the GPU with blits**, the Vulkan Tutorial's way: no control over alpha coverage or transparent texels, and no tests without a GPU. On the CPU, a 16 by 16 texture's mipmaps take microseconds.
- **A texture file built at compile time:** a tool and a build step to save milliseconds at startup.
- **Other PNG decoders:** lodepng (zlib license, also encodes), libspng (BSD license, needs zlib or miniz), Wuffs (memory-safe, a larger API). stb_image's README warns that its security fixes can take time. That matters for untrusted files, and these files are our own.

## Performance budget

Targets on the reference machine, release build. CPU numbers from nanobench, GPU numbers from Tracy's GPU zones (RFC 0001).

| What | Target |
|---|---|
| Load and validate the shipped M2 pack | under 5 ms |
| Load a synthetic pack of 1,000 blocks and 10,000 states, from memory | under 50 ms |
| Decode, turn and mipmap 1,000 textures on one thread | under 50 ms |
| `visuals()[state]`, `type_of(state)` | one array read each |
| Visual table | 14 bytes per state |
| Texture array | 1,364 bytes per layer |
| Sharpened filtering over plain nearest, M2 scene at 1440p | under 0.2 ms more GPU time |

## Test plan

- **Ids:** on a fixed test pack, air is 0, blocks follow name order and states follow the mixed-radix order; `find` and `key` round-trip every state; `find` fills left-out properties with defaults; `with` changes one property and nothing else; the same files loaded in shuffled order give identical ids, visuals and textures; the golden key table matches.
- **Faces:** a log with `axis=x` has its end texture on +x and -x and its bark turned along x; a furnace facing east has its front on +x; every combination of `rotate_x` and `rotate_y` matches a hand-computed face map.
- **Validation:** a corpus of known-bad files, at least one per error and warning listed above, each with its expected message, so every check is shown to fire.
- **Textures, without a GPU:** a known PNG decodes to known bytes; a 17 by 16 file is rejected with its message; quarter turns of an asymmetric texture; level 1 of a one-texel black and white checker is sRGB 188; a cutout texture keeps level 0's coverage to within one texel at every level; fully transparent texels get their neighbors' color.
- **Shipped pack:** loads with no errors and no warnings.
- **GPU, on lavapipe in CI:** upload the array with validation on and read back one layer at every level; it matches the CPU data byte for byte.
- **By eye on the reference machine,** on KosmicKrisp and on MoltenVK: the M2 chunk near, far and at shallow angles with both filters; leaves do not vanish, darken or grow fringes at a distance.

## Pull request plan

1. This RFC.
2. `Diagnostic` in `core`; toml++; parsing block files into definitions with every validation rule, and the bad-file corpus.
3. `BlockRegistry`: id assignment, keys, `find`, `with` and `type_of`, with the determinism and golden tests.
4. Faces: texture lookup, variants, turns, the `BlockVisual` table and the texture list.
5. stb_image and the CPU texture pipeline: decode, turn, fill and mipmaps, with tests.
6. The GPU array: upload, the new binding in the global set and the sampler, with the lavapipe readback test.
7. The `core` pack for M2: blocks, textures (original 16 by 16 pixel art, open question 4) and `assets/LICENSE`, with the shipped-pack test.
8. The sharpened filter, anisotropic filtering and their overlay toggles, with GPU time before and after.

## Open questions

All settled on 2026-10-02: every recommendation below was accepted.

1. **Data files or C++ tables.** C++ tables need no parser and catch some mistakes at compile time, but every content change is a rebuild, content lives inside engine code, and mistakes surface as compiler errors instead of a file and a line. Recommendation: **data files, in TOML**: a restart instead of a rebuild, errors that point at the content, and the same files for a dedicated server later.
2. **Derived or pinned ids.** Pinned ids, from a checked-in table that gives every key its number forever, would let saves skip the remap. The cost is a generated file that every branch adding blocks edits by hand, and gaps when blocks are removed. Recommendation: **derived ids**, with saves storing keys, as Minecraft and Luanti store names.
3. **Filtering, the owner's call by eye.** Plain nearest is the classic look and what RFC 0001's cube uses. The sharpened filter adds clean texel edges and makes anisotropic filtering usable. Recommendation: **the sharpened filter with anisotropic filtering** by default, its level set from measurements, with plain nearest kept as a toggle.
4. **Art direction, the owner's taste call.** Minecraft's textures are copyrighted and are not used, nor is anything traced from them.
   - *Original pixel art made for the project:* a look of its own and unambiguous ownership. It costs drawing time: M2 needs about ten textures, M4 a few dozen.
   - *A CC0 pack:* Kenney's Voxel Pack is CC0, but its tiles are 128 by 128 in a flat vector style, not 16 by 16 pixel art. OpenGameArt hosts CC0 16 by 16 block sets, but their status rests on each uploader's word and their origin cannot be checked. The best-known open 16 by 16 packs, Pixel Perfection (CC BY-SA 4.0) and Minetest Game's (CC BY-SA 3.0), are not CC0.
   - *Procedural textures:* no drawing and no license question, but noise at 16 by 16 tends to look generic, and the generator is code to maintain.

   Recommendation: **original pixel art**, drawn at 16 by 16 on one small shared palette, with its license stated in `assets/LICENSE`.

## As built

Pull request plan item 2, block files, landed as two pull requests: `Diagnostic`, toml++ and the parser, then the rules with the corpus of bad files.

- **`Diagnostic`'s line and column are 0 for a problem with a whole file or line**, and `format` leaves them out: `assets/core/stone.toml: error: ...`.
- **toml++ keeps a table in key order**, so a file's diagnostics are sorted into the file's order once it has been read, and a definition lists textures and conditions in the order the file writes them.
- **Three more errors than the RFC lists:** a file outside `assets/<namespace>/blocks/`, a block file without a `[textures]` table, and a condition that names no values. A property with no values at all falls under the one-value rule, and its message says "no values".
- **A variant changes nothing when it names no textures and no turn.** The check reads the file, not the faces it resolves to, so a variant that restates the block's own textures passes it.
- **toml++ 3.4.0 is patched at every configure** by `cmake/VxPatchTomlplusplus.cmake`: upstream's fixes for a table header that starts with neither a key character nor a quote, for non-ASCII characters outside any whitespace block, and for dotted keys deep enough to overflow the stack, all made after 3.4.0, and a guard of our own for an array value that starts with `}`. Without them, four short malformed files stopped a debug build on toml++'s assertions, with no message naming the file. Fuzzing found them.
- **Integer and boolean values are kept as text**, so every state has a text key such as `core:wheat[age=1]`.
- **A guess ("did you mean") needs one near miss nearer than any other**, at one edit for every three letters and two at most: `w` is as near to `x` as to `y`, so it gets none.
- **The rules across keys run only on tables read without a problem**, so one misspelt face does not also leave six faces without a texture. A misspelt key with a clear guess counts as that key read with a problem, and so does an opacity that cannot be read: `[propertes]` does not also leave every condition naming an unknown property, a misspelt `[textures]` is not also missing, and a misspelt or unreadable `opacity` does not turn a cull group into an error.
- **The corpus is one file per error in `tests/world/bad_blocks`**, each ending in comments that give the path it stands for and the messages it must give. With the expectations at the end, its line numbers are its own, and both example messages above come out exactly.
- **`Opacity` lives in `block_visual.hpp`** from item 2 on; `BlockVisual` joins it with item 4.

Pull request plan item 3, the registry:

- **`load` takes files given in memory**; reading a pack from disk comes with the shipped pack (item 7). Files are taken in path order, whatever order they arrive in, so the diagnostics come out the same.
- **Names, not paths, number the blocks.** The two orders agree within a namespace, but not across namespaces such as `a` and `a0`: `/` sorts before `0`, and `:` after it.
- **A block defined twice is an error**, which only files given in memory can cause.
- **Too many states are reported on the largest block's file**, with the three largest blocks named.
- **`state_count`** joins the sketch, for tests and tools that walk every state.
- **Loading the synthetic pack of 1,000 blocks and 10,000 states takes 5.5 ms**, the median of five release runs on the reference machine, against a budget of 50.

Pull request plan item 4, the faces:

- **Within one `textures` table, a single face beats `side`, which beats `all`; across tables, a later one wins whatever key it uses.** A variant's `side` covers a `north` the block named on its own, so a variant restates a face it wants to keep.
- **Layers come in name order, then by quarter turns**, after the checker, which is layer 0 with an empty name. `first_use` is the first use in id order, at the texture's value in its file, as diagnostics place it.
- **A quarter turn moves the image's top edge to its right**, clockwise as seen from outside the face. The sixteen combinations of `rotate_x` and `rotate_y` are worked out while compiling, by turning each face's direction and its image's top edge together; two compile-time checks pin the oak log above, and a test checks all sixteen against a table worked out apart from the engine.
- **Two more errors than the RFC lists:** more than 65,536 layers, the checker among them, which a face's 16 bits cannot number, reported where the first texture past the limit is used; and more than 255 cull groups, which a state's 8 bits cannot number, reported where the first group past the limit is named.
- **Each block's variants are read once**, into value masks and indices, so the work for each state compares numbers, never text.
- **Loading the synthetic pack now takes 6.6 ms**, the median of five release runs on the reference machine, against 5.5 ms without the faces (measured again the same day): about 110 ns a state, against a budget of 50 ms for the whole load.

Pull request plan item 5, the textures on the CPU:

- **stb_image is one file fetched by URL at a commit and checked against its hash**, not a clone: commit `013ac3b`, the last to change `stb_image.h` (2.30). It is built without stb's file, float and HDR loaders; the engine reads files itself.
- **One scale cannot keep coverage: ties can't be split.** Castaño scales every alpha by one factor, but pixel art is full of texels of equal alpha, which cross the test together: on hard-edged leaves, a single factor missed level 0's coverage by up to six texels of 64. The factor now puts the k-th most opaque texel exactly at the test, and ties there are broken by an ordered-dither (Bayer) rank, moving those texels one step of 255 to their side. The count is exact, and the texels that pass are spread out rather than taken row by row. A box with no covered texel at all stays a hole.
- **The alpha test passes at 0.5 or more**, an 8-bit alpha of 128; "transparent" in the warnings means below it.
- **Filling spreads a ring at a time** through the four neighbors, wrapping, each texel taking the average of its filled neighbors in linear light. A texture with nothing visible is left as it is.
- **Light is stored back as sRGB by a binary search** over the 255 points where the curve reaches each half step, which rounds as the curve does. Over 20 million points between 0 and 1 it agrees with the formula everywhere but on exact rounding ties.
- **A missing or unreadable file is reported on the PNG's path**, naming the texture and a place that uses it: where its least-turned layer is first used, which is not always the earliest use, since each turn keeps its own. The warning for texture files no block uses comes with reading a pack from disk (item 7), which lists the folder.
- **The layout is level by level**: every layer of level 0, then every layer of level 1, and so on, for one copy with a region per level. The checker is squares of 8 texels.
- **Building the array for 1,000 textures takes 23.2 ms**, the median of five release runs on the reference machine, against a budget of 50. That includes reading 1,000 files, which the budget does not count; opening them takes about a third of the time.

Pull request plan item 6, the GPU array, landed as two pull requests: the array image and its upload, then set 0's binding with the sampler.

- **`Image` takes mip levels and array layers** through `create_array`, whose view is an array even with one layer, as a shader that declares an array expects.
- **`upload_image` fills every level and layer in one copy**, a region per level, from bytes laid out level by level: the copy described above, made general, so the cube's checker and the block array upload the same way.
- **Exactly `maxImageArrayLayers` layers are allowed.** One more is refused with both numbers before Vulkan sees the request; without the check, that request aborts the program on KosmicKrisp. `Image::create_array` itself refuses layers outside 1 to that limit, and mip levels outside 1 to the level of 1 by 1, for any caller.
- **The array can be copied out of** as well as into, for the readback test and for tools.
- **The readback test also runs on both of the reference Mac's drivers**, KosmicKrisp and MoltenVK, chosen with `VK_DRIVER_FILES`.
- **Set 0's binding 2 is the array and binding 3 its sampler**, fixed in the layout: linear between texels and between levels, and repeating. The array counts against the fragment stage's sampled images, so the texture table holds one texture fewer where that limit binds: 255 on MoltenVK.
- **Until the core pack, the renderer binds an array of one layer**, the checker, made and uploaded at startup, so binding 2 always holds an image.
- **A probe shader in the tests samples the array through set 0** at chosen points, layers and levels, and tells the sampler's filtering and addressing apart: a texel's edge reads half its neighbor's light, a point between two levels half of each, and a point past the edge the texel at the other side. `vx_add_shaders` gained what calling it from the tests' directory needed.

Pull request plan item 7, the core pack, lands as two pull requests: reading a pack from disk with the warning for texture files no block uses, then the pack itself, its license and the shipped-pack test, with the app loading it.

- **`load(root)` reads every `assets/<namespace>/blocks/*.toml` under the root**, and nothing else in those folders, with each path relative to the root, so the diagnostics name files as the tests do. Engine code builds without exceptions, so every file system call takes an error code.
- **Names match exactly, whatever the file system.** The walk reads each folder's listing instead of asking for a path by name, which the Mac's case-insensitive file system answers for `Blocks` too, and Linux's does not. A folder or a file ending that differs only in case is an error naming both spellings. Files whose names start with a dot are skipped: `.DS_Store`, and the `._` files macOS leaves on other file systems.
- **No assets folder, one without a namespace, no block file, or a folder or file that cannot be read is an error**, and any error leaves no registry. A link to nothing named like a block file is a file that cannot be read. Every file that could be read is still parsed, so one unreadable file does not hide the others' problems.
- **Files are read with C's stdio** (`read_file`, in core), for block files and textures alike: through an istream, a read that fails partway throws on Linux's standard library, which engine code cannot catch, and passes for the end of the file on the Mac's.
- **A block file's path has exactly four parts**, `assets/<namespace>/blocks/<name>.toml`, in memory as on disk.
- **A texture file that no block uses is a warning on its path.** The texture files are listed on the walk's first pass, with the block files, so a folder of them that cannot be read stops the load like any other; they are compared with the registry's textures once it is built.
- **The pack's 16 by 16 art comes from the owner's tool**, outside this repository, which previews each texture through this engine's own `build_block_textures` (decided 2026-10-05). Its license is the repository's: all rights reserved.
- **For now the 11 textures are placeholders generated by code** in that tool, on the Resurrect 64 palette, the owner's call the same day in place of drawing them first; they are original, and any of them can be repainted there.
- **`load_block_pack(root)`, in the render layer, gives the registry and then the texture array's bytes**, each step only when the one before it had no error, so a pack whose blocks fail builds no textures. The textures' diagnostics name their files relative to the root, as the block files' do. The app and the shipped-pack test call the same function, so the test covers the app's own path to the pack.
- **The shipped-pack test also pins what every state shows**: 12 states, 16 layers, and each face's texture and quarter turns, so a swapped or unturned face in the pack fails it.
- **The renderer takes the array's bytes in `RendererDesc::block_textures`** and sizes the GPU array to their layers. Bytes that are not whole layers are refused before anything is made. Without bytes it binds the one-layer checker array as before, so a renderer needs no pack, as in the tests.
- **Where the pack is at run time, which no RFC said, was the owner's call on 2026-10-06: as built.** The app takes `--pack=DIR`, the folder holding `assets/`, or else the nearest folder from the executable's own up whose `assets/` holds a block file (through SDL's base path), so a build run from inside the repository finds its pack whatever the working directory; packaging may change the search later, and it is one function in `app/src/pack.cpp`. The pack loads before any window opens; its diagnostics print as compilers print theirs, each file named under the pack's root so it opens from any working folder; any error stops startup and warnings do not. The startup report names the pack and its states, and the layers the renderer's array holds.
- **The budget holds even unoptimized:** loading the shipped pack, textures included, takes 1.73 ms in a debug build on the reference machine against 5 ms. A release build was not measured.
