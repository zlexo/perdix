# Vision

## North star

**Speed and view distance.** The engine should show more of the world, at a higher frame rate and in less memory, than anyone expects from a block game. Everything else either serves that or waits.

## The reference machine

Performance is measured on one fixed machine: an Apple M3 laptop with an 8-core GPU and 8 GB of unified memory, running macOS 26. If the engine is fast there, it is fast on most hardware people own. Every performance number in an RFC or pull request says what it was measured on.

## Principles

- **Design before code.** Each system gets an RFC that covers the whole feature before any of it is written.
- **Measure, don't guess.** Profiling and a repeatable benchmark come early, and optimizations arrive with before and after numbers.
- **Data first.** Memory layout is designed on purpose: packed formats, contiguous storage, no object per block.
- **The world knows nothing about the GPU.** Simulation and rendering are separate layers ([ADR 0003](adr/0003-world-has-no-gpu-code.md)).
- **Small, reviewed steps.** Small pull requests, small commits, and every commit builds.

## Non-goals, for now

- **Feature parity with Minecraft.** Gameplay systems (entities, crafting, inventory) wait until the engine under them is fast.
- **More than one graphics API.** Vulkan only ([ADR 0002](adr/0002-vulkan.md)).
- **Ray-traced voxels.** This is a rasterizer with a block-game look.
- **Mods and scripting.**
- **Multiplayer,** though the architecture keeps the door open.

## How we will know

Each RFC sets measurable targets for its system: frame time at a given render distance, memory per chunk, generation and meshing throughput. The headline target (a render distance held at a frame rate on the reference machine) is set once the first chunks render and there is a baseline to set it against.
