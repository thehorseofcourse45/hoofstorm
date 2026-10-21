# FSR 1.0 Upscaling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the scene below window size and upscale it with AMD FSR 1.0 (EASU then RCAS), selectable in Preferences, on both the Vulkan and OpenGL renderers.

**Architecture:** A pure helper maps a quality preset to a render scale. `pipeline.cpp` uses it in place of `RenderResolutionMultiplier` when FSR is on. The last step of `renderFinalize` gains an EASU pass into a window-sized target, and the existing final shader samples that target with RCAS sharpening. Two new GLSL files carry AMD's code inlined (the shader loader has no `#include`).

**Tech Stack:** C++17, GLSL 330-style viewer shaders translated to SPIR-V by glslang (Vulkan), viewer XUI, MSBuild via `doc/vulkan/tools/build-viewer.cmd`.

**Spec:** `docs/superpowers/specs/2026-10-18-fsr-upscaling-design.md`

## Global Constraints

- Presets: 0 Ultra Quality 0.77, 1 Quality 0.67, 2 Balanced 0.59, 3 Performance 0.50 (render scale per axis).
- Settings: `RenderFSREnabled` bool false; `RenderFSRQuality` U32 1; `RenderFSRSharpness` F32 0.2 (AMD stops, 0 sharpest, 2 off; slider shows the inverse).
- FSR off must leave behavior identical to today, including `RenderResolutionMultiplier`; `RenderResolutionDivisor` keeps priority.
- Skipped for snapshots, the cube-map snapshot path and the GLTF material preview.
- Keep AMD's MIT copyright and license header in every derived shader file. Source copies: `C:\Users\TheTaZe\AppData\Local\Temp\claude\C--phoenix-firestorm\3407b543-529b-44e0-ad62-ad9074ce9979\scratchpad\fsr-src\ffx_a.h` and `ffx_fsr1.h`.
- Never launch or log into the viewer; the user does the in-world check. Commit identity: `git -c user.name=thehorseofcourse45 -c user.email=timberjoey4@yahoo.com`; commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Update Status and Next steps in `doc/vulkan/PROJECT-NOTES.md` at the end.
- Build: run `cmd.exe //c "C:\phoenix-firestorm\doc\vulkan\tools\build-viewer.cmd"` in the background; pass means `BUILD_EXIT=0` and no `error C` / `error LNK` in `build-vc170-64\viewer_build.log`. The viewer exe cannot be overwritten while the viewer runs.

## Review Focus

- Window smaller than the preset allows (scaled size rounds to the window size): FSR is skipped, plain path runs.
- Window resized or minimized while FSR is on: `mFSRMap` is reallocated at the new size, no crash on a zero-size target.
- FSR toggled or quality changed live: screen buffers reallocate, no leak of `mFSRMap`, output not blank.
- EASU or RCAS shader fails to compile: FSR disables itself for the session and the bilinear stretch is used.
- `RenderResolutionMultiplier` below 1 with FSR off: unchanged output.

---

### Task 1: Settings and render-scale helper

**Files:**
- Create: `indra/llrender/llfsr.h`, `indra/llrender/tests/fsr/fsr_scale_test.cpp`
- Modify: `indra/llrender/CMakeLists.txt` (test target next to `viewer_vulkan_activation_test`), `indra/newview/app_settings/settings.xml`, `indra/newview/pipeline.cpp` (+ `pipeline.h` statics), `indra/newview/llviewercontrol.cpp`

**Interfaces:**
- Produces in `llfsr.h` (inline, no namespace): `float fsrQualityScale(unsigned quality)` (clamps quality to 0..3, returns the preset table value); `float effectiveRenderScale(bool fsrEnabled, unsigned quality, float multiplier)` (returns the preset scale when `fsrEnabled`, else `multiplier` when `0 < multiplier < 1`, else `1.0f`).
- Produces in `LLPipeline`: statics `bool RenderFSREnabled; U32 RenderFSRQuality; F32 RenderFSRSharpness;` and `static F32 getRenderScale()` returning `effectiveRenderScale(RenderFSREnabled, RenderFSRQuality, RenderResolutionMultiplier)`.

