# RFC 0000: Project foundations

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M0 Foundations |
| **Issue** | #3 |

## Summary

The build, dependency, toolchain, test, formatting and CI setup, plus the source layout, that every later pull request builds on. Nothing here renders; it makes everything after it cheaper and stricter.

## Motivation

Foundations that are not decided up front get decided by accident, once per pull request. Warning levels drift, formatting drifts, and layer boundaries erode one convenient include at a time. All of it is cheapest to fix before the first line of engine code.

The development machine already shows the kind of drift this has to stop: the first `clang++` on its `PATH` is an old Homebrew Clang 14, while the system compiler is Apple Clang 17. A build that trusts `PATH` silently picks the wrong one.

## Goals and non-goals

Goals:

- One command each to configure, build and test, the same on macOS, Linux and Windows: `cmake --preset <p>`, `cmake --build --preset <p>`, `ctest --preset <p>`.
- Dependencies pinned to exact versions and fetched by the build itself. Beyond the toolchain (a compiler, CMake and Ninja), the only manual installs are the Vulkan SDK (RFC 0001) and the pinned `clang-format` (see Formatting).
- Warnings are errors in our code, and never applied to third-party code.
- The layer boundaries of [ADR 0003](../adr/0003-world-has-no-gpu-code.md) are enforced by the compiler, and its ban on GPU and window headers in `world` and `mesh` by a CI check.
- CI runs on every push and pull request, and formatting is checked by a pinned `clang-format`.
- Every check is shown to fail on a known-bad input before it is trusted.

Non-goals:

- Packaging, installers and release automation.
- Anything Vulkan: RFC 0001.
- Benchmarks: RFC 0002, whose pull request plan adds the nanobench harness in `bench/` (M2).

## Prior art

- **cpp-best-practices/cmake_template** (Jason Turner): per-target warning sets, sanitizer options, presets. We take the per-target warnings and the preset shape, and leave out the generated-project machinery.
- **Professional CMake** (Craig Scott): targets own their usage requirements; `FetchContent` over hand-rolled downloads.
- **O3DE**: a short namespace (`AZ`) that does not change with the product's branding.

## Design

### Toolchain and presets

`CMakePresets.json` defines every supported configuration. Presets pin the compiler explicitly, Apple Clang by path and the Linux compilers by version, so ADR 0001's rule holds on every host: the compiler is never whatever happens to be first on `PATH`.

| Preset | Host | Compiler | Build type |
|---|---|---|---|
| `mac-debug` | macOS | Apple Clang (`/usr/bin/clang++`) | Debug |
| `mac-release` | macOS | Apple Clang | Release |
| `linux-gcc` | Linux | GCC 13 (`g++-13`) | Debug |
| `linux-clang` | Linux | Clang 18 (`clang++-18`) | Debug |

GCC 13 and Clang 18 are Ubuntu 24.04's default versions, and GitHub's `ubuntu-24.04` runner has both. All presets use Ninja and build into `build/<preset>`. CMake 3.28 or newer. A Windows (MSVC) preset arrives with the Windows CI job.

### Dependencies

Dependencies come from CMake's built-in `FetchContent`, pinned to a release tag (or, for a library that publishes no releases, such as stb, to a commit), shallow-cloned, and declared `SYSTEM` so their headers never trigger our warnings. Each dependency is added by the pull request that first needs it, in the CMake file of the layer that uses it.

| Dependency | Version | Used by | Since |
|---|---|---|---|
| doctest | 2.5.3 | tests | this RFC |

### Source layout

```
CMakeLists.txt          project, options, layers
CMakePresets.json       every configuration, sanitizer builds included (#18)
cmake/                  helper modules (warnings, floating point)
engine/
  core/
    include/vx/core/    public headers
    src/                implementation and private headers
  world/  mesh/  render/    added by the RFCs that need them
app/                    the executable: window, input, main loop
tests/                  one doctest executable per layer
bench/                  nanobench benchmarks, added by RFC 0002
assets/                 data files (blocks, textures, terrain), added by RFC 0004 and RFC 0008
docs/                   vision, roadmap, ADRs, RFCs
```

Each layer is a static library, `vx_<layer>`, with an alias `vx::<layer>`. A layer's include directory is `engine/<layer>/include`, so its headers are included as `<vx/<layer>/name.hpp>` and are only visible to targets that link it. Including a header from a layer you do not depend on fails to compile, which is how ADR 0003's boundaries between layers are enforced. Include paths cannot hide system headers such as Vulkan's, so a CI check keeps GPU and window includes out of `engine/world` and `engine/mesh` (#28).

### Namespace

All engine code lives in namespace `vx` (for voxel), with nested namespaces only where a layer needs them. The namespace is short and does not change when the product is named or renamed.

### Warnings

Applied per target through `vx_target_warnings(<target>)`, only to our own targets:

