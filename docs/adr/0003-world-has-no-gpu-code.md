# ADR 0003: The world layer has no GPU code

- **Status:** Accepted
- **Date:** 2026-10-02

## Context

Block games mix two very different kinds of work: simulating a world (blocks, generation, lighting, physics, at a fixed tick rate) and drawing it (every frame, as fast as possible, on the GPU). When the two are tangled, every later step costs more: tests need a GPU, simulation can only run as fast as rendering, and splitting the game into a client and a server becomes a rewrite. Minecraft paid that price in release 1.3, when singleplayer was turned into a local server.

The split is cheap on day one and expensive later.

## Decision

The engine is built as layers, and dependencies only point downward:

```
app      window, input, main loop        (controller)
render   Vulkan: device, frames, passes  (view: reads the world, never changes it)
mesh     world data -> GPU-ready data    (pure functions, no Vulkan)
world    blocks, chunks, generation,     (model: zero GPU code,
         lighting, simulation             fixed 20 ticks per second)
core     math, memory, jobs, profiling
```

Each layer is its own CMake library and exposes only its own headers, so including a header from a layer you do not link fails to compile. Include paths fence only the engine's own `<vx/...>` headers, though. Every target still sees the compiler's system include paths, so wherever the Vulkan headers are installed in one (`/usr/include` on Ubuntu, `/usr/local/include` on macOS, which Apple Clang searches by default), a world file can still include `<vulkan/vulkan.h>` and compile. The build enforces the boundaries between our layers; a CI check keeps GPU and window headers out of `world` and `mesh`.

## Consequences

- World and meshing code is unit tested on CI without a GPU.
- A CI check rejects Vulkan and window-library includes under `engine/world` and `engine/mesh` (#28), because include paths cannot.
- A dedicated server later is `core` plus `world`, with no rendering code to strip out.
- The renderer needs a handoff from the world: mesh results are produced off the render thread and queued for upload. Later RFCs define it.
- A little indirection where a tangled design would call straight across. Worth it.
