# The benchmark flythrough

`perdix --flythrough` flies a fixed path over a fixed world and prints one Markdown block: the numbers RFC 0006 asks every performance pull request to quote. The path, the world and the settings are the same on every machine, and within each phase the camera's place is a function of time, so every machine faces the same streaming demand per second.

## Running it

Build the release preset and run the flythrough from the source folder, plugged in, with nothing else running:

```sh
cmake --preset mac-release && cmake --build --preset mac-release
./build/mac-release/app/perdix --flythrough
```

It opens a window drawing 2560 by 1440 (1280 by 720 points on a display of density 2), presents immediately when the driver offers it and FIFO otherwise, and takes about two minutes. Closing the window stops it with no report. A performance pull request pastes the report from before and after its change: three runs on the reference machine, an Apple M3, and the medians.

The path, at RFC 0006's view distance of 32 chunks over the placeholder hills (seed `0x5EED`):

| Phase | Camera | Shows |
|---|---|---|
| `load` | Still at (0, 160, 0) until the region is complete, at most 30 s; it asks whenever no request is out and no upload waits | cold throughput, time to a full view |
| `hover` | Still, 10 s | the baseline frame time |
| `cruise` | 30 s east at 64 blocks a second | steady streaming |
| `turns` | 15 s of S-curves at 64, the view swinging 90 degrees each way | in-view priority |
| `sprint` | 15 s east at 512 | throughput under pressure |
| `climb` | 15 s up to y = 2,000, looking 45 degrees down | the ground stays loaded |
| `teleport` | A million blocks east, at the load's height, until complete, at most 30 s | unloading |

A change to the path or its settings bumps the flythrough's version, the number in the report's title; results compare only within one version.

## Reading the report

The block opens with the engine's version and the commit it was built from (`-dirty` with uncommitted changes), the build, the GPU and driver, and the settings. Then one row a phase:

- **Time:** how long the phase ran; `(cap)` when a phase waiting for the region stopped at its cap instead.
- **Frame p50 / p99 / max:** frame times, from one frame's start to the next. Every 30th frame or so asks the world thread for a full sample, and the frame that takes in its answer is left out: it counts the main thread's holes and reads the arena and the process's memory.
- **Longest step / pass:** the longest the world thread took to take in results at once, RFC 0006's target being under 1 ms, and the longest pass of its loop: the main thread's messages, the results, the meshes sent on and the jobs fed, less the time spent answering samples.
- **Generated / uniform / meshed a second:** chunks built by a job, chunks found uniform without one, and meshes made by a job, between the phase's first and last samples. `-` when the phase had one sample.
- **Holes p99 within 256 / all:** chunks of the drawn region in view with nothing on screen and not known empty, at the 99th percentile of the phase's samples: those within 256 blocks of the eye, and all of them. The world thread counts the chunks with no mesh sent, and the main thread those whose mesh waits for its upload. RFC 0006 wants none near the camera during `sprint`.
- **Chunk data, GPU meshes, footprint:** at the phase's last sample and the most of any: the chunks' blocks and their bookkeeping, the arena's quads, and the process's memory (`phys_footprint` on macOS, the resident set on Linux).

## The quick run

`perdix --flythrough=quick` takes the same path for a few seconds, at view distance 16 in a 640 by 360 window, and asks for a sample whenever none is out, each checking RFC 0006's invariants: every record's neighbor counts and state match its neighbors, the state counts add up, the mesh half holds exactly the chunks the world half shows (complete, so with their 26 neighbors ready), and no record is left outside the region grown by the hysteresis. The run fails (exit code 4) on a broken invariant, on a wait for the region that runs into its cap, or on chunk data after the teleport more than 5% off its level after the load; and with validation on, as in debug builds, on a Vulkan validation error (3). The waits' cap, 300 s, is far beyond what a software driver needs: it ends a hang and never fails a slow machine. CI runs it on lavapipe under Xvfb; it never asserts a frame time or a speed.

## The ten-minute run

`perdix --flythrough=long` takes the same path with cruise, turns and sprint stretched to 300, 120 and 150 seconds: about ten minutes, so the sprint at its end runs on a machine that has worked hard for minutes. The reference machine, a 13-inch MacBook Air, has no fan and slows under sustained load; comparing the long run's sprint with the full run's shows by how much (RFC 0005).

## The worker sweep

`scripts/worker-sweep.sh` runs the full flythrough with 1 to 8 workers at each of the three QoS modes RFC 0005 compares: `switching` (utility, and user-initiated while streaming catches up, the default), `utility` and `user-initiated`, three runs of each, about two hours in all. It keeps every report in `build/sweep/<date>/` and prints one table of the medians: the load's time and throughput, the frame times of `hover`, `cruise` and `sprint`, the sprint's near holes and the world thread's longest pass. Run it plugged in with nothing else open: the flythrough's window takes the screen. `scripts/worker-sweep.sh 1 4 6` runs one round of 4 and 6 workers; `QOS=utility` runs one mode; `SUMMARIZE=build/sweep/<date>` prints a sweep's table again without running anything.

The app takes the same settings: `--workers=N` (1 to 64; the cores less two by default) and `--worker-qos=switching|utility|user-initiated`. The report's settings line names both.

## Sharing a result

The "Benchmark result" issue template takes the report and a few lines about the machine.