- [ ] **Step 1: Write the failing test** `fsr_scale_test.cpp`, a `main` that asserts: `fsrQualityScale(0)==0.77f`, `(1)==0.67f`, `(2)==0.59f`, `(3)==0.50f`, `(99)==0.50f`; `effectiveRenderScale(false,1,1.0f)==1.0f`; `(false,1,0.8f)==0.8f` (multiplier path unchanged); `(true,1,0.8f)==0.67f` (FSR overrides); `(false,1,0.0f)==1.0f`. Returns nonzero on any failure and prints `FSR_SCALE_CHECKS=<n> PASS`.
- [ ] **Step 2: Add the test target** `fsr_scale_test` (EXCLUDE_FROM_ALL, include dir `indra/llrender`) and run `cmake --build build-vc170-64 --config Release --target fsr_scale_test`. Expected: compile error, `llfsr.h` missing.
- [ ] **Step 3: Implement `llfsr.h`** with the two functions above.
- [ ] **Step 4: Run the test.** Expected: `FSR_SCALE_CHECKS=9 PASS`.
- [ ] **Step 5: Add the three settings** to `settings.xml` after `RenderResolutionMultiplier`, with the defaults and types from Global Constraints and one-line comments.
- [ ] **Step 6: Wire `pipeline.cpp`:** declare and cache the three statics like `RenderResolutionMultiplier` (cached-settings read near line 1221, `connectRefreshCachedSettingsSafe` near line 590); add `getRenderScale()`; replace the two `RenderResolutionMultiplier` uses (lines ~840 and ~971) with `getRenderScale()`, keeping the `> 0 && < 1` guard and the divisor branch first.
- [ ] **Step 7: Register listeners** in `llviewercontrol.cpp` for `RenderFSREnabled` and `RenderFSRQuality` using `handleRenderResolutionDivisorChanged` (sets `gResizeScreenTexture`), next to the multiplier listener (line ~1351).
- [ ] **Step 8: Build the viewer.** Expected: `BUILD_EXIT=0`, no errors.
- [ ] **Step 9: Commit** `Add FSR settings and render-scale helper`.

### Task 2: EASU and RCAS shaders

**Files:**
- Create: `indra/newview/app_settings/shaders/class1/deferred/fsrEasuF.glsl`, `.../fsrRcasF.glsl`, `indra/llrender/vulkan/tests/fsr_shader_test.cpp`
- Modify: `indra/llrender/vulkan/CMakeLists.txt` (new EXCLUDE_FROM_ALL target `llvulkan_fsr_shader_test`)

**Interfaces:**
- Consumes: shader conventions of `postDeferredNoDoFF.glsl` (`/*[EXTRA_CODE_HERE]*/`, `out vec4 frag_color`, `in vec2 vary_fragcoord`, `uniform sampler2D diffuseRect`, `uniform vec2 screen_res`).
- Produces: `fsrEasuF.glsl` with uniforms `diffuseRect`, `uniform vec2 fsr_input_size` (source pixels), `uniform vec2 fsr_output_size` (window pixels); writes RGBA. `fsrRcasF.glsl` with `diffuseRect` (the EASU output), `depthMap`, `uniform float fsr_sharpness` (RCAS attenuation stops, 0 sharpest), `uniform vec2 screen_res`, keeping `HAS_NOISE`, `clampHDRRange` and the `gl_FragDepth` write of `postDeferredNoDoFF.glsl`.

- [ ] **Step 1: Write the failing test** `fsr_shader_test.cpp`: reads both shader files (directory from `argv[1]`), prepends `#version 330` and a stub for `clampHDRRange`, compiles each through `LLShaderCompiler::compileProgram` with a trivial vertex stage (pattern: `shader_runtime_test.cpp`) and `LLShaderCompiler::viewerOptions()`, and asserts the `Result` succeeds and `validateSpirv` is true for both. Prints `FSR_SHADER_CHECKS=2 PASS`.
- [ ] **Step 2: Run it.** Expected: FAIL, shader files missing.
- [ ] **Step 3: Write `fsrEasuF.glsl`:** AMD header, then inline `ffx_a.h` and `ffx_fsr1.h` with `#define A_GPU 1`, `#define A_GLSL 1`, `#define FSR_EASU_F 1`; `FsrEasuCon` constants computed from the two size uniforms; `FsrEasuF` called in `main`; the `FsrEasuRF/GF/BF` tap functions backed by `textureGather` on `diffuseRect`. Unused `ffx_a.h` sections may be removed to keep the file small.
- [ ] **Step 4: Write `fsrRcasF.glsl`** the same way with `FSR_RCAS_F`, `FsrRcasCon(con, fsr_sharpness)`, `FsrRcasF` in `main`, then the noise, `clampHDRRange` and depth lines copied from `postDeferredNoDoFF.glsl`.
- [ ] **Step 5: Run the test.** Expected: `FSR_SHADER_CHECKS=2 PASS`. If glslang rejects `textureGather` or another feature the translator lacks, rewrite the taps with `texelFetch` and re-run.
- [ ] **Step 6: Commit** `Add FSR EASU and RCAS shaders`.

### Task 3: Shader programs and pipeline pass

**Files:**
- Modify: `indra/newview/llviewershadermgr.cpp` / `.h`, `indra/newview/pipeline.cpp` / `pipeline.h`

**Interfaces:**
- Consumes: Task 1 `LLPipeline::RenderFSREnabled`, `RenderFSRSharpness`, `getRenderScale()`; Task 2 shaders and uniform names.
- Produces: `LLGLSLShader gDeferredFSREasuProgram, gDeferredFSRRcasProgram` (declared in `llviewershadermgr.h`); `LLRenderTarget LLPipeline::mFSRMap`; `bool LLPipeline::fsrActive() const`, true when FSR is enabled, both programs `isComplete()`, `mFSRMap.isComplete()`, not `gCubeSnapshot`, and `mRT->screen` is smaller than `mRT->width` by `mRT->height`.

