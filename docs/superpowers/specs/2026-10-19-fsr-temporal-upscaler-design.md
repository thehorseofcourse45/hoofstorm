# FSR 2/3.1 temporal upscaler (Vulkan)

Date: 2026-10-19. Branch: vulkan-framebuffer. Status: draft for review.
Companion spec: `2026-10-19-fsr3-frame-generation-design.md` (built after this one).

## Goal

Better image quality than FSR 1.0 at the same render scale, or more frames per second at the same quality, on the Vulkan renderer. FSR 1.0 is spatial and looks soft. A temporal upscaler accumulates sub-pixel detail over frames, so it can render at 0.5 to 0.67 scale and look close to native, with built-in anti-aliasing.

Success is measured in the same scene on the user's RX 6700 XT: temporal Quality must look at least as sharp as FSR 1.0 Quality in a static scene (user's visual check), and must pass the performance gate in Testing item 6. If it fails the gate, the feature ships off by default.

## Decisions already made

- **Renderer:** Vulkan only. OpenGL keeps FSR 1.0 unchanged.
- **Motion vectors:** camera-only reprojection from depth. Per-object vectors are out of scope.
- **Code source:** AMD FidelityFX SDK **v1.1.4** vendored into the tree. Reason: v1.1.4 is the last SDK release with a Vulkan backend; SDK v2.x (2.0 to 2.3 at time of writing) lists Vulkan as unsupported, and v2 marks v1 deprecated. FSR 4 and the ML upscalers exist only in v2.x and are out of reach. The vendored copy is frozen; we own any fixes.
- **Frame generation:** separate spec, built after this one is verified in-world.

## Background

- `LLPipeline::renderFinalize` ([pipeline.cpp:9070](../../../indra/newview/pipeline.cpp)) runs: SSR copy, luminance, exposure, `tonemap` (~9104), `gammaCorrect` (~9114), glow, DoF, FXAA or SMAA, then "Present the screen target" (~9214) where FSR 1.0 runs (EASU into `mFSRMap`, then RCAS).
- FSR 1.0 is implemented by `LLPipeline::getRenderScale()` (`llfsr.h`), `RenderFSREnabled`, `RenderFSRQuality`, `RenderFSRSharpness`, and dynamic scaling. See `2026-10-18-fsr-upscaling-design.md`.
- The projection matrix is set in `llviewercamera.cpp` (~385, `set_current_projection`).
- No jitter, velocity buffer, history buffer or TAA exists in the viewer today.
- The Vulkan context has one queue and a pool for host buffers; VRAM use is logged by owner (`VRAM by owner MB`). In a 39-minute session on the 11.5 GB test GPU, VRAM used was 5.2 GB and flat, so there is headroom for the history targets.

## Vendored SDK

Location: `indra/llrender/vulkan/ffx/`. Contents (exact list fixed when the plan is written, from the v1.1.4 tree): the FidelityFX API core, the FSR 3.1 upscaler component, the Vulkan backend, and its GLSL shader sources. Frame interpolation and optical flow sources are **not** vendored until spec B starts.

- Each file keeps AMD's MIT copyright header. A `ffx/README` records: SDK version (1.1.4), source URL, commit or tag, date vendored, and every local modification.
- Shaders compile to SPIR-V at build time with the toolchain the Vulkan shader tests already use. The shader permutations are limited to the ones used: HDR input, inverted depth off, auto-exposure on, FP16 with an FP32 fallback chosen at run time from `shaderFloat16` and `storageBuffer16BitAccess`.
- Downloading the SDK needs the user's approval. The plan lists the files.

## Wrapper: `VulkanFsrUpscaler`

The viewer never calls AMD's API directly.

```
create(VkContext&, displayW, displayH, renderW, renderH, flags) -> bool
resize(displayW, displayH, renderW, renderH) -> bool
dispatch(cmd, color, depth, motion, reactiveMask, jitter, frameTimeMs, exposure, reset) -> bool
jitterPhaseCount(renderW, displayW) -> int
jitterOffset(phase, count) -> (x, y)
destroy()
```

- `create` and `resize` return false on any SDK error; the caller falls back (see Fallbacks).
- The wrapper owns all SDK scratch memory, history and internal targets. VRAM bytes are added to a new owner `upscaler` in the VRAM log.
- Command recording uses the frame's existing command buffer. Barriers the SDK needs are added in the wrapper, and the existing validation-layer tests must show zero messages.
- The wrapper is called from the render thread (when it is enabled) and from the main thread otherwise, through the same entry points the other passes use.

## Settings

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `RenderFSR2Enabled` | bool | false | Use the temporal upscaler |
| `RenderFSR2Quality` | U32 | 1 | 0 Native AA (1.0x), 1 Quality (1.5x), 2 Balanced (1.7x), 3 Performance (2.0x), 4 Ultra Performance (3.0x). Ratios are AMD's divisors; render scale is 1/ratio |
| `RenderFSR2Sharpness` | F32 | 0.5 | Optional sharpening 0 to 1 |
| `RenderFSR2Dynamic` | bool | false | Reuses the existing FSR 1.0 dynamic scaling to adapt the render scale to frame time |

- `RenderFSR2Enabled` and `RenderFSREnabled` are mutually exclusive: setting one unticks the other.
- `getRenderScale()` returns the FSR 2 scale when it is on. `RenderResolutionDivisor` keeps its current priority.
- The temporal upscaler is unavailable on OpenGL; the checkbox is disabled with a tooltip there.
- Controls go in Preferences, Graphics, Upscaling tab, and in `graphic_preset_controls.xml`.

## Pipeline

All of this applies only while the temporal upscaler is active.

### Where it runs

