# FSR 2/3.1 Temporal Upscaler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add AMD FSR 3.1 temporal upscaling (jitter, camera-only motion vectors, reactive mask, history) to the Vulkan renderer, selectable in Preferences, with FSR 1.0 as the fallback.

**Architecture:** AMD FidelityFX SDK v1.1.4 is vendored under `indra/llrender/vulkan/ffx/`. A wrapper `VulkanFsrUpscaler` hides it. A backend entry point lets `pipeline.cpp` (which only knows GL texture names) request one upscale dispatch; the Vulkan backend resolves the names to images, ends dynamic rendering, dispatches, and resumes. `renderFinalize` runs the pass before tonemap.

**Tech Stack:** C++17, Vulkan 1.3 (existing `VkContext`), vendored FidelityFX SDK 1.1.4 (MIT, GLSL to SPIR-V), viewer GLSL for the motion and mask passes, XUI, MSBuild via `doc/vulkan/tools/build-viewer.cmd`.

**Spec:** `docs/superpowers/specs/2026-10-19-fsr-temporal-upscaler-design.md` (companion, built later: `2026-10-19-fsr3-frame-generation-design.md`)

## Global Constraints

- Vulkan only; OpenGL path and `RenderFSR*` (FSR 1.0) behavior unchanged. `RenderFSR2Enabled` and `RenderFSREnabled` are mutually exclusive.
- Settings: `RenderFSR2Enabled` bool false; `RenderFSR2Quality` U32 1; `RenderFSR2Sharpness` F32 0.5 (0 to 1); `RenderFSR2Dynamic` bool false.
- Quality modes (AMD divisors): 0 Native AA 1.0, 1 Quality 1.5, 2 Balanced 1.7, 3 Performance 2.0, 4 Ultra Performance 3.0. Render scale = 1/divisor. Jitter sequence lengths: 18 (1.5), 23 (1.7), 32 (2.0), 72 (3.0), ceil(8·n²) otherwise; never offset (0, 0).
- Jitter to projection: X = 2·jx/renderW, Y = -2·jy/renderH, `jittered = translation × projection`. Mip bias = log2(renderW/displayW) - 1 (1.5x gives -1.58, 2.0x gives -2.0).
- Reactive mask R8, clamped to 0.9. Motion vectors 2×FLOAT in pixels, `motionVectorScale` = render width and height. Frame time delta in ms, at least 1.0.
- Skip jitter and the upscaler for: shadow maps, probes, mirrors, snapshots, `gCubeSnapshot`, the GLTF material preview, the UI.
- Fallback chain: temporal upscaler, then FSR 1.0, then bilinear. Failure unticks the setting, logs one warning, shows the existing FSR failure message.
- Keep AMD's MIT copyright header in every vendored or derived file. `ffx/README` records version, source URL, tag, date and local edits.
- Downloading the SDK needs the user's approval (state the URL, file list and size when asking).
- Never launch or log into the viewer; the user runs in-world checks. Commit identity: `git -c user.name=thehorseofcourse45 -c user.email=178369127+thehorseofcourse45@users.noreply.github.com`; commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Build: run `cmd.exe //c "C:\phoenix-firestorm\doc\vulkan\tools\build-viewer.cmd"` in the background; pass is `BUILD_EXIT=0` in `build-vc170-64\viewer_build.log` with no `error C` or `error LNK`. Editing the version file alone does not change the exe; run `doc/vulkan/tools/reconfigure.cmd` first. The `firestorm-bin` build does not refresh `Release\app_settings`, `skins` or `shaders`; copy changed files by hand before the user runs it.
- Vulkan test runs need `export VK_LAYER_PATH=C:/VulkanSDK/1.4.357.0/Bin`. Pass means the test's own `PASS` line and zero validation messages.

## Review Focus

- Teleport finished, camera cut, window resize, render-scale change, toggle: history is reset and the first frame after is not a smear or blank (reset flag test).
- Window so small that the render size rounds to the display size: the upscaler is skipped and the plain path runs, no zero-size context.
- Minimized window (zero size): no create, no dispatch, no crash; restore recreates.
- SDK context creation fails (missing FP16 features, out of memory): setting unticks, FSR 1.0 takes over, warning logged once.
- Toggle FSR 1.0 and FSR 2 back and forth live: no leak of `mFSR2Map`, VRAM `upscaler` owner returns to 0 when off.
- Moving avatar with a static camera: reactive mask covers it so ghosting is limited (user-checked in-world).
- Effects that read the projection (SSR, SSAO, fog) do not shimmer under jitter (user-checked in-world, list in Task 7).

---