- [ ] **Step 1: Register the programs** in `llviewershadermgr.cpp` after `gDeferredPostNoDoFNoiseProgram` (line ~2882), same pattern (vertex `deferred/postDeferredNoTCV.glsl`, fragment the new file, `HAS_NOISE` permutation on the RCAS program), created only when `RenderFSREnabled`. On `createShader()` failure set the `RenderFSREnabled` setting to false and log a warning.
- [ ] **Step 2: Allocate `mFSRMap`** (RGBA8, `mRT->width` by `mRT->height`, the unscaled window size) in `allocateScreenBufferInternal` beside `mFXAAMap` (line ~1020) when `RenderFSREnabled` and `getRenderScale() < 1`; `release()` it in `releaseScreenBuffers` (line ~1362). Skip allocation for zero dimensions.
- [ ] **Step 3: Add `fsrActive()`** and the two-pass present in `renderFinalize` at "Present the screen target" (~9115): when `fsrActive()`, bind `mFSRMap`, draw the screen triangle with EASU (`fsr_input_size` = `sourceBuffer` size, `fsr_output_size` = `mFSRMap` size), unbind, then draw the existing screen triangle with `gDeferredFSRRcasProgram` reading `mFSRMap` (depth map and `screen_res` as in the current final program, `fsr_sharpness` = `RenderFSRSharpness`). Otherwise leave the existing code path untouched. Snapshot and GLTF-preview callers already render with `fsrActive()` false because `gCubeSnapshot` or their own targets make `mRT->screen` full size; verify this when reading the code.
- [ ] **Step 4: Build the viewer.** Expected: `BUILD_EXIT=0`.
- [ ] **Step 5: Run regressions** from `build-vc170-64` with `VK_LAYER_PATH=C:/VulkanSDK/1.4.357.0/Bin`: `llvulkan_frames_test` (51), `llvulkan_rgb10_test` (41), `viewer_vulkan_activation_test` (45, with `FS_VK_RENDER_THREAD` 1 and 0). Expected: all PASS.
- [ ] **Step 6: Commit** `Add FSR upscale pass to the final present`.

### Task 4: Offscreen pass check on the Vulkan backend

**Files:**
- Modify: `indra/llrender/vulkan/tests/frames_test.cpp` (new check)

**Interfaces:**
- Consumes: Task 2 shader files read from disk (directory in `argv[2]`; skip the check with a printed SKIP when absent).

- [ ] **Step 1: Write the failing check** in `frames_test.cpp`: render a 64x64 horizontal gradient into a 32x32 RGBA8 target, run the EASU shader into a 64x64 target, read it back, and assert the output is 64x64, not all one value, and `pixel(63,0).r > pixel(0,0).r`.
- [ ] **Step 2: Run it.** Expected: FAIL (check not wired).
- [ ] **Step 3: Wire the check** using the file's existing helpers for targets, shader compile and readback.
- [ ] **Step 4: Run `llvulkan_frames_test`** with the shader directory. Expected: `FRAMES_CHECKS=54 PASS` (51 + 3) with zero validation messages.
- [ ] **Step 5: Commit** `Test FSR EASU through the Vulkan backend`.

### Task 5: Preferences UI and presets

**Files:**
- Modify: `indra/newview/skins/default/xui/en/panel_preferences_graphics1.xml`, `indra/newview/app_settings/graphic_preset_controls.xml`, `indra/newview/llfloaterpreference.cpp` (only if the slider needs a commit handler for the inverse display)

**Interfaces:**
- Consumes: the three settings from Task 1.

- [ ] **Step 1: Add controls** near the antialiasing combo (line ~1014): `check_box` `fsr_enabled` bound to `RenderFSREnabled`, label "FSR upscaling"; `combo_box` `fsr_quality` bound to `RenderFSRQuality` with items Ultra Quality 0, Quality 1, Balanced 2, Performance 3; `slider` `fsr_sharpness`, label "FSR sharpness", showing the inverse of `RenderFSRSharpness` (min 0, max 2, step 0.1; use a commit handler if the slider cannot invert).
- [ ] **Step 2: Enable/disable:** `fsr_quality` and `fsr_sharpness` use `enabled_control` on `RenderFSREnabled`.
- [ ] **Step 3: Add the three setting names** to `graphic_preset_controls.xml` in alphabetical position near `RenderFSAAType`.
- [ ] **Step 4: Build the viewer.** Expected: `BUILD_EXIT=0`, no XUI parse errors in the build log.
- [ ] **Step 5: Commit** `Add FSR controls to Preferences, Graphics`.

### Task 6: Notes and handoff

**Files:**
- Modify: `doc/vulkan/PROJECT-NOTES.md`

- [ ] **Step 1: Copy the new exe** once the user has closed the viewer: `cp build-vc170-64/newview/Release/firestorm-bin.exe build-vc170-64/newview/Release/Hoofstorm.exe`.
- [ ] **Step 2: Add a status entry** to `PROJECT-NOTES.md` (what was built, test counts, not checked in-world) and commit `Notes: FSR upscaling`.
- [ ] **Step 3: Hand the user the in-world check list:** enable FSR in Preferences, Graphics; try each preset; confirm the picture is sharp and not blank; toggle live; resize the window; take a snapshot.
