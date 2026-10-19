# Perdix

A Minecraft-like voxel engine in C++20 and Vulkan, built for speed and view distance first.

Named after Perdix, who in Greek myth invented the saw and the compass: tools for cutting and measuring precisely.

It streams in an endless world of placeholder hills, which you walk through as in Minecraft or fly over with a free camera, with a debug overlay. Perdix, a second player, lives in it on his own, with a body, a skin and a name tag: milestone 3.5, Body. With Minecraft installed, the blocks are drawn with the game's own textures. The project is built in the open, one reviewed pull request at a time.

## Playing

Click the window to walk: W A S D, Space to jump, Ctrl with W or W twice to sprint, Shift to sneak, Space twice to fly, the mouse to look. Escape releases the mouse. Nothing is on the F row:

- **V** switches to a camera that flies through everything and back, leaving your body standing where it was, in your skin, with your name over it.
- **O** shows the debug overlay.
- **G** opens the god panel, which frees the mouse: what Perdix does (his own life, following you, standing still), bringing him 5 blocks in front of the camera, and the skin each of you wears. A click in the world closes it.

```
--perdix=life|follow|still   what Perdix does from the start; his own life by default
--skin=NAME[:slim]           your skin: perdix, zlex, one of the game's, or a skin file
--perdix-skin=NAME[:slim]    his
--view-distance=N            how far the world reaches, 4 to 64 chunks of 32 blocks; 32 by default
```

## Minecraft's textures

If Minecraft: Java Edition is installed, Perdix draws its blocks with the game's textures, read at startup from the newest release in the official launcher's folder: `~/Library/Application Support/minecraft` on macOS, `%APPDATA%\.minecraft` on Windows (`~/.minecraft` without `APPDATA`), and `~/.minecraft` on Linux, then the Flathub package's `~/.var/app/com.mojang.Minecraft/.minecraft`. Nothing of the game's is copied or kept: the textures are read from your own copy each time Perdix starts, and none of them are in this repository. Without the game, Perdix draws its own.

```
--minecraft=PATH          a game folder that holds versions/, or a client jar
--minecraft-version=ID    a version folder's name, such as 26.2
--no-minecraft            Perdix's own textures, whatever is installed
```

The startup report says which it drew: `Textures: Minecraft 26.2, from <the jar>, 11 of 11`. The same copy gives the game's 18 default skins, and its font for the name tags; without the game, no name tag is drawn until Perdix has a font of his own.

Perdix is not an official Minecraft product. It is not approved by or associated with Mojang or Microsoft.

## Documents

- [Vision](docs/VISION.md): the north star, the reference machine, and what is out of scope
- [Roadmap](docs/ROADMAP.md): ten milestones, each ending in something you can see
- [Decisions](docs/adr/): architecture decision records
- [Designs](docs/design/): one RFC per system, accepted before any of it is written
- [Contributing](CONTRIBUTING.md): how work moves from design to merged code
- [Debugging the renderer](docs/DEBUGGING.md): drivers, validation, debug names, the overlay and frame captures
- [The benchmark flythrough](docs/BENCHMARK.md): running it, reading its report, and sharing a result

## License

Source-available, all rights reserved: read freely, but no use without permission. See [LICENSE](LICENSE).
