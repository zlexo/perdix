# Roadmap

Milestones in order. Each ends with something you can see, a tagged release and a changelog entry. The designs listed are the RFCs a milestone needs; they get numbers as they are written.

| # | Milestone | You will see | Designs |
|---|---|---|---|
| 0 | Foundations | the project builds and tests on CI | project foundations |
| 1 | First Cube | a textured cube you can fly around | renderer bootstrap, camera, profiling and debug UI |
| 2 | First Chunk | one chunk of blocks | block registry, chunk storage, chunk meshing |
| 3 | Infinite | the world streams in as you fly | streaming, job system, GPU memory, culling, benchmark |
| 4 | Terrain | mountains, caves, biomes, trees | noise, biomes, carving, features |
| 5 | Hands | walk, jump, break and place blocks | collision, voxel raycast, remeshing on edit |
| 6 | Light | sunlight, torches, smooth lighting | light propagation, ambient occlusion, day and night |
| 7 | Water & Sky | water, fog, sky | translucency, fluids, atmosphere |
| 8 | Save | worlds persist | world format, compression |
| 9 | Horizon | very long view distance | level of detail, occlusion culling |

After milestone 9 come multiplayer, entities and gameplay, each after its own vision decision.

Progress lives on the repository's milestones and issues.
