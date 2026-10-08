# Perdix

A Minecraft-like voxel engine in C++20 and Vulkan, built for speed and view distance first.

Named after Perdix, who in Greek myth invented the saw and the compass: tools for cutting and measuring precisely.

It renders one chunk of blocks you can fly around, meshed from the core pack's blocks, with a debug overlay. With Minecraft installed, the blocks are drawn with the game's own textures: milestone 2.5, Minecraft Look. The project is built in the open, one reviewed pull request at a time.

## Minecraft's textures

If Minecraft: Java Edition is installed, Perdix draws its blocks with the game's textures, read at startup from the newest release in the official launcher's folder: `~/Library/Application Support/minecraft` on macOS, `%APPDATA%\.minecraft` on Windows (`~/.minecraft` without `APPDATA`), and `~/.minecraft` on Linux, then the Flathub package's `~/.var/app/com.mojang.Minecraft/.minecraft`. Nothing of the game's is copied or kept: the textures are read from your own copy each time Perdix starts, and none of them are in this repository. Without the game, Perdix draws its own.

```
--minecraft=PATH          a game folder that holds versions/, or a client jar
--minecraft-version=ID    a version folder's name, such as 26.2
--no-minecraft            Perdix's own textures, whatever is installed
```

The startup report says which it drew: `Textures: Minecraft 26.2, from <the jar>, 11 of 11`.

Perdix is not an official Minecraft product. It is not approved by or associated with Mojang or Microsoft.

## Documents

- [Vision](docs/VISION.md): the north star, the reference machine, and what is out of scope
- [Roadmap](docs/ROADMAP.md): ten milestones, each ending in something you can see
- [Decisions](docs/adr/): architecture decision records
- [Designs](docs/design/): one RFC per system, accepted before any of it is written
- [Contributing](CONTRIBUTING.md): how work moves from design to merged code
- [Debugging the renderer](docs/DEBUGGING.md): drivers, validation, debug names, the overlay and frame captures

## License

Source-available, all rights reserved: read freely, but no use without permission. See [LICENSE](LICENSE).