### Task 1: Vendor the SDK and build it

**Files:**
- Create: `indra/llrender/vulkan/ffx/` (SDK files, `README`), `indra/llrender/vulkan/tests/fsr2_context_test.cpp`
- Modify: `indra/llrender/vulkan/CMakeLists.txt` (SDK sources into `llvulkan`; new test target beside `llvulkan_fsr_pass_test`)

**Interfaces:**
- Produces: the SDK headers on the include path of `llvulkan` as `<ffx/...>`; a compiled `ffx` Vulkan backend with SPIR-V shader blobs generated at build time.

- [ ] **Step 1: Ask the user to approve the download** of FidelityFX-SDK tag `v1.1.4` (GitHub, GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), naming the folders to take: API core, FSR 3.1 upscaler component, Vulkan backend, and their shader sources. Stop until approved.
- [ ] **Step 2: Download into an empty scratch directory** (not the repo), read the tree, and write the exact copy list into `ffx/README` with version, URL, tag and date. Copy only that list into `ffx/`.
- [ ] **Step 3: Write `fsr2_context_test.cpp`**: init a headless validated `VkContext`, create the FSR 3.1 upscaler context at render 640×360, display 960×540, destroy it, check zero validation messages, print `FSR2_CONTEXT PASS`.
- [ ] **Step 4: Add CMake rules**: SDK sources into `llvulkan`; shader compile to SPIR-V with the glslang the repo already uses; permutations limited to HDR input, auto-exposure, FP16 plus FP32 fallback.
- [ ] **Step 5: Build and run** `cmake --build . --config Release --target llvulkan_fsr2_context_test`, then run it. Expected: `FSR2_CONTEXT PASS`.
- [ ] **Step 6: Commit** `Vendor FidelityFX SDK 1.1.4 (FSR 3.1 upscaler, Vulkan backend)`.

### Task 2: Pure helpers

**Files:**
- Create: `indra/llrender/llfsr2.h`, `indra/llrender/tests/fsr/fsr2_helpers_test.cpp`
- Modify: `indra/llrender/CMakeLists.txt` (test target next to `fsr_scale_test`)

**Interfaces:**
- Produces (inline, no namespace): `float fsr2QualityDivisor(unsigned quality)` (clamps to 0..4); `float fsr2RenderScale(unsigned quality)` (1/divisor); `float fsr2MipBias(float renderW, float displayW)`; `int fsr2JitterPhaseCount(float divisor)` (table in Global Constraints); `void fsr2JitterProjection(float jx, float jy, float renderW, float renderH, float out[16])` (column-major translation matrix).

- [ ] **Step 1: Write the failing test** asserting: divisors `{1.0,1.5,1.7,2.0,3.0}`; `fsr2RenderScale(1)` is 0.6667±1e-4; `fsr2MipBias(640,960)` is -1.585±0.01 and `(480,960)` is -2.0±0.01; phase counts `{8,18,23,32,72}` for divisors `{1.0,1.5,1.7,2.0,3.0}` (native uses 8); quality 9 clamps to Ultra Performance; translation matrix for `(0.5,-0.5,640,360)` has `m[12]=2*0.5/640` and `m[13]=-2*(-0.5)/360`.
- [ ] **Step 2: Run it, expect FAIL** (header missing).
- [ ] **Step 3: Implement** the five functions in `llfsr2.h`.
- [ ] **Step 4: Run it, expect `FSR2_HELPERS PASS`.**
- [ ] **Step 5: Commit.**

### Task 3: `VulkanFsrUpscaler` wrapper

**Files:**
- Create: `indra/llrender/vulkan/vkfsr2.h`, `indra/llrender/vulkan/vkfsr2.cpp`, `indra/llrender/vulkan/tests/fsr2_test.cpp`
- Modify: `indra/llrender/vulkan/CMakeLists.txt`, `indra/llrender/vulkan/vkcontext.h` (counter `std::atomic<int64_t> upscalerMemBytes`, hook `bool failFsr2Create`)

**Interfaces:**
- Consumes: Task 1 SDK; `VkContext` (device, queue, command buffer of the current frame `slot().cmd`).
- Produces:
  - `struct Fsr2Inputs { VkImageView color, depth, motion, reactive; float jitterX, jitterY, frameTimeMs, sharpness; bool reset; bool depthInverted; }`
  - `class VulkanFsrUpscaler { bool create(VkContext&, uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH); bool resize(...same...); bool dispatch(VkCommandBuffer, const Fsr2Inputs&, VkImageView output); void destroy(); int64_t memoryBytes() const; }`
  - `create`/`resize` return false when `ctx.failFsr2Create` is set or the SDK reports an error; `memoryBytes` feeds `ctx.upscalerMemBytes`.

