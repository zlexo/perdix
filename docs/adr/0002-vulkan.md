# ADR 0002: Vulkan as the only graphics API

- **Status:** Accepted
- **Date:** 2026-10-02

## Context

The north star is speed and view distance. The rendering techniques that get there need a modern explicit API: compute shaders, indirect and multi-draw indirect drawing, large descriptor arrays, direct buffer addresses, and explicit control over memory and synchronization.

The engine is developed on macOS and must also run on Windows and Linux.

- **OpenGL** on macOS stops at version 4.1 and is deprecated: no compute shaders, no multi-draw indirect.
- **Metal** is excellent and Apple-only.
- **WebGPU** is portable and runs in browsers, but its standard form lacks multi-draw indirect and bindless resources, which caps how far rendering can be optimized.
- **Vulkan** runs natively on Windows and Linux. On macOS it runs on top of Metal: KosmicKrisp (LunarG) is a Vulkan 1.4 conformant driver for Apple Silicon on macOS 26. It has shipped in the Vulkan SDK since 1.4.335.0 (December 2025, as an alpha), and its conformant version since 1.4.363.0 (September 2026). MoltenVK covers older systems.

## Decision

**Vulkan is the only graphics API.**

- Target **Vulkan 1.3** and a short, explicit list of features: dynamic rendering, synchronization2, timeline semaphores, buffer device address, descriptor indexing and multi-draw indirect. RFC 0001 owns the exact list and checks it at device selection.
- **No mesh shaders or ray tracing** in the core rendering path. They are not portable to the Apple drivers yet.
- **No multi-API abstraction layer.** One renderer module, behind a narrow interface, talks to Vulkan directly.

## Consequences

- More setup before the first pixel than OpenGL would need. Helper libraries (RFC 0001) keep the boilerplate small.
- On macOS every frame goes through a translation layer onto Metal. MoltenVK stays installed next to KosmicKrisp, so a driver bug can be told apart from our own by switching drivers.
- RenderDoc does not run on macOS, so GPU frame captures on the development machine go through Xcode's Metal debugger, using MoltenVK's capture settings.
- The development machine is the only GPU hardware available. Linux CI renders with lavapipe (Mesa's software Vulkan driver) so at least one non-Apple driver runs every change, and benchmark results from other people's hardware cover the rest.