At the **start of `renderFinalize`**, before tonemap. Input: the lit HDR scene (`mRT->screen`) and the scene depth, at render size. Output: a display-size HDR target `mFSR2Map`. Tonemap, gamma, glow, DoF and the rest then run on the display-size image.

- Cost: the post chain now runs at full size, which gives back part of the speed gain. This is measured (see Testing).
- FXAA and SMAA are skipped while the temporal upscaler is on, because it already does anti-aliasing. The preference UI greys them out.
- FSR 1.0's EASU and RCAS passes are not used in this mode. Optional sharpening is the SDK's own.

### Jitter

- Phase count: `ffxFsr3UpscalerGetJitterPhaseCount(renderW, displayW)`. Offsets: `ffxFsr3UpscalerGetJitterOffset`, a Halton [2,3] sequence in pixels, never (0, 0).
- Applied as a translation to the projection at `llviewercamera.cpp` (~385): X = 2·jx / renderW, Y = -2·jy / renderH; `jittered = translation × projection`. The same offsets are reported in the dispatch description.
- Not applied to: shadow maps, reflection probes, mirrors, snapshots, the GLTF material preview, and the UI.
- Consumers of the projection (SSR, SSAO, fog, depth-based effects) get the **unjittered** matrix unless a test shows they are better with the jittered one. The plan includes a per-effect shimmer check.

### Motion vectors (camera only)

A full-screen pass reads depth, reconstructs world position with the current unjittered inverse view-projection, reprojects with the previous frame's unjittered view-projection, and writes 2×FLOAT screen-space motion in pixels (`motionVectorScale` = render width and height). The previous view-projection is stored per frame; it is reset with the history.

Known limit: moving avatars and objects get camera motion only, so they ghost. The reactive mask reduces it.

### Reactive and transparency masks

An R8 reactive mask at render size, clamped to 0.9, marks alpha-blended surfaces, particles, glow, water and avatars. Where it is written is part of the plan: the first version writes it in a pass from the existing deferred buffers where possible, and from forward-pass alpha only where it is not. If the SDK's transparency-and-composition mask is not needed in testing it is not added.

### Other inputs

- **Depth:** the viewer's depth convention is fixed by `LLPipeline`. The plan records it and sets the SDK's inverted or infinite flags to match; a wrong flag is found by the motion-vector test.
- **Exposure:** auto-exposure on. The viewer's own exposure map is not fed in the first version.
- **Frame time delta:** milliseconds from the viewer's frame timer, at least 1.0.
- **Texture LOD bias:** `log2(renderW/displayW) - 1` as in AMD's guidance (1.5x gives -1.58, 2.0x gives -2.0), applied through the viewer's existing texture bias path, and restored when the mode is off.

### History reset

`reset = true` on: teleport finished (`LLAgent::handleTeleportFinished` / `update_tp_display` reaching `TELEPORT_NONE`), a camera jump (teleport, camera preset, `Focus` snaps), window resize, render-scale change, toggling the setting, and the first frame after the backend loses and recreates its swapchain.

## Preferences UI

Upscaling tab: "Temporal upscaler (FSR 2/3)" checkbox, quality combo, sharpness slider, dynamic checkbox, plus a render size line like FSR 1.0's. Selecting it unticks FSR 1.0. Labels go in `en/strings.xml`.

## Fallbacks and errors

Chain: **temporal upscaler → FSR 1.0 → bilinear.**

- `create` or `resize` fails, a shader fails to load, or a Vulkan feature is missing: unticks `RenderFSR2Enabled`, logs one warning with the reason, shows the same failure message FSR 1.0 uses, and turns FSR 1.0 on at the matching scale.
- Out of device memory while creating history targets: same path. The existing buffer spill and VRAM budget logic applies unchanged.
- A dispatch error at run time: the frame uses the unfiltered render-size image stretched bilinearly, and after three consecutive errors the feature unticks itself.
- OpenGL renderer: the setting is hidden or disabled; nothing in the OpenGL path changes.

## Testing

1. **`llvulkan_fsr2_test`** (new, headless context, validation on): synthetic moving scene; output is display-size and not blank; a `reset` frame clears history; zero validation messages including teardown.
2. **Jitter test:** offsets repeat after the phase count, never (0, 0); the unjittered matrix is what reprojection uses; static camera gives zero motion.
3. **Fallback test:** a test hook (like `VkContext::failMemoryTypes`) forces `create` to fail; the setting unticks, a warning is logged, FSR 1.0 takes over.
4. **Regression:** `llvulkan_fsr_pass_test`, frames, rgb10, activation tests (render thread on and off) with the feature off.
5. **In-world, run by the user** (the assistant never launches the viewer): sharpness in a static scene vs FSR 1.0; avatar ghosting; teleport and camera-cut reset; resize; every mode; shimmer in SSR/SSAO/fog; 40 minutes of VRAM stability with the `upscaler` owner line.
6. **Performance gate:** record frame time with FSR 1.0 Quality, temporal Quality, and native, in the same scene. Gate: temporal Quality must be at least 15 percent faster than native resolution in the same scene (proposed; the user sets the number in review). Otherwise it ships off by default and the notes say why.

## Risks

1. The vendored v1.1.4 SDK is frozen; no upstream Vulkan fixes.
2. Camera-only vectors: avatar and object ghosting.
3. Full-size post chain eats part of the gain.
4. Jitter may make projection-reading effects shimmer.
5. Build time and repo size grow; the vendored file list is kept minimal.
6. FP16 shaders need device features; the FP32 path must be tested.

## Out of scope

FSR 4 and ML upscalers; per-object motion vectors; OpenGL support; replacing FSR 1.0; feeding the viewer's own exposure into the SDK; frame generation (separate spec).