- [ ] **Step 1: Write `fsr2_test.cpp`** (validated headless context, synthetic 640×360 colour with a moving square, depth constant, zero motion): `create` succeeds; 8 dispatches with changing jitter write a display-size, non-uniform output (read back centre vs corner pixels differ); `reset=true` frame afterwards produces output without validation messages; with `ctx.failFsr2Create=true` `create` returns false and `memoryBytes()` is 0; teardown has zero validation messages. Prints `FSR2_WRAPPER PASS`.
- [ ] **Step 2: Run, expect FAIL** (class missing).
- [ ] **Step 3: Implement** the class. Choose FP16 only when `shaderFloat16` and `storageBuffer16BitAccess` are both set. Add the image barriers the SDK needs (`VK_ACCESS_SHADER_READ_BIT` inputs) inside `dispatch`.
- [ ] **Step 4: Run, expect `FSR2_WRAPPER PASS`.**
- [ ] **Step 5: Commit.**

### Task 4: Backend entry point

**Files:**
- Modify: `indra/llrender/llrenderbackend.h`, `indra/llrender/llrenderbackend_vulkan.cpp`, `indra/llrender/vulkan/vkframebufferbackend.cpp` (and `.h` if needed), `indra/llrender/vulkan/vktexturebackend.h` (lookup of a texture's `VkImageView` by GL name)
- Test: `indra/llrender/vulkan/tests/frames_test.cpp` (new case)

**Interfaces:**
- Consumes: Task 3 wrapper.
- Produces in `llrenderbackend.h`:
  - `struct RenderBackendFsr2Params { unsigned colorTex, depthTex, motionTex, reactiveTex, outputTex; unsigned renderW, renderH, displayW, displayH; float jitterX, jitterY, frameTimeMs, sharpness; bool reset; }`
  - `bool dispatchRenderBackendFsr2(const RenderBackendFsr2Params&)` (false on any failure or when the Vulkan backend is not active; manages the wrapper lifetime, recreating it when sizes change)
  - `void releaseRenderBackendFsr2()` (frees history and sets `upscalerMemBytes` to 0)
  - `bool isRenderBackendFsr2Failed()` (true after the failure rule in Task 10 trips)

- [ ] **Step 1: Write the failing frames-test case**: render a gradient into a render-size target, call `dispatchRenderBackendFsr2` with the target textures, assert true, output texture non-blank, and that normal rendering into the next target still works (proves dynamic rendering resumed) with zero validation messages.
- [ ] **Step 2: Run, expect FAIL** (symbol missing).
- [ ] **Step 3: Implement** the three functions: end dynamic rendering through `VulkanFramebufferBackend::endRendering()`, resolve names to image views, transition layouts through the texture backend, call the wrapper, and let the next draw reopen rendering.
- [ ] **Step 4: Run the frames test and the activation test (render thread on and off); expect the existing pass counts plus the new case.**
- [ ] **Step 5: Commit.**

### Task 5: Settings, mutual exclusion, render scale

**Files:**
- Modify: `indra/newview/app_settings/settings.xml`, `indra/newview/pipeline.cpp`, `indra/newview/pipeline.h`, `indra/newview/llviewercontrol.cpp`
- Test: `indra/llrender/tests/fsr/fsr2_helpers_test.cpp`

**Interfaces:**
- Consumes: Task 2 helpers.
- Produces: `bool LLPipeline::isFsr2Active()` (setting on, Vulkan backend active, not `gCubeSnapshot`, render scale below 1, not failed); `getRenderScale()` returns `fsr2RenderScale(RenderFSR2Quality)` when FSR 2 is on (dynamic scaling reuses the FSR 1.0 controller).

- [ ] **Step 1: Add a test case** for the pure function `effectiveRenderScaleFsr2(bool fsr2, unsigned quality, float mul)` in `llfsr2.h`: on returns `1/divisor`; off returns `mul`; native AA (divisor 1.0) returns 1.0.
- [ ] **Step 2: Run, expect FAIL; implement; run, expect PASS.**
- [ ] **Step 3: Add the four settings** to `settings.xml` with the defaults above, cached statics in `pipeline.cpp`, and signal listeners in `llviewercontrol.cpp` that, when one of the two enable flags turns on, unticks the other and reallocates the screen buffers.
- [ ] **Step 4: Build the viewer** (background); expect `BUILD_EXIT=0`.
- [ ] **Step 5: Commit.**

### Task 6: Jitter and previous view-projection

**Files:**
- Modify: `indra/newview/llviewercamera.cpp` (~385), `indra/newview/pipeline.cpp`, `indra/newview/pipeline.h`
- Test: `indra/llrender/tests/fsr/fsr2_helpers_test.cpp`

**Interfaces:**
- Consumes: Task 2 `fsr2JitterProjection`, Task 5 `isFsr2Active()`.
- Produces: `glm::mat4 LLPipeline::mPrevViewProj`, `glm::mat4 LLPipeline::mCurViewProjUnjittered`, `int LLPipeline::mFsr2Phase` advanced once per displayed frame, and `glm::vec2 LLPipeline::getFsr2Jitter()` (pixels; never 0,0).

- [ ] **Step 1: Test**: a pure helper `fsr2NextPhase(int phase, int count)` wraps at `count` and never returns a negative; `fsr2JitterProjection` of an identity-projection point shifts it by exactly one jitter in NDC.
- [ ] **Step 2: Run, expect FAIL; implement; run, expect PASS.**
- [ ] **Step 3: Apply the jitter** in `LLViewerCamera::setPerspective` only when `isFsr2Active()` and the current render is the main scene view (not shadow, probe, mirror, snapshot, preview); store the unjittered matrix before applying.
- [ ] **Step 4: Give projection consumers the unjittered matrix**: list every shader uniform fed from `set_current_projection` / `gGLProjection` used by SSR, SSAO, fog and depth reconstruction; use the stored unjittered matrix for them. Record the list in the commit message.
- [ ] **Step 5: Build the viewer; expect `BUILD_EXIT=0`. Commit.**

### Task 7: Motion vector pass

**Files:**
- Create: `indra/newview/app_settings/shaders/class1/deferred/fsr2MotionF.glsl`
- Modify: `indra/newview/llviewershadermgr.cpp` (+ `.h`: `gDeferredFSR2MotionProgram`), `indra/newview/pipeline.cpp` (+ `.h`: `mFSR2MotionMap`, RG16F render-size target)
- Test: `indra/llrender/vulkan/tests/shader_test.cpp` (compile case)

**Interfaces:**
- Consumes: Task 6 matrices.
- Produces: `void LLPipeline::generateFsr2Motion(LLRenderTarget* depthSource, LLRenderTarget* dest)` writing pixel-space motion; uniforms `inv_view_proj_cur`, `view_proj_prev`, `screen_res`.

- [ ] **Step 1: Add a shader compile case** for `fsr2MotionF.glsl` to the existing Vulkan shader test; run, expect FAIL (file missing).
- [ ] **Step 2: Write the shader**: reconstruct world position from depth with the unjittered inverse view-projection, reproject with `view_proj_prev`, output `(prevNdc - curNdc) * 0.5 * screen_res`; document the depth convention found in the pipeline in a comment.
- [ ] **Step 3: Register the program** (loaded only while FSR 2 is on); add the target in `allocateScreenBuffer`, released when off or on resize.
- [ ] **Step 4: Run the shader test; expect PASS. Add a frames-test case**: static camera gives exactly zero motion at the centre pixel; a shifted previous matrix gives the expected non-zero value.
- [ ] **Step 5: Commit.**

### Task 8: Reactive mask pass

**Files:**
- Create: `indra/newview/app_settings/shaders/class1/deferred/fsr2ReactiveF.glsl`
- Modify: `indra/newview/llviewershadermgr.cpp`, `indra/newview/pipeline.cpp`/`.h` (`mFSR2ReactiveMap`, R8)
- Test: shader compile case in `indra/llrender/vulkan/tests/shader_test.cpp`

**Interfaces:**
- Produces: `void LLPipeline::generateFsr2Reactive(LLRenderTarget* dest)`; mask values clamped to 0.9, 0 where nothing is flagged.

- [ ] **Step 1: Compile test for the shader; run, expect FAIL.**
- [ ] **Step 2: Implement**: v1 derives the mask from existing deferred buffers (avatar and alpha-flagged pixels from the material/normal alpha channels where present); where a surface cannot be identified from them, add a forward-pass write only if the in-world check in Task 11 shows ghosting there.
- [ ] **Step 3: Run the shader test; expect PASS. Commit.**

### Task 9: `renderFinalize` integration

**Files:**
- Modify: `indra/newview/pipeline.cpp` (~9070 to ~9250), `indra/newview/pipeline.h`, the texture LOD bias path found by `grep -rn "TextureLODBias\|mipBias" indra/newview` (record the file in the commit)
- Test: `indra/llrender/vulkan/tests/frames_test.cpp`

**Interfaces:**
- Consumes: Tasks 4, 6, 7, 8.
- Produces: `mFSR2Map` (display-size HDR target); at the start of `renderFinalize`, when `isFsr2Active()`: motion pass, reactive pass, `dispatchRenderBackendFsr2`, then the rest of the chain (tonemap onward) reads `mFSR2Map` instead of `mRT->screen`. FXAA and SMAA are skipped. History reset flag set from the events below.

- [ ] **Step 1: Frames-test case**: full-size chain on a synthetic scene with FSR 2 on produces display-size, non-blank output and zero validation messages; with it off the output equals today's.
- [ ] **Step 2: Implement the pass ordering** and the `mFSR2Map` lifetime (allocated when on, released when off or on resize or size zero).
- [ ] **Step 3: Set the reset flag** on: teleport finished (`LLAgent::handleTeleportFinished` / `update_tp_display` reaching `TELEPORT_NONE`), window resize, render-scale change, toggle, swapchain recreation, and camera-position jumps over a threshold stored as a named constant.
- [ ] **Step 4: Apply and restore the texture LOD bias** from `fsr2MipBias` when the mode changes.
- [ ] **Step 5: Build the viewer; run frames, rgb10, activation tests (render thread on and off). Expect no regression with the feature off. Commit.**

### Task 10: Fallbacks and failure handling

**Files:**
- Modify: `indra/newview/pipeline.cpp`, `indra/newview/llviewercontrol.cpp`, `indra/newview/llviewershadermgr.cpp`, `indra/llrender/llrenderbackend_vulkan.cpp`
- Test: `indra/llrender/vulkan/tests/fsr2_test.cpp`

**Interfaces:**
- Consumes: `ctx.failFsr2Create` hook from Task 3.
- Produces: on create or resize failure, shader load failure, or 3 consecutive dispatch errors: `RenderFSR2Enabled` set false, one warning logged, the FSR 1.0 failure message shown, `RenderFSREnabled` set true at the matching scale.

- [ ] **Step 1: Test** (in `fsr2_test.cpp`): force `failFsr2Create`; call `dispatchRenderBackendFsr2` three times; `isRenderBackendFsr2Failed()` becomes true after the third error and no further dispatch is attempted.
- [ ] **Step 2: Run, expect FAIL; implement the rule and the settings flip; run, expect PASS.**
- [ ] **Step 3: Review Focus cases**: zero-size window and render size equal to display size skip creation (assert in the frames test).
- [ ] **Step 4: Commit.**

### Task 11: Preferences UI and presets

**Files:**
- Modify: `indra/newview/skins/default/xui/en/panel_preferences_graphics1.xml` (Upscaling tab, find it with `grep -rn "FSRQuality" indra/newview/skins`), `indra/newview/app_settings/graphic_preset_controls.xml`, `indra/newview/skins/default/xui/en/strings.xml`, the render size line in `pipeline.cpp`

**Interfaces:**
- Produces: checkbox "Temporal upscaler (FSR 2/3)", quality combo (5 modes), sharpness slider, dynamic checkbox; disabled with a tooltip when the renderer is OpenGL; FXAA/SMAA controls greyed while FSR 2 is on.

- [ ] **Step 1: Add the controls and strings; add the four settings to `graphic_preset_controls.xml`.**
- [ ] **Step 2: Copy changed `app_settings`, `skins` and `shaders` files into `build-vc170-64\newview\Release\` by hand** (the `firestorm-bin` build does not refresh them).
- [ ] **Step 3: Build; expect `BUILD_EXIT=0`. Commit.**

### Task 12: VRAM owner, notes, in-world check

**Files:**
- Modify: `indra/llrender/llrenderbackend_vulkan.cpp` (add `upscaler=` to the `VRAM by owner MB` line), `doc/vulkan/PROJECT-NOTES.md`, `doc/vulkan/HANDOFF.md`

- [ ] **Step 1: Add the owner and verify the log line shows `upscaler=0` with the feature off.**
- [ ] **Step 2: Update PROJECT-NOTES.md and HANDOFF.md** with status, test counts, vendored-SDK facts and the in-world checklist.
- [ ] **Step 3: Copy the exe to `Hoofstorm.exe` (viewer closed), then ask the user to run the in-world checks**: static sharpness vs FSR 1.0, avatar ghosting, teleport and camera-cut reset, resize, every mode, shimmer in SSR/SSAO/fog, 40 minutes VRAM stability, and the performance gate (temporal Quality at least 15 percent faster than native in the same scene, number set by the user). The assistant reads the log afterwards.
- [ ] **Step 4: Commit and, with approval, push.**
