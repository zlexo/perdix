# RFC 0009: Minecraft's textures from the player's install

| | |
|---|---|
| **Status** | Accepted (2026-10-07) |
| **Milestone** | M2.5 Minecraft Look |
| **Issue** | #162 |

## Summary

Perdix looks like Minecraft by drawing its blocks with Minecraft's own textures, read at startup from the copy of the game the player already has. The client jar of the newest installed release is read in place, the textures the core pack names are taken out of it, tinted and layered the way the game draws them, and handed to RFC 0004's texture pipeline. Nothing of Mojang's enters the repository, a build or the disk, and without an install the core pack's own textures are drawn, as today. The archive reader and the texture map are also what two later designs build on: the converter from Minecraft's blocks to Perdix's, and the web viewer.

## Motivation

On 2026-10-07 the owner set Perdix's direction: a Minecraft clone, better, that looks like Minecraft, and later shows in-game previews in a browser. RFC 0004 kept Minecraft's art out of the repository (its open question 4), and that stands: the repository is going public, and the art is Mojang's. But every player's machine already holds it. The launcher keeps the client jar of each version it has run; release 26.2's is 39 MB and holds 1,269 block textures, 1,175 of them 16 by 16.

Reading them so that the result looks like the game takes more than copying files:

- **Some textures are gray.** `grass_block_top.png` and `oak_leaves.png` are grayscale, and the game multiplies them by a biome's color as it draws. Without that, the grass is gray.
- **Some faces are two layers.** The grass block's side is `grass_block_side.png` with `grass_block_side_overlay.png` drawn over it, tinted. The side file has a green fringe of its own, but in plains all 45 fringe texels differ from what the game shows, by up to 83 of 255 in a channel. The local reference pack made on 2026-10-05, to compare the core pack's art against, used the side file alone, so its grass side is not what the game shows.
- **The newest version listed is often not there.** On the reference machine the launcher's `versions` folder has 82 version folders and only 20 jars. The newest release there, 26.3, has its description but no jar; the newest release with a jar is 26.2.
- **The descriptions have traps too.** A Fabric profile's description says it is a `release`, and its release time is when the profile was made: on the reference machine, the Fabric profile for 1.20.1, a 2023 release, says 2026-04-29.
- **The jar's own format has traps.** Every one of its 31,709 entries leaves its CRC and sizes out of its local header (they follow the data), so the sizes have to come from the central directory. A reader that trusts local headers reads nothing.

## Goals and non-goals

Goals:

- With no options, the app finds the official launcher's game folder on macOS, Windows and Linux, and draws the core pack with Minecraft's textures.
- Which version is used is a pure function of that folder: the newest release, by release time, that has a jar. One option names a version, snapshots included; another names a game folder or a jar, which covers every other launcher.
- The jar is read in place: its directory, then only the entries needed. Nothing is extracted, cached or written.
- Every core texture shows what the game shows in plains: its texels, tinted with the game's arithmetic and the colors in the game's own color maps, its layers composed as the game draws them.
- A texture the jar cannot provide falls back to the core pack's own, with a warning naming both. No install is a line in the startup report, not an error. An install named on the command line that cannot be read is an error.
- The startup report says where the textures came from.
- An archive that is truncated, inconsistent or hostile gives a diagnostic, never a crash or a read out of bounds.
- The tests need no Minecraft: they build their own archives and game folders. A check against the real install runs locally, on request.
- The reader works on bytes in memory as well as on files, so the web viewer can hand it a jar.

Non-goals:

