# FSR 1.0 upscaling

Date: 2026-10-18. Branch: vulkan-framebuffer.

## Goal

More frames per second on weaker GPUs or at 4K, with less quality loss than a bilinear stretch. The scene renders below window size and AMD FidelityFX Super Resolution 1.0 (FSR) upscales it to the window. It works on both the Vulkan and OpenGL renderers, because the viewer's GLSL is shared (Vulkan translates it to SPIR-V).

## Background

- `RenderResolutionMultiplier` (Catznip patch, `pipeline.cpp` lines ~840 and ~971) already shrinks every deferred render target. The final pass in `LLPipeline::renderFinalize` (`gDeferredPostNoDoFNoiseProgram`, `pipeline.cpp` ~9115) then stretches the result to the window with one bilinear sample. That stretch is what FSR replaces.
- That final pass also adds noise, applies `clampHDRRange`, and writes `gl_FragDepth` from the scene depth. All of it must be kept.
- FSR source: `ffx_a.h` and `ffx_fsr1.h` (v1.20210629) from GPUOpen-Effects/FidelityFX-FSR, MIT license. Downloaded to the session scratchpad; the AMD copyright and license header stays in every derived file.

## Settings

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `RenderFSREnabled` | bool | false | Use FSR when the scene renders below window size |
| `RenderFSRQuality` | U32 | 1 | 0 Ultra Quality (0.77), 1 Quality (0.67), 2 Balanced (0.59), 3 Performance (0.50): render scale per axis |
| `RenderFSRSharpness` | F32 | 0.2 | RCAS sharpening, 0 (sharpest) to 2 (off) in AMD's stops; the slider shows the inverse so higher means sharper |

A helper in `pipeline.cpp` returns the effective render scale. With FSR on it returns the preset value and ignores `RenderResolutionMultiplier`. With FSR off it returns `RenderResolutionMultiplier` exactly as today. The two existing uses are replaced by this helper. `RenderResolutionDivisor` keeps its current priority.

## Shaders

New files in `app_settings/shaders/class1/deferred/`:

- `fsrEasuF.glsl`: EASU, the edge-adaptive upscale. Reads the reduced-resolution `sourceBuffer`, writes window resolution.
- `fsrRcasF.glsl`: RCAS sharpening, plus the existing noise, `clampHDRRange` and depth write of `postDeferredNoDoFF.glsl`.
- The AMD code is shared between them. Step one of the implementation checks whether the shader loader supports including a shared file. If it does not, the AMD code is inlined into both files.
- Both are registered as `gDeferredFSREasuProgram` and `gDeferredFSRRcasProgram` in `llviewershadermgr.cpp`, loaded only when FSR is enabled.

## Pipeline

In `renderFinalize`, at "Present the screen target":

1. If FSR is enabled, the render scale is below 1, and the source is smaller than the window: draw EASU from `sourceBuffer` into `mFSRMap`, a window-sized RGBA8 target.
2. The final present draws RCAS reading `mFSRMap` instead of the plain program reading `sourceBuffer`. Depth still comes from `mRT->deferredScreen`.
3. Otherwise the existing final pass runs unchanged.

`mFSRMap` is allocated in `allocateScreenBuffer` when FSR is on and released when it is off or when the window resizes. FSR is skipped for snapshots, the cube-map snapshot path, and the GLTF material preview.

FSR runs after tonemap and gamma correction, so its input is display-ready color, which is what AMD's EASU and RCAS expect.

## Preferences UI

Preferences, Graphics tab (`panel_preferences_graphics1.xml`): a checkbox "FSR upscaling", a quality combo box, and a sharpness slider. The combo and slider are disabled while the checkbox is off. Changing any of the three reallocates the screen buffers, the same way the render scale change does today (`handleRenderResolutionDivisorChanged` path). All three settings are added to `graphic_preset_controls.xml` so presets save them. Labels are added to `en/strings.xml`.

## Errors and fallbacks

- If `mFSRMap` fails to allocate or an FSR shader fails to compile, FSR turns itself off for the session, logs a warning, and the bilinear stretch is used.
- If the render scale resolves to 1 (window smaller than the preset allows), FSR is skipped.

## Testing

- Compile both FSR shaders through the existing Vulkan shader test (`llvulkan_shader_test`) and through the OpenGL loader.
- Run the Vulkan frames, rgb10 and activation tests, render thread on and off, to confirm no regression with FSR off.
- A frames-test case that renders a known gradient at 0.5 scale through both passes and checks that the output is window sized and not blank.
- Visual check in-world by the user. The assistant does not launch the viewer.

## Out of scope

FSR 2/3 (temporal), dynamic resolution that adapts to frame time, and per-renderer defaults.
