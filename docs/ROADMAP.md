# Roadmap

Milestones in order. Each ends with something you can see, a tagged release and a changelog entry. The designs listed are the RFCs a milestone needs; each gets its number when its issue is opened.

| # | Milestone | You will see | Designs |
|---|---|---|---|
| 0 | Foundations | the project builds and tests on CI | project foundations |
| 1 | First Cube | a textured cube you can fly around | renderer bootstrap (camera, profiling and debug UI included) |
| 2 | First Chunk | one chunk of blocks | block registry, chunk storage, chunk meshing |
| 2.5 | Minecraft Look | the chunk drawn with Minecraft's own textures, read from your install | Minecraft's textures from the player's install |
| 2.75 | Walk | walk, sprint, sneak, jump and fly through the chunk as in Minecraft | player movement (collision included) |
| 3 | Infinite | the world streams in as you fly | job system, world streaming (benchmark flythrough included), chunk rendering (GPU memory and culling included) |
| 3.5 | Body | Perdix, a second player who lives in the world on his own, with a body, a skin and a name tag; your own body in the free camera | another player (the player model, skins, his life, text in the world, the first panel) |
| 4 | Terrain | mountains, caves, biomes, trees | terrain generation (noise, caves, biomes and features included) |
| 5 | Hands | break and place blocks | voxel raycast (remeshing on edit is already designed: RFC 0003, 0005 and 0006) |
| 6 | Light | sunlight, torches, smooth lighting | light propagation, smooth lighting, day and night |
| 7 | Water & Sky | water, fog, sky | translucency, fluids, atmosphere |
| 8 | Save | worlds persist | world format, compression |
| 9 | Horizon | very long view distance | level of detail, occlusion culling |

Horizon's design comes right after Infinite, before Terrain, so the terrain generator is built for far, coarse terrain from the start, and its core may land before Terrain too, on the placeholder hills (decided 2026-10-09).

After milestone 9 come multiplayer, more entities and gameplay, each after its own vision decision.

Progress lives on the repository's milestones and issues.