- **Resource packs**, the player's or anyone else's (open question 1).
- **Every other block.** The converter's RFC turns Minecraft's blockstates and models into Perdix's block files, including which blocks the game tints and with what: that lives in the game's code, not in its files.
- **Biome colors.** Tints here are the plains colors. Biomes come with M4 (RFC 0008, written before tints came up), and showing each biome's color needs the tint drawn in the shader instead of baked into the texture: a change for M4. The texture map below already says which layer is tinted.
- **Animated textures**, such as water and lava: M7 and the converter. None of the core pack's textures is animated.
- **Minecraft's per-texture mipmap hints** (open question 4).
- **How Minecraft shades faces.** The game multiplies face shade, ambient occlusion and light into the stored, gamma-encoded colors; Perdix multiplies in linear light, between its sRGB texture and its sRGB swapchain. The same 0.8 shade leaves a mid-gray north face at 0.45 in Perdix and 0.40 in the game, and the darkest occlusion corner of a top face at 0.32 against 0.20. Lighting owns that (M6), judged by eye.
- **Downloading the game** (Alternatives considered).

## Prior art

- **Chunky** (a path tracer for Minecraft worlds) looks in the official launcher's folders, the three in the table below, using `~/.minecraft` on Windows when `APPDATA` is unset. It takes the version folder with a jar whose description has the newest `releaseTime`, with no filter on `type`, so a newer snapshot wins. It opens jars and packs as zip file systems, keeps an animated texture's first frame, and draws placeholder textures when it finds nothing. We take the folders, the test for a jar and the placeholder fallback, and add the filter on releases and modded profiles.
- **Minecraft Overviewer** (a map renderer) ranks version folders by parsing their names as numbers, newest first, which skips snapshots only because their names do not parse, and falls back to older jars one file at a time. With nothing found it stops with an error. We rank by the descriptions instead: names such as `26.1-snapshot-10` and `25w36b` have no order of their own.
- **BlueMap** (a web map) renders nothing until its user sets `accept-download: true` in its settings, which accepts Mojang's EULA and confirms they own the game. It then downloads that version's client jar from Mojang's servers, checks its SHA-1 and keeps it. Its table of block tints names a color map (`@grass`, `@foliage`, `@dry_foliage`, `@water`) or a fixed color: the shape of the tints here.
- **ClassiCube**, an engine of its own rather than a Minecraft tool, asks before downloading, then fetches two old client jars from Mojang at addresses pinned to their SHA-1 and builds its default pack from them: a precedent for an engine that is not Mojang's drawing Mojang's textures, taken from the game's own files with the player's consent.
- **Amulet** (a world editor) downloads the latest client and extracts its `assets/` into a cache folder, without a prompt or a hash check. **Mineways** ships an atlas of its own and builds others with separate tools.
- **Web viewers.** deepslate's demo fetches an atlas from misode's `mcmeta` repository, an unofficial mirror of the game's assets that says it will come down if Mojang objects, and its documentation loads a pack zip the user supplies. prismarine-viewer builds atlases from an npm package that holds every version's textures. For the web viewer, a jar the user supplies is the clean path, and `Archive::from_bytes` is its way in.
- **Minecraft itself.** Which block is tinted with what lives in the client's code, not its files: a model face only carries a `tintindex`. The color maps, and each biome's temperature and downfall, are data. Some tints are fixed colors: spruce leaves `#619961`, birch leaves `#80A755`. Block textures have been in `textures/block/` under today's names (`grass_block_top`, formerly `grass_top`) since 1.13, and in the current style since 1.14.
- **The 2026-10-05 reference tool** (`tools/minecraft-reference.js` in the owner's texture tool, outside this repository) already does part of this. It reads the newest Fabric Loom jar, tints grass and leaves with the plains colors from the game's color maps, and writes Minecraft's versions of the core textures into a folder that it refuses to put inside a git work tree. We take its arithmetic, write nothing to disk, and add the overlay it missed.

## Design

### Where the game is

Three options, all optional:

```
--minecraft=PATH          a game folder that holds versions/, or a client jar
--minecraft-version=ID    a version folder's name, such as 26.2, 1.21.11 or 25w36b
--no-minecraft            the core pack's own textures, whatever is installed
```

Without `--minecraft`, the game folder is the official launcher's default:

| System | Game folder |
|---|---|
| macOS | `~/Library/Application Support/minecraft` |
| Windows | `%APPDATA%\.minecraft` |
| Linux | `~/.minecraft` |

When `APPDATA` is unset on Windows, `~/.minecraft` is tried, as Chunky does. On Linux, the community's Flathub package keeps the game in `~/.var/app/com.mojang.Minecraft/.minecraft`, which is tried second. The newer Windows launcher, installed through the Xbox app or the Microsoft Store, is reported to keep the game in the same `%APPDATA%\.minecraft`; with no Windows machine here, a player on Windows will confirm it.

A version is a folder `versions/<id>/` holding the launcher's description of it, `<id>.json`, and its jar, `<id>.jar`. The description gives its `type` (`release`, `snapshot`, and a few older or special kinds), its `releaseTime` (ISO 8601 with an offset), and, for a modded profile such as Fabric's, `inheritsFrom`, the version it builds on. The version is chosen like this:

1. `--minecraft-version=ID`: that folder, whatever its type. A folder without a jar is an error.
2. Otherwise, every folder holding both files, whose type is `release` and which has no `inheritsFrom`, newest `releaseTime` first, ties broken by id in byte order. The first whose jar opens is used; each newer one that did not open is a warning saying why.
3. No candidate: the core pack's own textures, and the report names the folder it looked in.

A folder without a jar is passed over before its description is read: on the reference machine that is 20 files of JSON instead of 82, 4.9 ms instead of 27. Modded profiles are passed over because their release times are when the profiles were made; the vanilla version each builds on is a candidate of its own. A description that cannot be read or lacks a field is a warning on its file, and that version is passed over.

`--minecraft=PATH` naming a game folder runs the same choice in it. Naming a `.jar` skips the choice: the version shown is the `id` in the jar's own `version.json`, or the file's name in jars too old to have one.

### Reading the jar

A jar is a zip archive. `vx::Archive` reads one in place:

1. **The end records.** The end of central directory record, searched for backward over a comment of up to 65,535 bytes, and the zip64 end records when the entry count, the directory's size or its offset needs them.
2. **The central directory, in one read**: 3.1 MiB for 26.2's 31,709 entries. Every entry's name is indexed with its method, flags, CRC-32, both sizes and the offset of its local header. Its sizes and CRC are the ones used: the jars leave them out of the local headers.
3. **An entry, when asked for by name.** Its local header, which must agree with the directory on the name and the method, then its data: stored, or deflated. Deflated data goes through stb_image's raw deflate decoder (`stbi_zlib_decode_noheader_buffer`), which already inflates every PNG that Perdix reads, into a buffer of exactly the size the directory gives. The result's CRC-32 must match.

Refused, each with a diagnostic naming the archive and, where there is one, the entry:

- a record or a name that runs past the end of the file or of the directory, and an entry count or directory size that disagrees with what is there;
- a local header that disagrees with the directory;
- a method other than stored or deflate, and an encrypted entry;
- an entry larger than 16 MiB once inflated (26.2's largest texture is 122 KiB, its largest entry of any kind 4.5 MiB);
- data that inflates to a size other than the directory's, and a CRC-32 that does not match;
- a name that appears twice, which zip readers resolve differently.

Nothing is ever extracted. Names are only compared with the names Perdix asks for, never turned into paths, so a hostile name has nowhere to write. Files are read through C's stdio, as `read_file` reads block files (RFC 0004's item 7, in review in #142), with 64-bit positions (`fseeko`, and `_fseeki64` on Windows), and a mutex lets several threads call `read` on one archive. Opening reads the last 64 KiB and the directory: 3.2 MiB of 39 MB.

### What each core texture is in Minecraft

Each namespace may say, in `assets/<namespace>/minecraft.toml`, what each of its textures is made of in Minecraft. The core pack's:

```toml
# assets/core/minecraft.toml: what each core texture is when the player's
# Minecraft is found (RFC 0009). Names are the game's block textures,
# assets/minecraft/textures/block/<name>.png in its jar.
stone = "stone"
cobblestone = "cobblestone"
dirt = "dirt"
sand = "sand"
grass_top = { texture = "grass_block_top", tint = "grass" }
# The side, with its tinted overlay drawn over it.
grass_side = ["grass_block_side", { texture = "grass_block_side_overlay", tint = "grass" }]
oak_log = "oak_log"
oak_log_top = "oak_log_top"
oak_planks = "oak_planks"
oak_leaves = { texture = "oak_leaves", tint = "foliage" }
glass = "glass"
```

| Value | Meaning |
|---|---|
| `"name"` | the game's block texture `assets/minecraft/textures/block/<name>.png` |
| `{ texture = "name", tint = "..." }` | the same, multiplied by a tint |
| `[layer, layer, ...]` | layers, each written as one of the two above, drawn bottom first |

A tint is either a color map in the jar, `assets/minecraft/textures/colormap/<name>.png` (26.2's jar has `grass`, `foliage` and `dry_foliage`), or a fixed color written `#rrggbb`, as the game uses for spruce and birch leaves. Texture and color map names use `a` to `z`, `0` to `9` and `_`, as Perdix's own names do, and so does every block texture in 26.2's jar.

The file is read with the block files and checked the same way, its problems printed as compilers print theirs. Errors: TOML syntax; a key in a layer table other than `texture` and `tint`, with a guess when one is near; a value of the wrong type, an empty list of layers, and a tint that is neither a name nor `#rrggbb`; a bad character in a name. Warnings: a key no block in the namespace uses, and a texture the namespace's blocks use with no key. The shipped-pack test fails on any warning, so a core texture added without its Minecraft counterpart fails it.

A namespace without the file is drawn from its own files, whatever is installed. The converter will write this file for the blocks it makes, which have no files of their own to fall back to.

### Making a texture

For each mapped texture of a namespace, when a jar is open:

1. **Decode each layer:** read its PNG from the jar and decode it with stb_image, which reads every kind the game ships (palette, gray and true color at 1, 2, 4 and 8 bits; on 26.2, 339 block textures use 4-bit palettes). It must be 16 by 16.
2. **Tint:** multiply each color channel by the tint's on the stored values, as the game's shader does: `c × t / 255`, rounded to the nearest. Alpha is left alone.
3. **Compose, bottom first:** a texel of an upper layer whose alpha passes the engine's alpha test (128 or more, RFC 0004) replaces the texel under it, alpha included; any other leaves it. The game draws an overlay as a second face with an alpha test, so for its overlays, whose texels are all either 0 or 255, this is what it shows.
4. **Hand it on:** the result is that texture's level 0, and RFC 0004's pipeline takes it from there, turning, filling and mipmapping it like any other.

If any step fails (an entry missing, not a PNG, not 16 by 16, a color map missing), that texture is drawn from its own file instead, with a warning at the name in the map:

```
assets/core/minecraft.toml:10:47: warning: Minecraft 26.2 has no block texture 'grass_block_side_overlay'; core:grass_side is drawn from its own file
```

**The plains tint.** A color map is 256 by 256. The game picks a biome's color at `x = (int)((1 - t) * 255)`, `y = (int)((1 - d * t) * 255)`, where `t` and `d` are the biome's temperature and downfall, single-precision floats clamped to 0 to 1 and then widened to double. For plains, `t = 0.8` and `d = 0.4`, which gives x = 50 and y = 173: on 26.2, `#91BD59` for grass and `#77AB2F` for foliage. The 2026-10-05 reference tool uses the same arithmetic.

### At startup

The block pack loads as RFC 0004's item 7 loads it (in review in #142 and #143), then:

1. Unless `--no-minecraft`, find the game and choose a version as above, and open the jar.
2. Read each namespace's `minecraft.toml`, make its textures from the jar, and pass them to `build_block_textures`, which uses a made texture in place of its file.
3. Close the jar. Nothing of it stays in memory but the 1,364 bytes a layer RFC 0004 already keeps.

The startup report gets one line:

```
Textures: Minecraft 26.2, from ~/Library/Application Support/minecraft/versions/26.2/26.2.jar, 11 of 11
Textures: the core pack's own; no Minecraft release with a jar in ~/Library/Application Support/minecraft (--minecraft=PATH names one)
```

### Where the code lives

A new layer, `engine/minecraft` (`vx_minecraft`), holds what Perdix knows about Minecraft's files: finding the game, the texture map, making textures, and later the converter. Like `world`, it has no GPU or window code (ADR 0003), and the CI check that keeps those includes out of `world` and `mesh` covers it too, so the web viewer can compile it to WebAssembly. It depends on `core` and `world`. `vx::Archive` goes in `core`, since nothing in it is about Minecraft. stb_image's target moves from `render`'s CMake file to `core`'s, unchanged: `core` inflates with it, `minecraft` and `render` decode with it. `render` does not depend on `minecraft`; it takes made textures as plain pixels.

### API sketch

```cpp
namespace vx {

// core: a zip archive read in place (RFC 0009). Opening reads the end records
// and the central directory; read() inflates one entry into memory. Nothing
// is extracted.
class Archive {
public:
    [[nodiscard]] static Result<Archive> open(const std::filesystem::path& path);
    [[nodiscard]] static Result<Archive> from_bytes(std::vector<std::byte> bytes);

    [[nodiscard]] std::size_t size() const noexcept; // entries
    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    // The entry, inflated and checked against its CRC-32, or why not.
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view name) const;
};

// world, beside TextureRef: a texture's level 0 made from something other
// than its file, RGBA8 with sRGB color, row by row from the top.
struct MadeTexture {
    std::string name; // "core:grass_side"
    std::array<std::uint8_t, 16 * 16 * 4> rgba;
};

} // namespace vx

namespace vx::minecraft {

struct InstallQuery {
    std::optional<std::filesystem::path> path;    // --minecraft
    std::optional<std::string> version;           // --minecraft-version
    std::filesystem::path home;                   // given, so tests need no real home
    std::optional<std::filesystem::path> appdata; // %APPDATA%, on Windows
};

struct Install {
    std::filesystem::path jar;
    std::string version;                // "26.2"
    std::vector<Diagnostic> passed_over; // newer versions that could not be used, and why
};

// The jar to read, or why there is none. The reason is an error only when the
// query named a path or a version; otherwise it is a line for the report.
[[nodiscard]] Result<Install> find_install(const InstallQuery& query);

struct Layer {
    std::string texture;                                                  // "grass_block_side_overlay"
    std::variant<std::monostate, std::string, std::array<std::uint8_t, 3>> tint; // none, a color map, a color
};

struct TextureMap {
    std::string namespace_name;                          // "core"
    std::map<std::string, std::vector<Layer>, std::less<>> textures; // "grass_side" -> its layers
};

struct TextureMapLoad {
    std::optional<TextureMap> map;       // empty when any diagnostic is an error
    std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] TextureMapLoad load_texture_map(const SourceFile& file);

// Level 0 of every mapped texture the jar can make. A texture it cannot make
// is left out with a warning, and is then drawn from its own file.
[[nodiscard]] std::vector<MadeTexture> make_textures(const Archive& jar, const TextureMap& map,
                                                     std::vector<Diagnostic>& diagnostics);

} // namespace vx::minecraft

namespace vx::render {

// RFC 0004's, with made textures: a texture found among them is not read
// from its file.
[[nodiscard]] std::vector<std::byte> build_block_textures(std::span<const TextureRef> textures,
                                                          const std::filesystem::path& assets,
                                                          std::span<const MadeTexture> made,
                                                          std::vector<Diagnostic>& diagnostics);

} // namespace vx::render
```

`find_install` reads the environment only through its query, so tests build game folders in a temporary folder and never see the machine's own. The app fills the query from its options and the environment.

### Dependencies

| Dependency | Version | Used by | Why |
|---|---|---|---|
| yyjson | 0.13.0 | minecraft | JSON for the launcher's version files and the jar's `version.json`, and later the converter's blockstates and models. MIT license, C, one source file compiled once; errors come back as values with the byte position, so engine code needs no exceptions. Pinned to its release tag. |
| stb_image | 2.30 | core, minecraft, render | already a dependency (RFC 0004); its target moves to `core`, unchanged |

## Alternatives considered

- **Shipping Minecraft's textures**, in the repository or a release: ruled out by RFC 0004's open question 4, and by Mojang's EULA, which forbids distributing anything Mojang made without its permission.
- **Extracting them once into a cache folder**, as the reference tool does: about 10 ms faster at startup, but it leaves Mojang's files on disk, where they can be committed by accident (the reference tool refuses to write inside a git work tree for that reason), and the cache goes stale when the game updates. Reading the jar costs about 11 ms a start.
- **Downloading the client jar from Mojang**, as BlueMap does once its user accepts the EULA, and ClassiCube after asking: it works without an install, but needs the network, a consent prompt and a 39 MB file on disk, for players who own the game and already have it.
- **Checking the whole jar** against the SHA-1 its description gives (`downloads.client.sha1`): it would catch a modified jar, but it reads all 39 MB at every start instead of 3.2 MiB. Each entry read is checked against its CRC-32, which catches a damaged one.
- **Original art drawn to look like Minecraft:** the owner chose the game's own textures on 2026-10-07. The core pack's art stays, as the fallback.
- **The mapping in each block file**, instead of one file per namespace: textures are shared (dirt is also the grass block's bottom), so a texture's counterpart belongs to the texture.
- **Tinting in the shader** instead of baking: the tint would need bits in the quad or the layer, and a two-layer face such as the grass side would need two quads. Baking needs neither, and is exact while there is one biome. M4 will need tints in the shader; the map already records which layer is tinted.
- **A zip library:** miniz 3.1.2 (MIT, one file, zip reading and an inflate of its own), libzip (needs zlib), minizip-ng (a fork of zlib's minizip). Each is a second inflate next to stb_image's, for a reader of a few hundred lines that its tests and a fuzzer cover. If fuzzing keeps finding problems in ours, miniz is the fallback.
- **Other JSON libraries:** simdjson 5.0.2 (Apache 2.0, the fastest, much larger), nlohmann/json 3.12.0 (MIT, reports errors by exception unless told otherwise, slow to compile), RapidJSON (MIT, last release 2016), or a parser of our own, which would be one more format to harden.
- **The version of the launcher profile played last**, from `launcher_profiles.json`: it follows what the player plays, but profiles name aliases such as `latest-release`, modded versions and custom folders (open question 3).

## Performance budget

Targets on the reference machine, release build, files in the system's cache. A throwaway program measured the approach on 2026-10-07 against the reference machine's own install (Apple Clang at `-O2`, stb_image 2.30's inflate and PNG decoder, medians of 15 runs):

| What | Measured | Target |
|---|---|---|
| Find the game folder and choose a version (82 folders, 20 with a jar) | 4.9 ms | under 10 ms |
| Open the jar: end records and central directory, 31,709 names | 5.0 ms | under 10 ms |
| The core pack's textures: 12 entries and 2 color maps read, inflated, checked and decoded | 0.96 ms | under 3 ms, with tints and layers |
| Every block texture in the jar, 1,269, read, inflated, checked and decoded on one thread | 23.4 ms | under 50 ms |
| Memory held after startup | | none beyond the texture array |

The first run after the files had left the cache took about 13, 14 and 5 ms for the first three rows. Cold numbers are reported in the pull requests, not budgeted.

## Test plan

- **Archives:** fixtures made by a script kept beside them (Python's `zipfile`, our own bytes): stored and deflated entries, a comment, entries with data descriptors as the game's jars have them, and zip64 records; then one bad archive for each refusal above, with its expected message. A synthetic archive the size of the game's jar reads back every entry exactly.
- **Fuzzing, in review:** `Archive::from_bytes` and `read` under libFuzzer (Homebrew Clang) with UBSan, seeded with the fixtures; every input that failed joins the bad archives.
- **Finding the game:** game folders built in a temporary folder. Releases with and without jars; snapshots; a Fabric profile with `inheritsFrom` and its own copy of the jar; descriptions that cannot be read or lack a field; two releases with the same time; a newest jar that does not open, falling to the next with a warning; `--minecraft-version` naming a snapshot, and naming a folder without a jar; `--minecraft` naming a jar, a game folder and a path that does not exist. Each system's default folder from a given home and `%APPDATA%`.
- **The texture map:** one bad file for each error and warning, as in RFC 0004's corpus.
- **Tints:** a synthetic color map with a different color at every pixel, so plains must read exactly (50, 173); the rounding of `c × t / 255` at known values.
- **Layers:** an upper texel at alpha 255 and 128 replaces, at 127 and 0 keeps; layers apply bottom first.
- **End to end, without Minecraft:** a synthetic client jar built by the test, our own pixels in the game's layout with a `version.json`. The core pack loads with it, and every face's level 0 equals the bytes worked out by hand; a jar missing one texture falls back with the expected warning.
- **The shipped map:** `assets/core/minecraft.toml` maps every core texture and loads with no warnings.
- **The real install, locally, on request** (`VX_TEST_MINECRAFT=1`): 11 of 11 textures from the jar; grass and foliage equal the color maps' pixel at (50, 173); the grass side equals the side file with the tinted overlay over it. With the variable set and no install, the test fails instead of skipping.
- **Nothing of Mojang's in the repository:** CI fails if a tracked path contains `assets/minecraft/` or ends in `.jar`.
- **By eye, on the reference machine and both drivers:** the M2 chunk next to the game itself at the same view; the grass side's fringe; leaves and glass far away.

## Pull request plan

1. This RFC.
2. `vx::Archive` in `core`: end records, the central directory and stored entries; stb_image's target moves to `core`; the fixture script and the bad archives.
3. Deflated entries and CRC-32 checks, data descriptors and zip64, and the fuzzing round.
4. The `minecraft` layer and yyjson: `find_install`, each system's default folder and the version choice, with the game-folder tests.
5. The texture map: reading and checking it, its bad files, and `assets/core/minecraft.toml`.
6. Making textures: decoding, tints and layers, the fallback, and `build_block_textures` taking made textures, with the synthetic-jar tests.
7. The app: the three options and the report line, the local test against the real install, the CI check for Mojang's files, and a README section on using the game's textures, with the statement the Usage Guidelines ask for (not an official Minecraft product, not approved by or associated with Mojang or Microsoft).

## Open questions

All settled on 2026-10-07: every recommendation below was accepted.

1. **Resource packs.** The game layers the player's enabled packs (`options.txt`) over the jar. Many are 16 by 16, but popular ones are 32 to 512 texels, which RFC 0004's 16 by 16 array cannot hold, and they come from the internet: untrusted archives and PNGs, where stb_image's warning that its security fixes can take time starts to matter. Recommendation: **later, in an RFC of their own**, which also decides texture sizes and a hardened decoder.
2. **Which launchers are searched.** Prism Launcher and MultiMC keep the vanilla jar at `libraries/com/mojang/minecraft/<version>/minecraft-<version>-client.jar` under their data folder; the Modrinth App lays out `meta/versions/<id>/` as the official launcher lays out `versions/`; where CurseForge keeps jars was not confirmed. The reference machine has Prism and the Modrinth App as well as the official launcher. Searching them all means a data folder per launcher on each system, three layouts, and a rule for choosing between installs. With `--minecraft`, Prism's jar can be named directly, and the Modrinth App's `meta` folder works as a game folder. Recommendation: **the official launcher's folder by default**; every other launcher through `--minecraft`, which takes a game folder or a jar.
3. **Which version.** The newest installed release, the version of the launcher profile played last, or one pinned by Perdix. Recommendation: **the newest installed release**: predictable, and the latest art. The converter may pin a version for block data; textures need none.
4. **Minecraft's mipmap hints, a call by eye.** Since release 1.21.11, 45 block textures carry a `mipmap_strategy` in their `.mcmeta` file (`mean` for glass, `dark_cutout` for oak leaves, `strict_cutout` for 27 others), and five an `alpha_cutoff_bias`. RFC 0004's pipeline keeps every cutout texture's coverage at every level, which keeps leaves dense far away; with `mean`, the game lets glass fade. Recommendation: **RFC 0004's pipeline for every texture**, with leaves and glass judged far away once the chunk shows them.
5. **Screenshots and clips in public, the owner's call.** Mojang's Usage Guidelines count textures as part of Minecraft's brand, count any sharing, even free, as use, and ask anything that uses the name to say clearly that it is not an official Minecraft product and not approved by or associated with Mojang or Microsoft. They do not say whether screenshots of another engine drawing the game's textures are allowed. Recommendation: **anything published (the devlog, the site, the README, issues and pull requests) shows the core pack's own art**, taken with `--no-minecraft`; Minecraft's textures stay on the owner's screen.

## As built

Pull request plan item 2, the archive's directory and stored entries, landed as four pull requests: `crc32`; the end record and the central directory, from memory; archives in files; reading stored entries.

- **`crc32` is in `core` on its own** (`vx/core/crc32.hpp`): a table of 256 remainders made at compile time, a byte at a time. PNG files store the same CRC.
- **`from_bytes` takes a name** as well as the bytes, for its messages: `from_bytes("26.2.jar", bytes)`.
- **A problem with the archive refuses it; a problem with one entry refuses only that entry.** Opening fails on what makes the whole directory untrustworthy: no end record, split disks, zip64 records (item 3), a directory that runs past the end record or holds more or fewer entries than it says, a damaged header. Reading fails, for that entry alone, on encryption, a method other than stored, a size over 16 MiB, a stored entry whose two sizes differ, a local header that is misplaced, damaged or disagrees, data that runs into the directory, bytes that cannot be read, a CRC that does not match, and a name listed twice, so one odd entry in a jar does not hide the textures.
- **The end record is the last one whose comment reaches the end of the archive**, as Python's `zipfile` and `unzip` take it: a comment that is itself a whole end record reaching the end is taken for the record, and the archive reads as empty, in all three. Extra fields and entry comments are stepped over, and an entry's data is found past its local extra field, which may differ from the central one, as Info-ZIP's do.
- **A file's size comes from the open file**, not from its path, which could name another file by then, and anything but a regular file, such as a folder, is refused. On Windows, messages name the path in UTF-8 (`path::string()` throws on characters the code page lacks, and engine code cannot catch it) and the file opens with `_wfopen_s`, since the C runtime marks `_wfopen` deprecated.
- **Every entry's name is indexed, sorted**, pointing into the directory kept in memory, and found by binary search: no string per entry.
- **Reading a file takes a lock around each seek and read.** A test reads one archive from eight threads at once; without the lock, one thread's seek lands between another's seek and read, and entries come back wrong.
- **The tests build their archives byte by byte** (`tests/core/zip_bytes.hpp`), each refusal by damaging one field, instead of keeping a fixture per case. One fixture comes from a zip writer apart from ours: `stored.zip`, written by Python's `zipfile` through `make_stored.py` beside it, into a stream it cannot seek, so its entries carry data descriptors as Minecraft's jars do. The archive the size of the game's jar is 31,709 entries built by the test, each read back.
- **stb_image's target moves to `core` with item 3**, which first inflates; nothing here needs it.
- **Checked beyond the tests:** the core tests run clean under UndefinedBehaviorSanitizer and under Guard Malloc, which crashes on a read even one byte past an allocation; a planted one-byte overread in the end record search passes the plain run and crashes under Guard Malloc. Fuzzing, with Homebrew Clang 20's libFuzzer, UndefinedBehaviorSanitizer and libc++'s debug bounds checks, seeded with the tests' archives: 7.1 million inputs in three minutes, nothing found, at most 31 MB of memory; the same planted overread was found in the first second.
- **A read-only review** found no blocker. Its two findings to fix (Windows names and opening, and no test with extra fields or entry comments) and its eight smaller ones were fixed in the pull request that brought the code, each new test shown to fail without its fix.
