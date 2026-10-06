# Debugging the renderer

What to reach for when a frame looks wrong, a run fails or a number looks off, on the reference machine (an Apple M3, macOS) and on Linux. Each tool here is set up in RFC 0001; its As built section records what was found with them.

## Before anything: the environment

On macOS the drivers and the validation layers come from the Vulkan SDK, and its environment must be set in the shell that runs the engine or its tests:

```sh
source ~/VulkanSDK/1.4.363.0/setup-env.sh
```

Without it the window fails at startup with `SDL could not create the window: Installed Vulkan Portability library doesn't implement the VK_KHR_surface extension`, and the Vulkan tests with `validation requested but VK_LAYER_KHRONOS_validation was not found`. macOS strips `DYLD_LIBRARY_PATH` from programs started through protected binaries such as `/bin/sh` and `/usr/bin/env`, so run the engine directly, not through them; the engine falls back to the SDK seen at configure time to find the loader, but not the drivers.

## Which driver

The startup report says which GPU and driver run, and the overlay shows the same. On the reference machine that is KosmicKrisp, a conformant driver, and MoltenVK only when no conformant driver passes the feature floor. `VK_DRIVER_FILES` makes the loader see only the drivers it names:

```sh
# MoltenVK, through the portability fallback
VK_DRIVER_FILES="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json" ./build/mac-debug/app/perdix
# KosmicKrisp alone
VK_DRIVER_FILES="$VULKAN_SDK/share/vulkan/icd.d/libkosmickrisp_icd.json" ./build/mac-debug/app/perdix
```

A bug that shows on one driver and not the other is likely the driver's; one that shows on both, and on lavapipe in CI, is likely ours. `vulkaninfo --summary` lists what the loader sees.

## Validation

Debug builds run `VK_LAYER_KHRONOS_validation` with synchronization validation. Each message goes to standard error with its ID and the objects it is about, by their debug names:

```text
[vulkan error] VUID-vkCmdFillBuffer-dstOffset-00025 (command buffer 0xb54a1c000, buffer "named probe buffer"): vkCmdFillBuffer(): dstOffset (2) is not a multiple of 4.
```

A debug run that counted a validation error exits with 3, so `perdix --frames=10` is a validation check, and every Vulkan test fails on any message, warnings included. The overlay shows the count, red once there is an error.

A clean run is not proof. The layers did not see these, which tests and pixels had to catch:

- an image read from a wrong slot of a partially bound array;
- a missing barrier between two render passes that write one attachment;
- dynamic indexing of sampled images without the feature, until the shader declared the capability.

## Debug names

Every Vulkan object Perdix makes is named when it is made, in every build where the loader offers `VK_EXT_debug_utils`: buffers and images by what they hold (`cube vertices`, `cube checker`, `depth target`, `frame slot 1 cube origins`), the cube's pipeline `cube`, the chunk's `chunk, opaque` and `chunk, cutout`, its `chunk quads` and the shared `chunk indices`, the frame slots' command buffers and semaphores (`frame slot 0`, `frame slot 0 image acquired`, `frames done`), the swapchain's images (`swapchain image 2`), the `texture table` and the `GPU frame timer`. ImGui's Vulkan backend makes objects of its own that stay unnamed, as do VMA's memory blocks.

To check that nothing new went unnamed, record a run with the SDK's API dump layer, which logs every call, and compare the handles the create calls return with those `vkSetDebugUtilsObjectNameEXT` names:

```sh
VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_api_dump VK_APIDUMP_OUTPUT_FORMAT=json VK_APIDUMP_DETAILED=true \
VK_APIDUMP_LOG_FILENAME=run.json ./build/mac-debug/app/perdix --frames=3 --overlay
```

## Frame times: the overlay and Tracy

F3 shows the overlay, and `--overlay` shows it from the first frame: graphs of the CPU's and the GPU's time a frame against the 0.5 ms budget, the frame's interval, the present mode switch, the camera's position and the validation count. Its GPU time comes from the renderer's own timestamps, two frames late; on KosmicKrisp about one frame in 200 drawn to a window has no time, its timestamps coming in after the frame is done, and the graph skips it. Judge the CPU graph in `mac-profile` or `mac-release`: debug builds pay for validation.

For where the time goes, step by step, Tracy: see Profiling in the [contributing guide](../CONTRIBUTING.md). KosmicKrisp drops timestamps written inside a render pass, so GPU zones span whole passes, and it does not time a transfer before the first pass of a span.

## Frame captures

RenderDoc does not run on macOS. Xcode's Metal frame capture does, through MoltenVK, which can write a capture of the first frame to a file:

```sh
VK_DRIVER_FILES="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json" \
MTL_CAPTURE_ENABLED=1 \
MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE=2 \
MVK_CONFIG_AUTO_GPU_CAPTURE_OUTPUT_FILE=frame.gputrace \
./build/mac-debug/app/perdix --frames=10
open frame.gputrace
```

Scope 2 is the first frame; 1 is everything the device does. One frame at 2560 by 1440 takes about 145 MB. Writing the capture needs no Xcode, but opening it does: the full Xcode, not only its command line tools. The capture carries the debug names of buffers, images and pipelines; MoltenVK does not label the swapchain's images or the descriptor sets. KosmicKrisp has no such switch that the engine has used, so capture on MoltenVK.

On Linux, RenderDoc is the usual tool for a frame capture; the project has not used it yet.