- GCC and Clang: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align -Wnull-dereference -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough`
- MSVC: `/W4 /permissive-`

Warnings are errors (`-Werror`, `/WX`) through the option `VX_WARNINGS_AS_ERRORS`, on by default.

### Floating point

Every engine target compiles with `-ffp-contract=off` (MSVC: `/fp:precise`, which stopped contracting by default in Visual Studio 2022), applied per target by the same CMake helper module as the warnings. Nothing in the build, dependencies included, uses `-ffast-math` or `/fp:fast`.

The reason is terrain generation, which must give the same bits on arm64 and x86-64 (RFC 0008). Clang, Apple Clang included, fuses `a * b + c` into one multiply-add by default wherever the CPU has one. Every arm64 CPU does and baseline x86-64 does not, so the same code rounds differently on the two, which RFC 0008 measured on the reference machine. With contraction off, both round every operation alike. RFC 0008's golden hashes check the result on every compiler and architecture; its control, an arm64 build with `-ffp-contract=fast` that must fail them, proves they can.

### Tests

[doctest](https://github.com/doctest/doctest): a single header, the fastest-compiling of the mainstream C++ test frameworks. One test executable per layer (`vx_core_tests`, later `vx_world_tests` and so on), with every test case registered in CTest by `doctest_discover_tests`, so `ctest` can run, filter and report each case on its own.

### Formatting

`clang-format` with the repository's `.clang-format`: LLVM style, 4-space indent, 100 columns, pointers bound to the type (`int* p`), includes grouped as local, engine, third-party, standard library. The version is pinned to 22.1.8, because different `clang-format` versions format the same file differently. CI installs it from PyPI; locally, `pipx install clang-format==22.1.8` installs the same release. Check that `clang-format --version` reports 22.1.8: on the development machine the first `clang-format` on `PATH` is Homebrew's 14, which rejects the configuration (its `MainIncludeChar` key needs clang-format 19 or newer).

### Version

The version is set once, in `project(VERSION ...)`. CMake generates a private header from it, and `vx::version()` and `vx::version_string()` expose it.

### CI

GitHub Actions on every push to `main` and every pull request:

| Job | Runs |
|---|---|
| Linux (GCC) | configure, build, test |
| Linux (Clang) | configure, build, test |
| Format | the pinned `clang-format` in check mode over every source file |
| macOS | configure, build, test with Apple Clang; only while the repository is public |

While the repository is private, macOS minutes cost about ten times the Linux price and Windows about 1.7 times, so those jobs wait for the repository to go public, where Actions minutes are free.

## Alternatives considered

- **vcpkg** (manifest mode): binary caching and a huge catalog, at the cost of a second tool to bootstrap on every machine and in CI. Worth revisiting if dependency builds get slow.
- **CPM.cmake**: a thin layer over `FetchContent` with a shared source cache. Nicer syntax, one more file to vendor; `FetchContent` alone is enough for the handful of dependencies planned.
- **Git submodules**: manual updates and noisy history.
- **System packages**: not reproducible across machines.
- **Catch2** v3: excellent and more actively developed, but slower to compile. **GoogleTest**: heavier, and its mocking is not needed.
- **Meson, Bazel, premake, xmake**: all workable; CMake wins on IDE support and on being what the Vulkan ecosystem ships.

## Performance budget

Measured on the reference machine, not asserted in CI:

- Clean configure and build of the skeleton: under 30 seconds, not counting the first dependency download.
- No-op rebuild: under 1 second.
- CI feedback on a pull request: under 5 minutes.

## Test plan

- CI green on GCC and Clang; local build and tests green on Apple Clang with `mac-debug`.
- Controls, run once and recorded in the build skeleton's pull request: a planted warning fails the build, a planted formatting error fails the format check, and a failing test fails `ctest`.
- When the second layer lands, a control include across a forbidden boundary fails to compile.

## Pull request plan

1. This RFC.
2. Build skeleton: CMake, presets, `engine/core`, `app`, doctest wired into CTest, the Linux and format CI jobs (#4).
3. Sanitizer preset and CI job (#5).
4. clang-tidy on CI, version pinned like clang-format (#6).
5. Windows CI job and preset, once the repository is public (#7).

## Open questions

1. **Exceptions and RTTI.** Recommendation: off in engine code; errors are values (return codes, `std::optional`, or a small `Result` type, until a later ADR moves the standard to C++23 and `std::expected` can replace it). The skeleton keeps the compiler defaults until this is settled.
2. **Naming.** Recommendation: types in `PascalCase`; functions, variables, namespaces and files in `snake_case`; private members with a trailing underscore (`size_`); macros in `VX_SCREAMING_CASE`. The skeleton follows this.
3. **Namespace `vx`, or the product's name once it has one.** Recommendation: `vx`, so naming the product never touches code.
4. **doctest or Catch2.** Recommendation: doctest, for compile speed.

## As built

Not built yet.
