# Hoofstorm

A fork of the [Firestorm](https://github.com/FirestormViewer/phoenix-firestorm) Second Life viewer with a native Vulkan renderer. OpenGL stays as the fallback.

This is an unofficial personal fork. It is not supported by the Firestorm project.

## What is different from Firestorm

- **Render backend layer.** Calls that used to go straight to OpenGL now go through backend interfaces in `indra/llrender/`: render state, shaders, textures, vertex buffers and framebuffers. Each has a GL implementation and a Vulkan implementation.
- **Native Vulkan renderer** in `indra/llrender/vulkan/`. It is the default when the viewer is built with it. Set `FS_RENDER_BACKEND=opengl` to use OpenGL instead. If Vulkan window init fails, or no Vulkan driver is installed (`vulkan-1.dll` is delay-loaded), the viewer falls back to OpenGL.
- **Threaded renderer.** Vulkan backend calls run on a render thread, on by default (`FS_VK_RENDER_THREAD=0` runs them on the viewer thread). See `doc/vulkan/render-thread-design.md`.
- **FSR 1.0 upscaling.** Preferences > Graphics > Upscaling renders the 3D view at a lower scale (presets or a custom slider), upscales it with AMD FidelityFX Super Resolution 1.0 and sharpens it. The FSR state shows next to the FPS in the status bar tooltip. Optional dynamic scaling holds a target frame rate. Snapshots render at full size.
- **VRAM budget.** `VK_EXT_memory_budget` raises the texture discard bias when video memory runs low, and a texture that cannot be allocated falls back instead of crashing.
- **Performance work on the Vulkan path:**
  - two frames in flight (`FS_VK_FRAMES_IN_FLIGHT=1` turns it off)
  - batched texture uploads and a fast 8-bit upload path
  - async pipeline compilation (`FS_VK_ASYNC_PIPELINES=0` turns it off)
  - pooled vertex and index buffers
  - cached texture views and samplers, and persistent per-program uniform sets
  - push descriptors (`FS_VK_PUSH_DESCRIPTORS=0` turns them off) and narrow barriers (`FS_VK_NARROW_BARRIERS=0` turns them off)
  - redundant state, pipeline and buffer binds skipped per command buffer
  - small vertex buffers carved from shared slabs, and a fast exit
- **Distant avatar animation throttle.** Settings `FSDistantAvatarAnimPeriod` (default 3, 1 = off) and `FSDistantAvatarAnimDistance` (default 32).
- **Render target feedback tolerance.** A draw that samples a target it is writing uses a fallback texture instead of aborting. `FS_VK_STRICT_FEEDBACK=1` restores the abort.
- **Test suites** under `indra/llrender/vulkan/tests/` and a standalone skeleton in `indra/vulkan_skeleton/`.

## Status

Work in progress. With the threaded renderer, Vulkan runs in-world at roughly 85 to 95 FPS in the test region, but there is no like-for-like OpenGL comparison yet. Visual parity and coverage across GPUs are not fully verified, and FSR dynamic scaling has not been checked in-world. Windows and Visual Studio 2022 only.

## Building

Build as you would Firestorm, following the [Firestorm build instructions](https://wiki.firestormviewer.org/fs_compiling_firestorm). You also need the [Vulkan SDK](https://vulkan.lunarg.com/) with `glslang`/`glslc`. Helper scripts are in `doc/vulkan/tools/`.

## Documentation

Design notes, build commands, gotchas and profiling logs are in [`doc/vulkan/`](doc/vulkan). Start with `HANDOFF.md` and `PROJECT-NOTES.md`.

## License

Same as Firestorm and the Linden Lab viewer: LGPL 2.1. See [`LICENSE`](LICENSE) and the notices in the source tree. Firestorm and Second Life are trademarks of their respective owners.
