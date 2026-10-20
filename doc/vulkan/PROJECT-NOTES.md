# Vulkan project notes

## Current status: FSR 3.1 temporal upscaler, wrapper dispatches, 2026-10-19

- Spec `docs/superpowers/specs/2026-10-19-fsr-temporal-upscaler-design.md`. SDK vendored under `indra/llrender/vulkan/ffx/` (see its README); pure helpers in `llfsr2.h`.
- `VulkanFsrUpscaler` (`vkfsr2.h/.cpp`, in `llvulkan`): create/resize/dispatch/destroy, FP16 shaders when `VkContext::fp16Math`. Takes `Fsr2Image` (VkImage, format, size, current layout), not image views: the SDK makes its own views. After a dispatch the inputs and the output are in SHADER_READ_ONLY_OPTIMAL. VRAM in `VkContext::upscalerMemBytes` (20 MB at 640x360 to 960x540), test hook `VkContext::failFsr2Create`.
- Test `llvulkan_fsr2_test` (21 checks, `FSR2_WRAPPER PASS`): 8 jittered dispatches plus a reset frame, FP16 and FP32, output read back, zero validation messages.
- Fixed in the SDK: `rw_luma_history` was declared `rgba8` in GLSL for an RGBA16F image (validation warning on every dispatch).
- Next: viewer integration (renderFinalize, jitter, motion vectors, reactive mask, settings, fallback); add `upscaler=` to the VRAM-by-owner log line.

## Current status: VRAM growth fixed and instrumented, 2026-10-19

- Cause of the earlier `vkAllocateMemory` VkResult -2 crash: VRAM exhaustion (not FSR). 16 MiB slab blocks were never released.
- Slab release (`vkbackends.cpp`): a block with all slots free is freed, except the last spare block per slot size and memory type. Long session: VRAM 4.5 GB down to 2.7 GB. Pool test `POOL_CHECKS=71 PASS`.
- Spill (`vkbackends.cpp`): if a slab block fails in device memory it falls back to another host-visible coherent type. Throws only if every type fails. Tested with the `VkContext::failMemoryTypes` hook in the pool test (73 checks): preferred type fails, another type takes the block; all types fail, still throws. On the RX 6700 XT the preferred type is system RAM and the spill target is the 256 MiB BAR.
- Logging (every ~300 frames, `llrenderbackend_vulkan.cpp`): `VRAM used/budget MB` and `VRAM by owner MB: textures= render targets= host buffers= device buffers= upload staging=` (counts `vkAllocate` bytes only; a texture moves to render targets once a framebuffer attaches it).
- In-world check (39 min, one teleport, 11.5 GB budget): used 5.2-5.3 GB and flat for the last 13 min, textures plateau at 4.4-4.5 GB, render targets about 290 MB, host buffers about 800 MB, device buffers 65-160 MB churn. No crash, no VK_ERROR. The teleport rise is the new region loading, not a leak. The old ~0.5 GB / 40 min creep did not reproduce.
- Next: A later VRAM drop (5.3 to 2.8 GB, 22:30-22:33Z) is normal texture discard.

## Current status: FSR 1.0 upscaling built, checked in-world 2026-10-19 (user), 2026-10-18

- Spec `docs/superpowers/specs/2026-10-18-fsr-upscaling-design.md`, plan `docs/superpowers/plans/2026-10-18-fsr-upscaling.md`. Settings `RenderFSREnabled`, `RenderFSRQuality` (0-3: 0.77/0.67/0.59/0.50), `RenderFSRSharpness` (0 sharpest, 2 off); controls in Preferences, Graphics (Hardware tab) and in `graphic_preset_controls.xml`.
- `LLPipeline::getRenderScale()` (`llfsr.h`) replaces the plain multiplier; with FSR on, `renderFinalize` runs EASU (`fsrEasuF.glsl`) into `mFSRMap`, then RCAS + noise + depth (`fsrRcasF.glsl`) to the screen. Probes and mirrors keep full size. FSR shaders are only built while the setting is on; a compile failure unticks the setting.
- Tests: `fsr_scale_test` 9, `llvulkan_fsr_shader_test` 2, `llvulkan_fsr_pass_test` 9 (EASU ramp on the Vulkan backend, zero validation), frames 51, rgb10 41, activation 45 (render thread on and off).
- GOTCHA: the `firestorm-bin` build does NOT refresh `build-vc170-64/newview/Release/app_settings`, `skins` or `shaders`. After changing settings, shaders or XUI, copy them into `Release` by hand or the new exe fails on unknown settings. Done for the FSR files.
- Known gaps: high-res snapshots with FSR on render at the preset scale and are upscaled (softer); GL 4.0/4.1 and 3.x drivers cannot compile the FSR shaders (`packHalf2x16`, `textureGather` need 4.2/4.0) and the checkbox unticks itself with only a log warning; the shader files are about 3500 lines each against the loader's 4096-line limit.
- In-world check 2026-10-19: all passed (presets, live toggle, sharpness, resize, dynamic scaling, snapshot, Upscaling tab, failure message). Log: shaders loaded, no VK_ERROR/validation/crash.

## Current status: teleport stalls investigated from logs, 2026-10-18

- Teleports (Firestorm.log 04:51:23, Firestorm.old 22:24:58 22:40:40 23:03:00 00:16:14) each give 4-12 frames of 150-440 ms. FrameSpike splits them into `to Main:Display` (= `idle()`: 170-360 ms, region arrival and object/message work) and `to Display:Sky` (= `stateSort` + `rebuildPools`: 50-190 ms).
- Not the render thread: sync wait ~0 ms and throttle wait 1-5 ms per frame, blocking readback 2-28 calls and 10-60 ms total per 300 frames, blocking query 0 calls after login, GPU max 10-110 ms, no render-thread record over 50 ms. The earlier readback/query suspicion is cleared.
- Open: whether `idle()` and `stateSort`/`rebuildPools` cost more than on OpenGL. Needs a main-thread sample across a teleport: `python -I doc/vulkan/tools/mainprof.py <pid> 600 5` with `MAINPROF_PATH=stateSort`, then again with `MAINPROF_PATH=idle`.

## Current status: VRAM budget feeds texture discard bias, 2026-10-18

- Added `VK_EXT_memory_budget` (`VkContext::deviceLocalBudget`, `getRenderBackendVRAMBudget` in `llrenderbackend.h`). `LLViewerTexture::updateClass` raises `over_pct` when real device-local use passes 90% of the OS budget, so the discard bias rises before VRAM runs out; recovery needs use below ~81%.
- Built OK, activation test passes (render thread on and off). Not checked in-world. Also uncommitted: texture OOM fallback and framebuffer copy guard.
- Next: check in-world, then auto-fallback to OpenGL, degrade instead of abort in render-target alloc and `findMemoryType`.

## Current status: crowd hitches from VRAM vertex promotions, 2026-10-17
- Crowd run (Vulkan, render thread, ~40 avatars): focused FPS ~35-40; zero `blocking readback`/`query`/`upload` calls, so the teleport readback stalls did not show (no teleport in the run). Avatars got appearance within ~40 s of login (no region re-enable in the run).
- Hitches were the main thread waiting in `endRenderBackendFrame` -> `throttle(1)` (FrameSpike `to Main:Sleep`, 50-140 ms) while the render thread had 100-219 ms frames with no single record over 35 ms. Render-thread profile (`mainprof.py <pid> 1500 10 <VulkanRender tid>`): `VulkanVertexBufferBackend::promote` ~18% of busy time, inside the driver's `vkAllocateMemory` (`NtGdiDdDDICreateAllocation`, `WaitForSynchronizationObjectFromCpu`). Promotions came in bursts (~6000 per 300 frames), and ~60% were dropped again later.
- Fix: `resolve()` promotes at most `kPromotionsPerFrame` (16) buffers per frame (`vkbackends.h/.cpp`). Same spot after the fix, settled period (after the first ~30 s of loading): frames over 80 ms ~25/min -> ~2.5/min; endFrame waits over 50 ms only during login load; render-thread worst record 17 ms. `buffer_pool_test` 70/70, `frames_test` 51/51.
- FrameSpike labels: `to Display:Sky` is `stateSort` + `rebuildPools` (the ping sits before `updateSky`); `to Main:End` includes the 40 ms `BackgroundYieldTime` sleep while unfocused; `to Main:Sleep` is `endRenderBackendFrame`.
- `mainprof.py` takes `MAINPROF_PATH=<function substring>` to print the most common full stacks through that function.
- Avatar load delay (~2 min): our region sends `AvatarAppearance` for agents in the neighbour region before that region is enabled, so the avatars do not exist yet; `process_avatar_appearance` dropped the message ("sent for unknown avatar") and the sim resent it ~120 s later. In two runs every late avatar (15/15, 14/14) had its message dropped. Upstream drops it the same way. Fix: `LLVOAvatar::holdAppearanceForUnknownAvatar` copies the fields (`LLAvatarAppearanceFields`, held up to 60 s) and `applyHeldAppearance` runs them when the avatar arrives (`processUpdateMessage`, first name). Parsing goes through the copied fields for every message (`parseAppearanceMessage(fields, ...)`, `LLPrimitive::parseTEMessage(data, size, tec)`). Verified: 33 held messages applied 0-3 s after arrival; focused fresh login, all 35 avatars fully loaded 25-60 s after arrival (median 34 s). Unfocused logins look slow again (BackgroundYieldTime sleep + texture cache pause): measure focused.
- stateSort spikes when avatars finish loading (600-1800 ms): their meshes rebuild at once and each new vertex buffer storage was its own `vkAllocateMemory` on the render thread (11414 in 300 frames, render-thread frames up to 3.4 s). The 16 x 1 MiB render-thread queue filled and the main thread blocked in `rt::Thread::flush` under `stateSort` (`_unmapBuffer` -> `VertexProxy::bind`). Fix: `StorageSlabs` in `vkbackends.h/.cpp` carves storage up to 1 MiB from 16 MiB blocks in power-of-two slots (own `VkBuffer` per storage, bound at the slot offset; slots returned under a mutex, also from the reaper; blocks kept until shutdown). `buffer_pool_test` check: 1000 live small buffers 936 -> 1 allocations (71 checks). Same login after: worst frame 2247 -> 260 ms, stateSort worst 1787 -> 171 ms, frames over 300 ms 15 -> 0, main-thread condition waits 11% -> 2% of samples. Remaining stateSort (~10%) is geometry rebuild and avatar LOD work.
- `mainprof.py` with `MAINPROF_PATH` also prints what the matching function calls 1-3 levels down.
- Next: remaining spikes are main-thread `stateSort` (up to 300+ ms when avatars arrive) and idle (`to Main:Display`). If promotions still show in a profile, suballocate device copies from large blocks.

## Current status: frame stutter and far-avatar jitter, 2026-10-16 (evening)
- Restore crash 2026-10-17 01:31 (`FATAL: Vulkan framebuffer: framebuffer copies require an active frame`, right after "no longer backgrounded or minimized"): after a restore the first call of a frame can be a framebuffer copy or read; only draws and `clear()` started a frame. Now all four go through `VulkanFramebufferBackend::ensureFrame()` (keeps the bound framebuffer; while minimized a copy is skipped and a read returns zeros). `frames_test` reproduces the old abort and passes now (51 checks); all `llvulkan_*` tests and activation 45/45 pass.
- Minimize crash 2026-10-17 01:44: avatar render-time profiling (`LLVOAvatar::placeProfileQuery`, from impostor updates) begins GPU timer queries while minimized; `VulkanQueryBackend::begin` threw "Query issued while window unavailable" (then "Ending inactive Vulkan query"), the render thread rethrew on the main thread and the viewer crashed. Now `begin` uses `ensureFrame()` and, with no frame, registers the query with no GPU work so it reads 0. `query_test` minimizes a real window to reproduce it (failed with the exact message before, 31 checks pass now).
- Avatar load delay (one crowd run): avatars waited about 2 min for their first `AvatarAppearance` message, then rezzed about 10 s later. The burst followed a re-enable of the neighbour region (35.93.185.51, re-enabled about every 62 s). Not shown to be viewer-side; compare the same spot with stock Firestorm or OpenGL.
- Slow exit: a full exit took 23 s, 10 s of it in `LLVertexBuffer::cleanupClass` (the vertex backend freeing every buffer with its own `vkFreeMemory`); the earlier "stuck after Settings patched up" logs were most likely this, logged late. Now `LLVertexBuffer::cleanupClass` calls `setRenderBackendExiting()` when `LLApp::isExiting()`: with validation off, `VkContext::exiting` skips the per-buffer frees and `vkDestroyDevice`/`vkDestroyInstance`, leaving them to process teardown (pipeline cache still saved). That alone did not help: 4.9 s of the remaining time was buffer-pool evictions freed inline by `collectReleasedResources` during teardown. Pool evictions and trims now free on the reaper (also off the render thread at runtime), and `stopReaper` drops its queue when `exiting`. Measured: vertex teardown 4.9 s -> 24 ms, whole exit 23 s -> 8 s. `mainprof.py` takes `MAINPROF_ANY=1` to sample without the window in the foreground.

- Stutter root cause: deferred frees in `VkContext::beginFrame` -> `collectSlot` on the render thread. Each texture, VRAM vertex copy and staging buffer is its own `vkFreeMemory`, slow on the AMD driver. One crowd run: 1080 frame starts over 4 ms, 11.3 s of 15.8 s in releases (single frees up to 300 ms); fence wait median 0.01 ms. The main thread waited for it in `throttle(1)`.
- Fix (built, activation test 45/45 both thread modes, not yet tested in-world): `VkContext::deferFree` / `reap` run plain `vkDestroy*`/`vkFreeMemory` on a `VulkanReaper` thread once the frame is finished (texture images, texture staging, dropped VRAM vertex copies, upload batch chunks). Releases that touch pools (command pool, descriptor pool, buffer recycling, programs) stay on the render thread. `stopReaper()` in `shutdown()`.
- Diagnostics added: `FrameSpike` log (main-loop frames over 20 ms, per `pingMainloopTimeout` phase, `llappviewer.cpp`); `VulkanSlowRecord` log (render-thread records over 4 ms as module offsets plus beginFrame breakdown); `doc/vulkan/tools/symaddr.py <exe> <rva>...` resolves the offsets with the PDB.
- Far-avatar attachment jitter: caused by the distant-animation throttle (pose updated every 3rd/6th frame). User confirmed it stops with `FSDistantAvatarAnimPeriod` 1. Default changed 3 -> 1 (settings.xml and `llvoavatar.cpp`); parallel motion carries the load.
- Reaper verified in-world: release time in slow frame starts 11278 -> 28.5 ms; user reports less stutter.
- Texture upload stalls: `TEXSLOW` diagnostic (stderr, `VulkanTextureBackend::subImage2D`) showed conversion negligible; uploads blocked in quantized 10/20/32/52 ms waits. Cause: `uploadRingAlloc` on a full ring ends the frame and `vkQueueWaitIdle`s (`blocking upload` 27 calls, 434 ms in one run). Fix (built, activation 45/45, not yet tested in-world): `upload()` uses the ring only when `ringFits`, else one-off staging (`deferFree`). Viewer slices uploads into 8-32 rows (`sub_image_lines`), so one texture can hit this many times.
- Render thread now `THREAD_PRIORITY_ABOVE_NORMAL` (`vkrenderthread.h`); it did not remove the upload stalls (those were the ring), kept since the main thread waits on it.
- A 44-avatar crowd (13000 updates/300 frames) runs 15-40 FPS, main-thread idle (avatar update) dominant: next target after uploads.
- Ring fix verified: ring-full upload stalls gone. The remaining `blocking upload` stalls (5 calls, ~30 ms each) were the between-frames batch flush over its 128 MiB limit waiting with `vkQueueWaitIdle`.
- Batch flush no longer waits (built, not yet tested in-world): `VkContext::flushBatch` submits on its own fence (`fencedBatches`), released by `collectFencedBatches` (polled in `flushBatch`, `beginFrame`, `deferRelease`; waited in `collectReleasedResources`). Between-frames `deferRelease` with a fenced batch pending chains onto that batch's release. `batchLimit` is a member so tests can lower it. `upload_ring_test` updated (the ring-overflow check expected the old stall) and extended: 54 checks pass, zero validation. All `llvulkan_*` tests pass (`pipeline_cache_test` needs a cache path argument plus `--validate`); activation 45/45 both modes.
- Pass timing (`FS_VK_PASS_TIMING`) was on by default and cost about 5 ms/frame of render-thread query readback in a 44-avatar crowd (`queries` in the beginFrame breakdown). Now opt-in (`FS_VK_PASS_TIMING=1`). Activation 45/45 for both thread modes x pass timing on/off. Not yet tested in-world.
- Next: user runs the crowd with this build; check the beginFrame `queries` share and FPS.

## Current status: vertex data in VRAM, restore crash fix, 2026-10-16 (later)

- Big win: vertex and index buffers lived in the first HOST_VISIBLE|HOST_COHERENT memory type, which on the RX 6700 XT (no Resizable BAR; the device-local host-visible heap is only 256 MB) is system RAM. Every vertex fetch crossed PCIe. Skinned avatars cost about 4.9 us per 1K vertex shader invocations.
  - `VulkanVertexBufferBackend::promote`: a buffer drawn after 60 frames without an upload gets a DEVICE_LOCAL copy, recorded into the upload batch (submitted ahead of the frame) with a TRANSFER -> VERTEX_INPUT barrier; draws bind `Buf::drawBuffer()`. Any upload or free drops the copy (`dropDeviceCopy`, deferred release). Budget `kDeviceCap` 2 GB; allocation failure keeps the host copy. Host storage now has TRANSFER_SRC usage.
  - Result, 15+ avatar crowd: skinned shader 4.9 -> 0.7 us per 1K vertex invocations; G-buffer 9.0-9.3 -> 3.85 ms; GPU 12.3 -> 9.7 ms avg with more avatars in view; CPU recording unchanged (about 10.5 ms).
  - A 2-frame threshold made about 5 copies/frame that were dropped again by the next upload and doubled CPU recording time; 60 frames brought it to about 70 copies per 300 frames. Log: `VRAM buffer copies made/dropped (total)=... holding MB=...`.
  - Ceiling: one `vkAllocateMemory` per promoted buffer. Suballocate from large blocks if allocation count or time shows up.
- Pass timing: per-shader stretches for every full-size pass (`screen pass fb=... shader ...`), and for the G-buffer pass a pipeline statistics query per stretch (`Kverts/frame`, `Kvs/frame`, `Kfrags/frame`). Closed early if a viewer GL primitive query starts.
- Crash fix: `VulkanFramebufferBackend::clear` now begins the frame like a draw does (keeping the bound framebuffer; nothing while minimized). After a restore from minimized, a frame starting with a clear aborted with "draw outside frame or incomplete framebuffer". The two cases now have separate messages; incomplete also prints the framebuffer and status.
- Measurement gotcha: `BackgroundYieldTime` (40 ms) sleeps every frame while the viewer is not focused, so FPS read then is about 16-20. Only use focused samples.
- Lighting/post chain (main screen target, 5 passes, about 2.5 ms) is spread across Soften, point lights, alpha, MultiLight, Haze; nothing dominant.

- Done: `doAtmospherics` / `doWaterHaze` no longer copy the scene depth into `mWaterDis` on Vulkan (`Copy Depth Shader`, 4 passes, about 0.52 ms plus gaps). The haze shaders sample `mRT->deferredScreen` depth directly; the backend puts it in the read-only depth layout because the haze pass has depth test and writes off. GL keeps the copy (feedback loop there). Fog and water haze checked above and below water.

- Main-thread profiler: `doc/vulkan/tools/mainprof.py <pid> <samples> <interval_ms>` suspends the viewer's main thread, walks it with dbghelp (symbols from `firestorm-bin.pdb`) and prints inclusive and self shares. It only samples while the viewer is the foreground window. Run with `python -I`.
- Fixed: pass timing took `VulkanQueryBackend::mLock` on every draw and for the whole timestamp readback, so the main thread's reflection-probe occlusion polls (`getRenderQuery32` -> `available`) waited on it: 6.8% of main-thread samples. Pass timing state is recording-thread only (the main thread only calls generate/available/result, which use `mQueries`), so it no longer locks. After: not in the top 70, crowd FPS about 48 -> 52-58.
- Main thread in a 15-avatar crowd after the fix: display about 52%, idle 42% (avatar idleUpdate about 24%: skeleton, joints, motions, name tags), UI about 11%. No Vulkan-side item stands out.
- Tried and reverted: copying probe mips into the cube array with a one-subresource transition instead of the whole image. No change in the gaps (fb=67 0.10 ms); the gaps are dependent-pass drains. Rendering straight into cube layers would need per-subresource layout tracking; not worth about 0.3 ms.
- The disk pipeline cache already exists (`vulkan-pipelines.bin` in the shader cache folder, `vkpipelinecache.cpp`), with async compile on workers.
- Upstream `FirestormViewer/phoenix-firestorm` master merged again 2026-10-16 (4 commits, no conflicts).

Next steps: (1) main-thread avatar update cost (upstream code) if crowds matter most; (2) suballocate promoted vertex buffers if allocation cost shows up; (3) multithreaded recording only once the main thread stops being the limit (throttle wait several ms).

## Current status: masked draw order, barrier batching, 2026-10-16

GPU: AMD Radeon RX 6700 XT (no lazily allocated memory type, so transient attachments buy nothing; skipped).
- `Material Shader 2` (legacy alpha-mask material, diffuse only) was the largest G-buffer cost, up to 5-6 ms in a stadium view. The shader is one fetch plus `discard`; the cost was fill, mostly 8 large faces. `discard` stops early depth rejection, so masked draws now go after opaque ones:
  - `LLDrawPoolMaterials` runs its 24 deferred passes in `drawOrder` (opaque static, opaque rigged, then the 8 masked passes; `FIRST_MASKED_PASS = 16`).
  - `renderGeomDeferred` draws the materials pool's masked passes later, just before `POOL_GRASS` (the first masked pool, also where occlusion runs), so opaque PBR and terrain lay depth first. The pass loop and the sky-last block share one `render_passes` helper.
  - Same view measured about 5.2 -> 1.0 ms for that shader (view noise not fully excluded). The second step (after PBR/terrain) had no measurable effect in that scene; kept.
- `FS_VK_DRAW_TIMING=<program id>` (opt-in): one timestamp stretch per draw of that program in the G-buffer pass; the log lists its 12 slowest draws by order in the pass (`G-buffer draw #N of studied shader`).
- Texture layout barriers recorded back to back now go out as one `vkCmdPipelineBarrier` (`VulkanTextureBackend::beginBarrierBatch/flushBarriers`): the draw's sampler loop (`vkprogram.cpp`) and `beginRendering`'s attachment transitions. A second transition of an image already in the batch flushes first. Gap between passes in a busy scene about 1.32 -> 1.07 ms/frame (noisy). Activation test 45/45 both thread modes.
- `vulkan_fatal.log` (in the logs folder): the GUI viewer has no stderr, so the renderer reopens stderr there for the backend's `FATAL:` abort messages and calls `LLError::stopLoggingToStderr()` so the viewer log is not duplicated into it.

Next steps: (1) `fb=222` (5 full-size single-color passes, about 2 ms): identify and merge or halve; (2) fewer small passes (probe faces, 128x128); (3) depth pre-pass for masked geometry only if a scene still shows masked draws dominating with nothing opaque in front.

## Current status: main-thread trim, GPU pass timing, upstream merge, 2026-10-15

In-world about 170-185 FPS after the vsync fix (swapchain honors `RenderVSyncEnable`). Main-thread profile (800 samples, viewer focused) shows the main thread waiting on the render thread (`endRenderBackendFrame`, about 14%): in this scene the GPU, not the CPU, sets the frame rate.
- Done and pushed (`master` = `vulkan-framebuffer`):
  - `LLToolPie::handleHover` ran `pickImmediate` twice with identical arguments; the second now reuses `mHoverPick` (hover pick 6-7% -> 3.5%).
  - `LLVOAvatarSelf::idleUpdateAppearanceAnimation` wrote every wearable param each frame; now every 10th frame unless the appearance is morphing (editors call `writeToAvatar` themselves). 3.2% -> under 1%.
  - Threaded avatar motion jitter fixed: `LLMotionController::sampleTimer()` gives the worker the main-thread clock sample. `FSParallelAvatarMotion` is back to default on. Confirmed smooth in-world.
  - Upstream `FirestormViewer/phoenix-firestorm` master merged (8 commits, no conflicts).
  - `v0.1.0` pre-release republished from `e15ef02434` (zip made with `C:/Windows/System32/tar.exe -a`; Git's tar writes a non-zip). Not tested from a clean folder.
- Done and committed: per-render-pass GPU timing. `FS_VK_PASS_TIMING=1` writes two timestamps around each render pass; the 300-frame `VulkanPacing` log lists the 14 most expensive targets (`pass GPU fb=... ms/frame=... passes/frame=...`). Off by default. The begin timestamp is BOTTOM_OF_PIPE, so each log line also gives "gap before" (time since the previous pass ended: barriers, transitions, clears, idle); bodies plus gaps match the GPU frame time. Code: `VulkanQueryBackend::passBegin/passEnd`, read back in `beginFrame`. Activation test 45/45 with it on and off, both thread modes. Note `viewer_vulkan_activation_test` is its own target; build it separately or it is stale.
- Done (2026-10-15): per-shader timing inside the G-buffer pass (`G-buffer shader` log lines); pass timing is now on by default (`FS_VK_PASS_TIMING=0` disables).
  - Sky and clouds are drawn after the opaque pools (`renderGeomDeferred`, `LLDrawPoolWLSky::sDrawnLast`). Sky + cloud shaders went from about 1.1-1.7 ms to about 0.25 ms of the G-buffer pass.
  - Gotcha: sky drawn last needs both no depth write and no depth clear. `LLDrawPoolWLSky::endDeferredPass` clears depth; running it after the opaque pools wiped the depth (avatar semi-transparent, objects clipped). Stars are drawn before the moon in this mode because the moon no longer writes depth.
  - Fixed an upstream crash: `LLEnvironment::getCurrentSky/getCurrentWater` dereferenced null `mEnvironments[idx]` after a menu environment change.
  - Distant avatars' attachments follow the moving root on throttled frames (`LLVOAvatar::idleUpdate`).
- Findings (steady state, 1920x1017): GPU 4.0-4.2 ms/frame; render pass bodies 3.5-3.7 ms, so 0.5-2 ms is between passes. G-buffer pass about 250 draws, 2.25 ms (60% of pass time). Lighting/post chain about 0.75 ms in 5 passes, roughly fixed per pass (barely changed at half window size). Reflection probe passes about 0.15 ms total. Gap between passes: about 0.9-1.0 ms/frame (about 14% of GPU time) spread over about 55 passes, roughly 17 us each; largest are G-buffer start 0.13 ms and the 512x512 probe-face passes 0.10 ms. Batching the per-attachment barriers in `beginRendering` would save under 0.1 ms; fewer probe/small passes is the bigger lever. With the user's settings (CAS, DoF, SSR, shadows off) only tonemap, glow combine, FXAA and the final present are full-screen post passes, about 0.15 ms each.
- Looked at, nothing to gain: own-avatar `updateCharacter` (keyframe `std::map::lower_bound`, about 1% at best), reflection probe update (0.9-4% CPU, scene render), UI draw (13.6%, no hot leaf).

Next steps: (1) fewer small passes (probe faces, 128x128 targets), measured gap cost is about 1 ms; (2) G-buffer pass by shader program (needs per-program timing); (3) merging `combineGlow` into tonemap or FXAA into the present saves about 0.15 ms each (about 7% together), shader rewrites; (4) re-test the release zip from a clean folder.

## Current status: CPU latency work and render thread, 2026-10-14

Main-thread profiling of the heavy region (300 samples, viewer focused) drove this round. Done and built, all measured only in-world and only with indicative scene parity (draw counts differ run to run, 2,350 to 3,085):
- Avatar: throttled frames also skip joint LOD updates (`LLVOAvatar::updateLOD`, uses `mFarAnimSkipped`); second throttle tier `FSFarAvatarAnimPeriod` (6) beyond `FSFarAvatarAnimDistance` (25 m), first tier unchanged (period 3 beyond 10 m).
- Backend: framebuffer attachment view cached per texture (`Tex::aView`), per-unit bound texture pointer (`mUnitPtr`), and vertex/index buffer lookup by sequential id (`mById`) plus a per-binding pointer cache, which removed the per-draw `mBuffers.find`. Texture priority pass refreshes 2.5% of textures per frame instead of 5% (`updateImagesFetchTextures`).
- Measured: recording 22.6-23.5 ms -> 16.7-19.8 ms at 2,500-2,900 draws; GPU 14 -> 10-12 ms. `VulkanPacing` now also logs draws and render passes per target (framebuffer name, size, colors). G-buffer pass is about 60% of draws (1,500 of 2,500); no shadow passes in that scene.
- **Render thread (new, default on):** runs every real backend call on a second thread; `FS_VK_RENDER_THREAD=0` turns it off. Design: `render-thread-design.md`. Code: `indra/llrender/vulkan/vkrenderthread.h` (queue, no Vulkan), `indra/llrender/llrenderbackend_threaded.h` (proxies for the five backend interfaces), wiring in `llrenderbackend_vulkan.cpp`. Names (textures, framebuffers, vertex buffers, uniform buffers) are handed out by the proxy and checked on the render thread. `VulkanQueryBackend` has a recursive mutex. `VulkanTextureBackend::sourceBytes` and `VulkanVertexBufferBackend::allocateWithId` were added for the proxies. Verified: `tests/thread_queue_test.cpp` (queue, standalone), `viewer_vulkan_activation_test` 45/45 with and without the switch, zero validation messages. In-world: 85-95 FPS in the heavy region (OpenGL 45-48), 0 syncs per frame after the query-result fix; occlusion flicker fixed by reporting a query unavailable until its queued begin has run.
- `VulkanPacing` log gains "render thread per frame: records, KiB, syncs, sync wait ms, throttle wait ms" when the thread is on.

- Avatar animation threading: other residents' avatars queue their motion and skeleton update and run it on a worker pool after the object idle loop (`FSParallelAvatarMotion`, default on; self, animesh, loading and paused avatars stay inline). In-world fine; main thread motion+matrix cost 9% -> 4%. Remaining serial avatar cost: `updateRootPositionAndRotation` 5%, `updateSkinInfoMatrixPalette` 3%, animesh `matchVolumeTransform` 2%.

Next steps: (1) in-world check of distant avatars (10 m / 25 m tiers) and texture sharpening speed (revert the `/40` in `updateImagesFetchTextures` if slow); (2) main thread is now bound by scene work (avatar/joint updates, particles, sorting); (3) remaining ideas: `RenderAvatarMaxNonImpostors` 30 -> 15, fewer G-buffer draws, bindless/indirect draws.

## Current status: texture upload optimization, 2026-10-13

Native Vulkan is working in-world, but performance parity is unfinished. The user confirmed 16–17 FPS on the preceding binary versus 45–48 FPS in regular installed OpenGL Firestorm. Exact camera/settings/viewer-version parity was requested but not independently verified. Earlier 35–40 and 20–25 FPS reports preceded captured native activation and must not be treated as native measurements.

A fresh foreground profile of the preceding native viewer captured 200 valid main-thread stacks, all 200 while focused: 48 contained texture subImage2D, 31 upload, 36 deferred release collection, and 32 driver GPU-address frees. These are overlapping inclusive samples, not additive percentages. Evidence: outputs/live-cpu-profile-vulkan-gap.log in the session workspace.

Texture staging now shares the existing 16 MiB fenced uniform ring (TRANSFER_SRC usage, 16-byte texture offsets). Recorded uploads use the ring; overflow submits/waits before reuse and invalidates uniform snapshots via uniformGeneration. Inactive uploads use it only when frameFence is signaled and remain synchronous. Unsignaled inactive fences and larger payloads retain independent staging allocations. submitNow no longer scans/transitions the entire texture registry: each existing-image callback transitions its own image. Other framebuffer attachments keep their layouts and contents. Rendering quality/settings were unchanged.

Final GPU fixture: 46 checks, zero synchronization validation messages including teardown. Five-trial 15,360-update median 3536.0102 -> 126.8561 ms, staging allocations 15360 -> 0. With 10,000 registry entries, ring-only global-scan median 574.7687 -> 125.0978 ms after local transitions. These are component workloads, not overall FPS gains. Overflow pixel/uniform preservation, oversized/unsignaled fallback, completed idle reuse and attachment preservation all pass.

Final affected checks: texture/depth/sampler 41 with both unsafe-feedback negative fixtures retained; frontend 45 validated / 45 normal / GL 10 / fallback 10; query 28 validated / 28 normal; calibration 21 validated / 21 normal / GL 17; streaming and texture pacing validated + three normal trials each; skeleton 123 headless / 144 windowed. Passing suites report zero validation messages. Timing analyzer now displays texture staging alloc/ring and its privacy/rejection fixture passes. Release BUILD_EXIT=0 and diff check pass.

Installed build: 2026-10-13T14:51:14.649256-04:00; firestorm-bin.exe and FirestormFSdev.exe SHA256 `ac724d25e5c2493a8bba16632cd04c4fa1873a3f810271c6c1046aa4ada751ce`. Prior live-tested build is build-vc170-64/FirestormFSdev.pre-upload-ring.exe, SHA256 `b11734c8fd709f985b3ca4136b0a27605d09a57e57dddd9c7c6432cd5f247eaf`. The normal Vulkan CMD uses the new executable. No commit/push/merge. This upload build's in-world FPS/visual regression test is still pending.

Next: user manually launches the new build in the same heavy region with the viewer focused. Capture later settled VulkanPacing intervals and texture staging counters, then a fresh main-thread profile before choosing more optimization. Full native visual parity, cross-GPU coverage, two frames in flight and distributable dependency/installer packaging remain unfinished. See the self-contained session outputs/handoff.md for commands, evidence and ownership assumptions. Historical entries below describe earlier states and are superseded by this section.

## Latest: verified buffer reuse and follow-up descriptor cache optimization (2026-10-13)

- The 4 FPS regression was investigated in the user's running region. Initial 180-sample main-thread profile found 73 stacks in vertex-buffer allocation, 71 in createStorage and 68 in frame-start deferred resource collection. The completed-buffer cache addresses this by reusing only fence-completed storage, retaining at most 64 MiB, separating vertex/index kinds, and draining its owner callbacks before backend destruction. Rendering quality settings were not changed.
- Pool fixture: 70 checks, zero synchronization validation messages including teardown. Driver allocations fell 3842 -> 64 with 3778 reuses; five-trial geometry churn median 824.084 -> 20.169 ms (40.9x for that workload). Recorded red/green draws remain intact after free/allocate; the memory bound is verified. The user closed the old viewer and tested the installed pool build, reporting about 8 FPS versus 4 FPS before.
- A later settled 300-frame interval records CPU 48.938 ms (p95 61.603), GPU 14.177 ms, zero blocking partial submissions and zero pipeline builds; buffer storage 7 allocations / 10637 reuses / 0 evictions. The earlier interval was CPU 302.995 ms, GPU 21.413 ms. Scene/loading/focus differences mean these are live observations, not a controlled overall FPS multiplier.
- The settled 100-stack profile moved the hot path to descriptor preparation: 22 stacks in unitDescriptor/prepareDraw, 13 in samplerOf, 7 in viewOf, 16 including snprintf. Repeated textual cache-key formatting is now replaced by fully initialized exact binary keys, following the existing pipeline-key pattern. Float anisotropy uses its exact bits, and enum fields are explicitly converted; no padding bytes enter the key. Existing state-based sampler and view sharing remain intact.
- Paired descriptor workload (five medians, 10000 calls, 10000 registry entries): 13.654 -> 2.874 ms (4.8x for this operation). 41 texture/sampler/depth/lookup checks pass, including filter-state distinction/restoration, pixel guards, shader-binary reload, shared-depth transitions and synchronization validation. Both unsafe-feedback negative fixtures still abort. Native frontend 45 validated / 45 normal, GL 10 / fallback 10; query tests 28 validated / 28 normal. New buffer-storage live counters and timing-analyzer fixture pass. Release BUILD_EXIT=0; diff check passes.
- The profile includes background sleep; the existing viewer yields when another app has focus (BackgroundYieldTime default 40 ms). Foreground FPS should be used for the next comparison. No sleep/rate-cap preferences were changed. The WPR attempt lacked system profiling privilege, so process-only sampling was used and completed; WPR reports no recording.
- Final build 2026-10-13T11:12:00.972974-04:00; matching EXE aliases SHA256 `b11734c8fd709f985b3ca4136b0a27605d09a57e57dddd9c7c6432cd5f247eaf`. The pre-pool viewer remains build-vc170-64/FirestormFSdev.pre-buffer-pool.exe; the live-tested intermediate remains build-vc170-64/FirestormFSdev.pre-descriptor-keys.exe. The existing normal Vulkan CMD launches this final build. No commit/push/merge.

Next: the user tests the final binary-key build in the same region with the viewer focused. Pooling has a captured live benefit; the additional cache-key change has fixture validation, but its extra in-world FPS gain has not been measured. Profile remaining costs from later settled intervals rather than extrapolating workload speedups to total FPS.

## Latest: CPU texture lookup optimization after first in-world test (2026-10-13)

- User reports the native viewer now works in-world but achieves at most about 7 FPS. Captured VulkanPacing interval: CPU recording avg 394.148 ms, p95 797.59 ms; GPU avg 13.3462 ms, p95 18.007 ms; no partial blocking submissions/uploads/readbacks/query waits. The CPU interval includes a 31.4-second maximum and 7.69 seconds of pipeline compilation across 300 frames, so its average is not steady-state frame time. This is user-reported in-world operation, not complete visual parity validation.
- VulkanTextureBackend::unitDescriptor was scanning every texture to recover an ID after already resolving the bound texture. This added O(texture-count) CPU work for each sampler of every draw, including an entire failed scan for fallback textures. It now calls prepareSample with the already-resolved mUnitTex ID only for a valid matching binding. Image transitions, framebuffer feedback guards and fallback behavior are preserved; no rendering-quality setting changes.
- Paired real-GPU fixture, five-trial median of 10,000 descriptor calls with 10,000 registry objects: 867.018 -> 13.569 ms (63.9x for this operation). Fallback: 854.921 -> 13.628 ms (62.7x). Small table 13.697 -> 12.519 ms. This does not claim a corresponding viewer FPS multiplier.
- The registry-scaling guard failed before the fix and now passes. 39 format/sampler/depth/lookup checks pass with synchronization validation and zero errors/warnings. Both writable-depth and color-feedback negative fixtures remain fatal. Native frontend 45 validated / 45 normal; GL 10 / fallback 10. Full Release BUILD_EXIT=0; diff check passes.
- Build 2026-10-13T10:29:07.006058-04:00; matching EXE aliases SHA256 `7d3eb67cc482ff4d0539e18c7e19305e44a8b2bbd05ec803e26a7c80524976a3`. Previous binary retained as build-vc170-64/FirestormFSdev.pre-texture-lookup.exe. Existing normal Vulkan CMD launches the optimized viewer. No commit/push/merge.

Next: user reruns the normal Vulkan CMD in the same region/settings and reports FPS after loading settles. Inspect later VulkanPacing intervals, especially CPU p95, pipeline build time, and GPU time; further optimization must follow the remaining measured bottleneck. Actual post-change in-world FPS has not been measured.

## Latest: shared read-only depth attachment repair (2026-10-13)

- The user's next crash was -1073740791 / 0xC0000409 on the rebuilt viewer. PID 4580 dump and console show the material pass was passed; the abort now comes from LLPipeline::renderDeferredLighting sampling the current depth attachment. The preceding sampler fix did not complete native in-world compatibility.
- Read-only shared depth sampling now uses DEPTH_READ_ONLY_OPTIMAL for both attachment and descriptor, LOAD plus STORE_OP_NONE, and read access barriers. Each draw resolves its own sampled-depth state; depth clears and ordinary non-sampling draws resume writable attachment layout. The pipeline disables depth writes when depth testing is disabled, matching the existing GL behavior. Cached sampler descriptors are refreshed when image layout changes.
- Writable depth feedback and current color feedback remain fatal. No unsafe feedback guard was removed. This follows the Vulkan render-pass rules: https://docs.vulkan.org/spec/latest/chapters/renderpass.html
- The isolated fixture reproduced the exact old depth abort. Now 36 format/sampler/depth checks pass, including unchanged depth during repeated sampling, ordinary writes afterwards, clear/sample transitions, and same-frame descriptor reuse across attachment/non-attachment sampling. Explicit VK_LAYER_VALIDATE_SYNC=1 also passes with zero validation errors/warnings; both unsafe-feedback negative fixtures still abort as expected.
- Final regressions: 27 shader/cache checks, 28 validated / 28 normal query checks, 45 validated / 45 normal native frontend checks, 10 GL and 10 fallback. Release BUILD_EXIT=0; diff check passes. Matching EXE aliases SHA256 `e91e458649709b44ace5dbbc781bc7b8e2f95f8226832f51a9602a361467717d`; build 2026-10-13T02:18:50.137673-04:00. Previous binary: build-vc170-64/FirestormFSdev.pre-depth-sampling.exe. No commit/push/merge.

Next: user reruns the normal Vulkan CMD and tests live login/in-world rendering. The new fixture proves shared-depth pass compatibility, not a complete live world session. Continue diagnosing any further native integration failure from the matching dump before performance optimization.

## Latest: material-pass attachment feedback repair (2026-10-13)

- User reported another exit (1073740791); Windows Event 1000 identifies the actual exception as 0xC0000409. The matching PID 18480 dump and console identify an intentional abort: sampling a current color attachment creates feedback, from LLDrawPoolMaterials::renderDeferred through VkProgram::prepareDraw.
- The backend previously enumerated every declared sampler, including unused shared helper functions. That exposed inactive deferred-buffer samplers to the viewer and bound stale render targets during material draws. glslang linked-program active-uniform reflection now filters sampler metadata. Descriptor layout/writes retain original SPIR-V binding numbers even when filtered bindings leave gaps.
- Program-binary schema 3 and an active-samplers cache salt prevent reuse of old declared-only metadata. Cached binding validation permits ordered unique bounded gaps. The framebuffer feedback guard remains unchanged.
- Isolated real-GPU regression reproduced the exact abort before the fix. It now draws correct pixels with the current attachment bound only to an unused helper sampler, exposes only the active sampler, and preserves behavior after program-binary reload. Genuine active attachment feedback still aborts as expected. 27 format/sampler checks and 27 existing geometry/cache checks pass; zero validation errors/warnings. Frontend: 45 validated native / 45 normal / 10 GL / 10 fallback, retaining the worker-exit regression. Release BUILD_EXIT=0; diff check passes.
- Matching executable aliases SHA256 `0654be00a794d43c3ddd9ead7f505d4732388c3ff23e8d67762e980b7ff95fd4`; build 2026-10-13T02:06:33.526602-04:00. Previous binary retained as build-vc170-64/FirestormFSdev.pre-sampler-feedback.exe. Existing normal Vulkan CMD uses the rebuilt executable. No commit/push/merge.

Next: user manually reruns the normal Vulkan CMD and tests login and in-world material rendering. Fixtures prove the inactive-sampler error is repaired; no successful full in-world session or heavy-sim optimization is claimed. Diagnose any subsequent failure using its matching log/dump.

## Latest: login heap-corruption repair (2026-10-13)

- User login failed with -1073740940 / 0xC0000374. Windows Application Error and the matching 3888 crash dump confirm heap corruption.
- The symbolized dump shows a worker TLS LLRender destructor clearing the shared sVBCache, releasing Vulkan buffers through VkContext::deferRelease while the main thread uploads font textures. The cache and backends belong to the main graphics thread.
- LLRender::shutdown now clears the native shared UI cache only on_main_thread(); worker teardown keeps its local cleanup. This removes the unauthorized worker access instead of adding locks around shared Vulkan state.
- Regression on the real viewer frontend: populated UI cache, worker shutdown and TLS destructor, unchanged allocated buffer bytes, then successful pixel-checked render. It failed before the fix and passes afterwards: 45 validated native / 45 normal / 10 GL / 10 fallback; zero validation errors or warnings. Release BUILD_EXIT=0; diff check passes.
- Matching EXE aliases SHA256 `ce199b689550183b6310f1c10bb1e438403bc176b0153b9787e2342d424040be`; build 2026-10-13T01:23:49.377234-04:00. Previous binary retained as build-vc170-64/FirestormFSdev.pre-worker-exit.exe. The existing Vulkan CMD launches this rebuilt firestorm-bin.exe. No commit/push/merge.

Next: user reruns the normal Vulkan CMD and manually tests login and in-world rendering. The regression proves this worker-exit fault is fixed; no successful native in-world session or heavy-sim performance is claimed. Continue diagnosing any subsequent failure from its matching dump/log before optimizing.

The user reported that the Vulkan CMD did not appear to start the viewer. Logs proved that it selected Vulkan and
initialized the native window, then aborted during startup. Full application reproduction with auto-login disabled
found missing normal-buffer formats and an invalid post-presentation framebuffer clear.

- GL_RGB10_A2 now uses native A2B10G10R10_UNORM_PACK32 storage; GL_RGBA16 uses R16G16B16A16_UNORM. Normalized uploads
  and readbacks retain their actual precision. Both HDR and non-HDR deferred-normal attachment paths are supported.
- display_startup acquires a frame for each render call and skips the GL post-swap depth clear in native sessions;
  Vulkan clears depth on its next acquired frame. OpenGL retains its ordering; minimized acquisition still defers.
- The normal CMD waits for the process and captures console output in its adjacent Vulkan-startup.log. A failure
  displays the exit code and fatal/validation message instead of silently closing. Stub success/failure/env tests pass.
- Real GPU format tests: 19 checks, including float/byte conversion, quantization/clamping, attachment clear/readback
  and sampler channels. Actual frontend 43 validated native / 43 normal / 10 GL / 10 fallback; query tests 28 validated
  and 28 normal. All final passing Vulkan fixtures have zero validation messages. Full Release BUILD_EXIT=0.
- The actual viewer now reaches STATE_LOGIN_WAIT, owns a visible 1920x1017 client window and continues native frame
  presentation with validation enabled and zero fatal/errors/warnings. Auto-login was explicitly disabled; no login
  or in-world test was performed. This is the first captured full application startup proof, superseding earlier
  fixture-only activation claims. The user's earlier measured session was confirmed OpenGL from its live log.
- Build: 2026-10-13T00:56:34.021342-04:00; matching EXE aliases SHA256 `7cfbc5abe8cb44ab499f567921d3dc3b60b189535c5f79ca5f6dd7264474c4f6`. Prior binary:
  `build-vc170-64/FirestormFSdev.pre-startup-rgb10.exe`. FSdev 7.2.5.81822; OPENSIM OFF; no commit/push/merge.

Next: close the validation startup test and run the normal Vulkan CMD for user login and heavy-sim performance/culling
tests. Driver cache saves on normal exit. Inspect later VulkanPacing intervals; startup still incurs many blocking font
texture uploads (16,822 in the first captured interval) and should be optimized separately after runtime correctness.
Later login intervals had zero blocking calls. Native in-world visual parity/performance remains unverified.

## Latest: persistent pipelines, stall timings and startup calibration (2026-10-13)

- A real `VkPipelineCache` now feeds graphics pipeline creation. The existing shader-cache directory and preference
  control disk persistence; reload restores it when re-enabled. Second viewer instances load read-only. The cache
  envelope checks length, checksum, format, architecture, vendor/device, driver version and Vulkan cache UUID/header
  before passing data to the driver. Reads/exports are bounded at 64 MiB; per-process temporary files atomically replace
  complete data. Missing, corrupt, incompatible, locked and unwritable caches safely miss or retain the previous file.
- On RX 6700 XT, three paired 32-pipeline trials: median creation 22.21 -> 3.78 ms;
  all 32 warm pipelines report application-cache hits. Twenty-one integrity/persistence scenarios pass, including
  Unicode paths, disabled/re-enabled caching and read-only loading. Real viewer-front-end restart/persistence passes.
- VulkanPacing adds cache hits, p95 recording/fence/acquire/present/GPU durations, and maximum presentation waits.
  Blocking calls log count/total milliseconds by readback, query, uniform-ring, buffer reuse, upload and explicit wait.
  Availability polling stays asynchronous and records no blocking query calls. Recording includes these waits and
  pipeline creation; categories overlap. The first 300-frame interval includes startup work and GPU data lags one frame.
- Startup calibration now shares the original 1024x1024, 32-texture, 64-repeat workload between GL and Vulkan.
  Native measurements use GPU timestamp queries and the fixed byte budget, independently verifying corners/center
  against the source texture after each pass. Invalid pixels, missing timing, disabled/full profiling or minimized
  acquisition use the existing fallback. Frame ownership and cleanup are preserved. SkipBenchmark/NoHardwareProbe
  remain honored by the original feature-manager callers. Normal native capture: 190.81 GB/s,
  1.31 seconds startup workload; this is effective benchmark throughput, not peak physical VRAM bandwidth.
- Calibration: 21 validated native / 21 normal native / 17 OpenGL checks; incorrect-output rejection, borrowed frames,
  profiling exclusion, minimized recovery and later rendering pass. Query suite 28 validated / 28 normal; production
  frontend 37 native, 10 GL, 10 fallback, 38 pacing, 37 normal, with actual saved/reloaded cache data. Broad shared-runtime
  gates 123 headless / 144 windowed. Final passing Vulkan runs have zero validation errors/warnings.
- Ordinary stream/texture fixtures retain zero partial blocking submits, correct pixels and bounded-memory fallback.
  Three trials with five warm-up / 24 measured frames: stream median/p95 1.46/2.44 ms;
  textures 8.47/9.83 ms. No new in-world FPS or comparative stream/upload speedup is established.
- Release BUILD_EXIT=0, no compiler/linker errors, 2026-10-13T00:27:34.058552-04:00; aliases SHA256 `93412f964af1a166592af5d6d56897469dcfdc74c133694e9ae11d4957038356`.
  Viewer FSdev 7.2.5.81822, OPENSIM OFF, default OpenGL and explicit FS_RENDER_BACKEND=vulkan. Prior EXE:
  `build-vc170-64/FirestormFSdev.pre-pipeline-calibration.exe`. No commit/push/merge or viewer login.

Next: user runs the same heavy sim, camera movement, occlusion/reappearance, water/probes, snapshots and minimize/resize.
Exit normally to save the driver cache, then repeat to compare warm startup/scene hitches. Capture later VulkanPacing
intervals before choosing scheduling/multiple frames in flight or synchronous-readback changes. The installed release
log inspected this turn was OpenGL and contained no native frame timings; the small native fixture had 0.13 ms average
GPU work and 5.75 ms average fence wait, which is not evidence about the user's heavy-sim bottleneck.

Observed limit: long precise occlusion counts intermittently overcounted this calibration workload on this AMD setup;
the root cause is unconfirmed. Extra synchronization and an ordered device-reset experiment did not fix it; the reset
experiment was reverted. Calibration uses independently checked fixed-workload timestamps instead. Short precise and
boolean-query regressions pass; long-run profiler sample-count accuracy remains a separate investigation. Other GPUs,
in-world culling parity, calibration-driven graphics defaults and any steady-FPS improvement still need user/runtime evidence.

## Latest: native GPU timing and occlusion (2026-10-12)

The user reports consistent 35-40 FPS in heavy sims on the preceding pacing build. This is user-reported evidence.
They requested GPU timing and occlusion next; the new build still requires their in-world validation.

- `VulkanQueryBackend` owns pooled query slots and logical query names. Device-gated host resets prevent stale
  availability on reuse; slots/pages remain alive until their GPU fence and logical result references are finished.
  Names allocate no GPU storage until used; results are cached and slots reclaimed after consumption or discard.
- GPU timestamp results use the selected queue's timestamp valid bits and physical-device nanosecond period, including
  counter wrap. Whole-frame timestamps are read asynchronously after the existing frame fence. VulkanPacing now logs
  GPU avg/max ms alongside CPU recording/wait metrics; a span can include idle gaps between partial submissions.
- Shader and avatar timing use native queries. The full shader profiler also receives precise sample counts and actual
  hardware input/geometry-output primitive statistics. Active/runtime/pending state prevents ending unstarted queries,
  safely closes profiling on finish/reload, and avoids conflicting automatic avatar profiling while a full profile runs.
  Scoped timers sum completed GPU command batches and can finish after presentation or while no frame is recording.
- World/octree and reflection-probe occlusion now use native query results. Existing UseOcclusion preferences and
  hardware feature rules remain effective. Availability polls never submit/wait. A timed-out native world query keeps
  the group visible and reissues later. Depth clamp preserves far-plane water bounds; unsupported device features gate
  the relevant consumers conservatively. Scene-monitor sample queries use the bridge and defer during full profiling.
- Logical sample/boolean queries share physical occlusion segments when they overlap. Queries close/resume across
  target changes, rendering scopes, shader geometry-stage changes and synchronous partial submissions. Ordinary query
  begin/end preserves the current rendering scope, avoiding a render-target restart for every visibility test.
- Validation on RX 6700 XT: 26 native GPU checks with validation and 26 without; 37 production native frontend checks;
  10 GL and 10 fallback checks; 38-check pacing mode and 37-check no-SDK-layer mode. Broad regressions 123 headless / 144
  windowed. Shader corpus 210 programs / 424 stages; startup selector/factory 12 tests / 276 checks. Vulkan validation
  zero errors/warnings. Tests include reuse, unsubmitted availability, overlapping scopes, target switches, mixed geometry
  shaders, late lifetime/deletion, explicit reads, profile completion, minimized recovery, water bounds and GL timers.
- Pacing guards retain zero partial blocking submissions in ordinary stream/texture fixture frames. Three final trials,
  five warm-up / 24 measured frames, validation off with whole-frame GPU timing enabled: stream: median/p95 1.60/2.99 ms; textures: median/p95 9.93/12.50 ms.
  These synthetic results do not establish new in-world FPS, culling parity, or a performance improvement over the prior build.
- Final Release viewer BUILD_EXIT=0, no compiler/linker errors; 2026-10-12T23:27:48-04:00; matching executable aliases SHA256 `747a762fc8ae00dc67fba7dc65fb5095435802db68ea39f82aa2ff6aaaac458b`.
  Previous user-tested EXE: `build-vc170-64/FirestormFSdev.pre-gpu-queries.exe`. Viewer remains FSdev 7.2.5.81822,
  OPENSIM OFF, default OpenGL with explicit FS_RENDER_BACKEND=vulkan activation. No commit/push/merge or viewer login.

Next: user tests the same heavy sim, camera movement, hidden/reappearing objects, water, reflection probes, snapshots,
resize/minimize, and GPU profiling/autotuning. Compare GPU versus recording/fence/acquire/present logs before choosing
further optimization. Native startup bandwidth calibration still uses the hardware table; its benchmark is a separate
unfinished port. Pipeline disk caching, scheduling/multiple frames in flight and synchronous CPU readbacks remain.
Only the local AMD GPU was tested; unsupported feature gates were also checked by a focused capability simulation.

## Latest: measured Vulkan pacing optimization (2026-10-12)

The user ran the full activation build and reports that it works but skips frames. This is user-reported runtime
evidence, not an independently measured FPS/visual-parity result. No viewer launch/login was performed by the agent.

- Texture writes/clears/mips/downscale copies now batch in the recording frame instead of flushing and synchronously
  submitting each operation. Upload staging and replaced images/views are fence-retired; readbacks remain synchronous.
- Vertex/index updates use reusable physical backing versions while prior draws are live. Partial bytes persist via
  cached upload shadows (avoids slow CPU reads from GPU-visible memory). Extra retained Vulkan allocation sizes are
  limited to 64 MiB; overflow fences safely, and resource accounting/release includes all versions. CPU shadows add RAM.
- Normal native windows follow existing gDebugGL validation mode, rather than always enabling expensive validation.
  Integration tests explicitly set debug mode so their validation proof remains meaningful. A normal native test also
  passed with an unavailable SDK layer path. Optional validation launcher uses RenderDebugGLSession TRUE.
- VulkanPacing logs every 300 frames: record avg/max, fence/acquire/present waits, blocking submits, buffer versions,
  pipeline count/build time. Metrics are CPU wall-clock durations; initial cumulative counters can include startup work.
- Headless baseline vs final, 3 trials, five warm-up / 24 measured frames, identical budget and explicit frame-fence
  completion, validation off for timing: stream median/p95 **46.80/57.42 ->
  1.38/2.18 ms**; texture median/p95 **10.87/12.95 ->
  6.99/8.63 ms**. Partial blocking submissions **6120 -> 0** (stream) and
  **384 -> 0** (texture) per 24 measured frames. These are synthetic fixtures, not in-world FPS/smoothness results.
- All pixel/validation gates pass: early/late overwrite draws, texture ordering, partial uploads, bounded-memory
  fallback; ordinary fixture frames enforce zero partial blocking submissions. Native 29 + GL 8 + fallback 8 checks;
  pacing-log native 30 and no-layer native 29; broad 123 headless / 144 windowed; corpus 210 programs / 424 stages;
  selector/factory 12 tests / 276 checks. Vulkan validation zero errors/warnings. No test suppression or quality-bar reduction.
- Final viewer: BUILD_EXIT=0, no compiler/linker errors; 2026-10-12T19:50:03-04:00; matching aliases SHA256 `845f9fb5669c46fba3d499bcb1f71f8804bf14a9ff8d47ed37fa8d44e9f3d35a`.
  Tested activation EXE backed up as `build-vc170-64/FirestormFSdev.pre-pacing-optimization.exe`.
  Viewer version is 7.2.5.81822 (configured from repository cwd, avoiding transient revision-zero regeneration).

Next: user retests the same scene with the normal Vulkan launcher. If stutter remains, inspect VulkanPacing recording,
fence/acquire/present and pipeline metrics before making further changes. FIFO/single-frame scheduling, pipeline cold
builds, CPU readbacks, ring exhaustion and bounded-memory fallback remain. Do not claim all frame skipping is eliminated
from synthetic measurements. OPENSIM remains OFF; no commit/push/merge. See frame-pacing-performance.md and chat outputs.

## Latest: experimental full viewer activation (2026-10-12)

This section supersedes the activation guard/foundation notes below. The user explicitly requested full activation
and will test the viewer themselves. **Normal LLViewerWindow now selects the native Vulkan window when
FS_RENDER_BACKEND=vulkan.** OpenGL remains the default. Do not launch/login the viewer automatically.

- The immutable selector chooses Vulkan in runtime-enabled builds; Win32 installs the complete backend set before
  shader/resource creation, skips WGL, and initializes the capability view from actual Vulkan device limits/features,
  memory heaps, renderer and API version. Numeric GL/GLSL versions serve only the existing GLSL source assembler.
- WGL texture/media worker contexts are disabled for the native session without changing saved preferences.
  GL error probes, extension/debug queries, timer queries, occlusion and GPU timer benchmarking are bypassed where
  unavailable. Scene-monitor query comparisons are disabled. Native fences use a synchronous submit/wait bridge.
- ReflectionProbes, GLTFJoints, GLTFNodes and GLTFMaterials keep their caller-owned handles and std140 packing but
  now upload/bind through a common API. Vulkan stores CPU shadows and snapshots changes into a device-aligned upload
  ring. Set-2 layouts derive active bindings from validated SPIR-V, including cache-loaded programs; descriptors refer
  to immutable snapshots. Unbound blocks receive a zero buffer. Ring exhaustion submits/waits and safely rebinds data.
- Frame acquisition precedes idle rendering. Vulkan probe/snapshot updates run before final display because these
  passes reuse framebuffer zero; window swap requests defer until that final display is complete. OpenGL keeps its
  existing ordering. Minimized windows defer previews/downscaling and refuse snapshots until acquisition resumes.
- UI line loops close their last segment, including indexed draws. Draw calls that bypass LLVertexBuffer instance
  checks preserve configured arrays (the real texture-downscale caller supplies a screen-triangle buffer).
  Translator revision 3 writes a default one-pixel vertex point size; supported non-solid fill/large-point features
  are enabled. Native physics-debug draws use existing LLVertexBuffer helpers. Vertex AND index reuse waits before
  overwriting data referenced by recorded/submitted draws. Native capability shutdown precedes context deletion.
- Validation: **29 actual native frontend checks**, **8 OpenGL**, **8 forced-fallback**; **123 headless / 144 windowed**
  shared-runtime checks; zero Vulkan validation errors/warnings. Native checks cover all four named UBOs, same-frame
  overwrite, upload-ring exhaustion, readback after deferred swap, vertex/index streaming, UI loops/points, configured
  array draws, resize/minimize/restore and shutdown. Startup/factory suite: **12 tests / 276 checks**. Corpus:
  **210/210 programs** link/reflect/cache-roundtrip; **424/424 stages** compile/validate, zero layout mismatches.
- Final Release viewer build: BUILD_EXIT=0, no compiler/linker errors. EXE: 2026-10-12T16:05:17-04:00.
  firestorm-bin.exe and FirestormFSdev.exe SHA256: `60bae976188bb4b354cd529663c8cf12a89e7c11b08fb190f0fe1c61ae8b3b3c`.
  Previous alias preserved in `build-vc170-64/FirestormFSdev.pre-full-activation.exe`.
  User launchers and evidence are in this chat's outputs folder. No viewer launch, login/in-world proof or commits.

Limits: this activates the complete normal viewer render path for experimental manual testing; it does not prove
visual parity or performance. The user must check login UI, world, terrain, avatars, shadows/PBR/reflections, previews,
media, snapshots, resize and shutdown. GPU profiling/autotuning and query-based occlusion remain unavailable; FIFO,
single-sample targets and synchronous uploads/fences remain. Native fullscreen transitions/context replacement and
mid-session API fallback are not implemented. Validation layers/compiler package are local SDK/development resources;
no distributable installer or hosted dependency release was produced. **OPENSIM remains OFF.** Existing old reports
describe historical milestones, not the current activation state. Follow up on the user's runtime evidence rather
than treating this as a release-ready renderer.

## Latest: native activation foundation (2026-10-12)

- Native Vulkan activation is implemented and verified in the actual viewer window/rendering classes. A new
  `LL_WINDOW_VULKAN` flag opts LLWindowWin32 into an isolated native renderer session. It creates the HWND, initializes
  Vulkan and the complete backend set, then skips pixel-format/WGL creation. Shared OS event/drag/drop/timer setup is
  extracted without changing its GL ordering. The normal LLViewerWindow still passes flags=0 and remains OpenGL.
- `llrenderbackend_vulkan.cpp` owns the context, state, shaders, textures, framebuffers and an initially prepared vertex
  backend. Publication is atomic after successful initialization. Getters use this complete set; vertex ownership is
  transferred to LLVertexBuffer as before. Activating after any getter/vertex factory or a current GL context is rejected.
  Native-window initialization failure returns to the WGL path before Vulkan handles escape. Idempotent calls are allowed
  only for the same owning HWND; another window is rejected. Shutdown waits/releases the set before destroying its HWND.
- `beginRenderBackendFrame()` supplies acquisition/minimize handling; the native window's swapBuffers submits/presents.
  Resize keeps the HWND/surface and lets VkContext recreate window resources on acquisition. Fullscreen API switching is
  rejected, shared GL contexts are unavailable, and presentation remains FIFO. This is not a migrated LLAppViewer frame loop.
- LLRender::init skips GL-only initialization/query calls in an activated session. GL error helpers defer to Vulkan
  validation. Native LLRender shutdown clears its cached vertex buffers before backend/context teardown.
- Buffer uploads now submit/wait before overwriting a buffer referenced by earlier recorded draws, or wait for the
  submitted frame if recording has ended. Prior-frame data is safe after beginFrame's fence wait. This is a synchronous
  correctness bridge, not the asynchronous upload/performance milestone; indexed buffers use the same tracking.
- `viewer_vulkan_activation_test` links actual LLWindow/LLRender/LLVertexBuffer/LLGLSLShader and the viewer-built runtime,
  with no LLAppViewer/login/network startup. **21 native checks** pass: WGL is absent, complete-set getters, real LLRender
  drawing, presentation, resize, minimize/restore, streaming buffer preservation and shutdown. **8 GL default** and
  **8 forced-fallback** checks pass. The fallback deliberately publishes a GL shader object before requesting a Vulkan
  window, and checks working GL clear/readback. Native runs report zero Vulkan validation errors/warnings. Prototype
  callback/threadpool logging and an existing WGL-extension warning in GL mode are not Vulkan validation messages.
- Mutation: a separately compiled vkbackends object without the stream wait made the real streaming pixel check fail.
  Production runtime source was never changed; temporary target_sources was restored byte-for-byte and the normal native
  test rebuilt/retested. A stale early test after a compile failure was rejected; accepted final evidence requires a fresh
  executable and exactly 21 native checks. Shared regressions remain **123 headless / 144 windowed**, zero validation messages.
  The 12 startup/factory tests also passed after registry integration (their native accessors are inert mocks for GL testing).
- Final viewer build BUILD_EXIT=0, zero compiler/linker errors, exe updated 2026-10-12 15:33:20 local.
  FirestormFSdev.exe alias refreshed, hashes match firestorm-bin.exe; prior alias preserved in
  `build-vc170-64/FirestormFSdev.pre-native-activation.exe`. No viewer GUI/login/in-world run, commit/push/merge.

IMPORTANT: this is a tested activation foundation, **not a selectable full Vulkan viewer**. FS_RENDER_BACKEND=vulkan is
still guarded in the normal app. The native flag is used by the integration executable; the parsed startup request is
unchanged and `isVulkanRenderBackendActive()` is the authoritative installed-session state. Do not flip the main window
flag while GL capability setup, texture workers, named UBOs and post-display/snapshot rendering are still unported.
The caller must release client shader/render/vertex resources before closing its native window. No mid-session GL fallback
or native fullscreen recreation is implemented. Physical device-init failure cleanup uses the existing VkContext tests;
this phase's automatic window-fallback test injects a preexisting GL object, not a driver failure. OPENSIM remains OFF.

Next: replace startup capability queries with actual Vk limits/features, disable/replace WGL texture worker contexts,
port named uniform-buffer streaming/set-2 descriptors and remaining GL queries/fences, then migrate LLViewerWindow startup
and LLAppViewer's frame/snapshot ordering. Validate login UI before world/avatars/deferred/PBR and live in-world checks.
Design: `native-activation-design.md`; current evidence is in the chat outputs directory.

## Latest: shared shader compiler/package, geometry and cache (2026-10-12)

- Step 3 is implemented for the local Windows development build. LLShaderCompiler, std140 packing and
  VulkanShaderBackend now live in `indra/llrender/vulkan` and compile into the same llvulkan library for viewer and tests.
- A pinned glslang 16.4.0 / Vulkan SDK 1.4.357.0 **static** compiler package was created and installed using autobuild
  into `build-vc170-64/packages-vulkan`. Recipe/config/licenses are in `llrender/vulkan/third_party/glslang`.
  CMake uses LL_GLSLANG_ROOT, verifies all 83 file hashes, selects Release/Debug libs and rejects missing/corrupt packages.
  Viewer and skeleton no longer link shader compiler libraries directly from the SDK. The Vulkan API SDK remains separate.
- Upstream glslang, SPIRV-Tools and SPIRV-Headers notices are included in the archive and staged into the Windows viewer
  `LICENSES/glslang` directory through viewer_manifest. Probe dependencies contain no compiler DLL.
- Geometry modules are created/destroyed with the program, feature-gated and included in pipeline stage lists.
  The device enables supported geometry, clip/cull-distance and geometry-point-size features. Devices without geometry
  return a shader link/cache-load failure rather than creating an invalid module. All four captured geometry programs
  now link/reflect/cache-roundtrip. A synthetic geometry shader transforms vertices and reads a geometry-only uniform;
  compiled and cached versions give the same GPU pixels. This is not an in-world normal-debug visual comparison.
- Cache schema 2 records translator revision, glslang version, SPIRV-Tools build, Vulkan/SPIR-V targets and package
  fingerprint. LLShaderBackend exposes a cache salt for viewer shader hashes; empty GL salt preserves prior GL keys.
  Cache reads validate sizes, canonical uniform layouts, stage execution models and SPIR-V before driver allocation;
  failures leave an existing linked/bound program intact. Successful reload updates the active program pointer.
  VkProgram uses cleanup on partial creation failure, including the geometry module.
- Tests: 27 geometry/cache checks; **144 windowed** and **123 headless** checks; 27 more with depth-clip-control disabled;
  zero Vulkan validation errors/warnings. All **210/210** programs link/reflect/cache-roundtrip (no geometry skip), and
  translator corpus **424/424** stages compile/validate with zero layout mismatches. Startup guard: 12 tests/276 checks pass.
  A separate probe linked the actual viewer-built library and passed 27 checks. Standalone Debug probe also passed 27.
- Package-only translator configured, built and passed all 210/424 with VULKAN_SDK unset and SDK removed from PATH.
  This proves compiler-package independence, not independence of the Vulkan rendering API SDK.
- Release viewer build: BUILD_EXIT=0, zero compiler/linker errors; exe updated 2026-10-12 14:39:44 local.
  FirestormFSdev.exe alias refreshed; hashes match firestorm-bin.exe. Prior alias backed up as
  `build-vc170-64/FirestormFSdev.pre-shader-package.exe`. No viewer GUI/login/in-world test or commit/push/merge.

Limits: the package is installed **locally** using an isolated autobuild.xml; no hosted URL was published or invented
in the main manifest. A clean build must create/install the archive and set LL_GLSLANG_ROOT per the recipe README.
SDK static Debug libraries lack their compiler PDBs, so Debug linking emits LNK4099 and compiler-internal debug symbols
are unavailable; no linker warning was suppressed. The viewer cache is Release-only; Debug validation used the standalone
build. The production viewer still selects OpenGL, and OPENSIM remains OFF. Geometry linkage and synthetic GPU checks do
not establish all viewer rendering passes. Existing set-2 uniform-block streaming and other renderer API gaps remain.

Next: complete step 2 activation (atomic backend installation, WGL bypass/capability initialization, frame hooks and
failure fallback) together with step 4's remaining GL/API gaps, then integrate UI/world/avatars/deferred/PBR passes.
Do not remove the startup eligibility guard just because the shader library builds.
Design/reproduction: `shader-package-design.md`; artifacts/logs in the current chat outputs directory.

## Latest: startup selection guard (2026-10-12)

- Backend construction is centralized in `llrenderbackend.cpp`; all five backends resolve the same process-wide
  startup choice before any API object allocation. Vertex-buffer ownership/cleanup and lazy GL allocation are preserved.
- `LLAppViewer::init` resolves and logs the request before window creation. If a getter runs earlier, it resolves the
  same choice itself. There is no runtime switch or mid-session backend replacement.
- `FS_RENDER_BACKEND` accepts `opengl` and `vulkan` (case-sensitive). Missing/empty selects OpenGL.
  Invalid values fall back with a diagnostic. Vulkan requests fall back to OpenGL with a distinct reason for builds
  without the runtime and builds with the runtime but without complete viewer shader/render-path integration.
- This is a startup **guard and centralization**, not a selectable Vulkan viewer. No Vulkan device is initialized
  by the selector, and viewer Vulkan device-failure fallback is not yet exercised. Installing Vulkan shaders/backends
  as one complete set, bypassing WGL startup, frame hooks and replacing remaining direct GL calls remain activation work.
- 12 isolated tests / 276 checks passed with runtime enabled and disabled, including every possible first getter,
  request freezing, lazy allocation and vertex cleanup/recreation. Tests compile the actual factory bodies against inert
  API objects. Removing pre-allocation selection from a test fixture produced the expected failure; restored tests passed.
- Final viewer build: BUILD_EXIT=0, zero compiler/linker errors, exe updated 2026-10-12 14:15:27 local time.
  `FirestormFSdev.exe` was refreshed and hashes match `firestorm-bin.exe`. Prior alias saved as
  `build-vc170-64/FirestormFSdev.pre-startup-selector.exe`.
- No viewer GUI launched, no live login/render check in this step, no commits/pushes/merges. OPENSIM remains OFF.

Next: step 3 shader compiler/backend packaging, geometry support and shader-cache versioning; then finish step 2
activation with a complete backend set and integrate frame/render paths. Do not remove the eligibility guard to force
Vulkan while direct GL callers and GL shaders remain. Design and reproduction: `startup-selection-design.md`.

# Vulkan renderer for Firestorm: project notes

## Latest: Windows device/window runtime (2026-10-12)

- User confirmed the previous OpenGL framebuffer build's rendering and snapshots looked correct in-world.
- The user confirmed the desktop is unlocked and connected. Windowed Vulkan is working again; the earlier
  surface-capabilities VK_ERROR_UNKNOWN is no longer reproducible. Its specific cause was not established.
- Shared device/window/render-resource code now lives in `indra/llrender/vulkan`, target `llvulkan`.
  Skeleton and viewer build the same implementation. Shader compiler/backend stay in the test renderer until packaging.
  The std-only translator moved with the program resource code; both translator tests and skeleton use its new path.
- `LL_VULKAN_RUNTIME=ON` is enabled in this local viewer build. It defaults OFF for builds without the Windows SDK.
  It compiles and links the runtime; startup selection is still OpenGL. The existing HWND accessor supplies the window
  for the later selector. No viewer GUI was launched by the assistant and no commits/pushes/merges were made.
- Resize recreates swapchain views, depth, readback and per-image presentation semaphores together. Minimize skips
  acquisition without resetting the frame fence. Out-of-date/suboptimal status triggers recreation and event-loop retry.
- Buffers, image/views/memory and shader-program resources use deletion after frame-fence completion. This includes
  destruction requested while commands are being recorded, and requests between submission and fence completion.
- Context initialization failures are recoverable by a caller; noncopyable context destruction cleans up partial
  initialization. Shutdown is idempotent and guards allocations that have no live mapping.
- Tests: 21 window/lifetime checks; 117 total checks windowed; 96 headless; 206/206 supported shader programs link,
  reflect and round-trip; zero Vulkan validation errors and warnings. Translator remains 210/210 programs and 424/424 stages.
  A mutation that removed the fence-ready check failed the deterministic deletion test; restored code was rebuilt.
- Final full viewer build and the incremental rebuild passed: `BUILD_EXIT=0`, zero compiler/linker errors.
  `llvulkan.lib` was built from the final runtime source and is in the viewer link dependencies; the executable is newer
  than that library. Initial strict-compiler macro redefinitions/color-mask warning were fixed without relaxing checks.

This first runtime still uses one frame in flight. Uploads/readbacks block; asynchronous scheduling and multiple frames
remain later integration work. Swapchain recreation waits for the device; asynchronous presentation-fence recreation
is not implemented. CPU/GPU buffer-update scheduling still needs review when wiring real viewer drawing.

Next: startup backend selection before backend getters are first used (step 2).
The compiler package/geometry shaders and actual world/avatars/deferred/PBR passes remain later work. Do not advertise
the current executable as a working Vulkan viewer. Current native cache has `OPENSIM=OFF`, despite the older handoff's
ReleaseOS label; that setting was not intentionally changed in this step. Inspect/resolve that separately if OpenSim
support is required in the distributed build. Local revision was configured from the repository directory so Git
version detection works. Branch remains `vulkan-framebuffer`, with both milestones uncommitted.

Design and exact lifetime contracts: `window-device-design.md`.


**Current state and next steps are in `HANDOFF.md` (read that first); the sections below are a rolling log, their older "Status", "Vertex buffer" and "Next layer" parts are historical.**

Read this first in a new or compacted session. It records goals, state, decisions and the next steps.
Update the "Status" and "Next steps" sections whenever something changes.

## Goal

Build our own viewer based on Firestorm (OpenSim + Second Life) with a Vulkan renderer. OpenGL stays as the fallback
backend until Vulkan matches it. Reverse engineering the Firestorm binary is NOT needed: the full source is here.

## Environment (Windows, this machine)

| Thing | Where |
|---|---|
| Source (fork clone) | `C:\phoenix-firestorm` |
| Our fork | https://github.com/thehorseofcourse45/phoenix-firestorm (`origin`) |
| Firestorm upstream | https://github.com/FirestormViewer/phoenix-firestorm (`upstream`) |
| Build dir | `C:\phoenix-firestorm\build-vc170-64` (solution `SecondLife.sln`) |
| Build env launcher | `C:\phoenix-firestorm\fs-build-env.cmd` (untracked; run from plain `cmd.exe`, not PowerShell) |
| Toolchain | VS 2022 Build Tools (`E:\VS2022BuildTools`), CMake 4.4.4, Cygwin `C:\cygwin64` (patch), autobuild 3.10.3 in venv `C:\fs-build-venv` (made from Miniconda Python) |
| Build variables | `C:\fs-build-variables\variables` (env `AUTOBUILD_VARIABLES_FILE`; `AUTOBUILD_VSVER=170`) |
| Vulkan SDK | `C:\VulkanSDK\1.4.357.0` (already installed, on PATH) |
| Ghidra 12.1.4 + JDK 21 + REA | installed but NOT used. REA registration never finished (`rea setup`). Not needed. |

Build target: `ReleaseOS` (OpenSim, no Kakadu, no FMOD), channel `FSdev`.

Rebuild after editing (cmd.exe via `fs-build-env.cmd`, or from bash with the same PATH):

    cmake --build . --config Release --target firestorm-bin -- /m /v:minimal

Output: `build-vc170-64\newview\Release\firestorm-bin.exe`. `FirestormFSdev.exe` there is a COPY made by the packaging
step; refresh it with `cp -f firestorm-bin.exe FirestormFSdev.exe`. The packaging step (`llpackage`) fails at the end
because NSIS is not installed. That is harmless; ignore it. Run the exe from inside the `Release` folder.

### Gotchas learned

- Do not use the guide's `--chan`. Set the channel with `-DVIEWER_CHANNEL:STRING=FSdev`.
- The MSBuild viewer target is `firestorm-bin`, not `newview`.
- Writing files to the `C:\` root is blocked for the assistant; use the repo or the scratchpad.
- Background-task "completed (exit code 0)" notices only reflect the wrapper command. Check the log for the real result.
- Python: only Miniconda's 3.13 works. `C:\Users\TheTaZe\AppData\Local\Programs\Python\Python313` is broken and the
  StabilityMatrix pythons registered with `py` are dangling.
- `C:\fs-venv` is a broken leftover folder (could not be deleted by the tool); safe to delete by hand. Real venv: `C:\fs-build-venv`.
- Global git identity is empty. Commits by the assistant use `-c user.name=thehorseofcourse45 -c user.email=178369127+thehorseofcourse45@users.noreply.github.com`.

## Status

Current state: native GPU timing and occlusion are implemented and built; see the latest section above.
In-world confirmation of this build remains with the user. Startup bandwidth calibration remains deferred.

## Current continuation: 2026-10-12 framebuffer layer

Work is **uncommitted on local branch `vulkan-framebuffer`**, based on `8a8d32790f`.
Master and the remote have not been changed. The existing `.gitignore` and `.ignore` changes were preserved.

### Implemented

- `LLFramebufferBackend` and the GL implementation in `llrender`; framebuffer calls in `LLRenderTarget`, clears,
  readers and the raw framebuffer helpers now go through it. Only the window startup clears remain direct GL.
- Binding ownership moved from `LLRenderTarget::sCurFBO` to the backend. Temporary attachment binds save the actual
  draw framebuffer and restore it. `sCurResX/Y` remain render-target viewport bookkeeping, independent of the API.
- The interface exposes `checkStatus()` instead of a boolean so the existing specific framebuffer error messages survive.
  `attach(fb, ...)` requires `fb` to be bound, preserving the bind/attach/check/restore order and GL diagnostics.
- Standalone Vulkan framebuffer backend: lazy rendering scopes, first-clear load operations, bounded scissor clears,
  attachment-format pipeline keys, MRT, depth-only/shared depth, synchronous pixel readback, and the three texture-copy
  operations previously marked unsupported. Texture image layouts are tracked by the texture backend.
- Offscreen images use GL's bottom row as texture row zero; viewport and winding differ from the presentation target.
  Copying the presentation target flips rows and converts its BGRA format. Both directions are tested.
- Texture edits made during a frame submit pending recording before editing or replacing resources. Pipeline/descriptor
  binds happen after fallback texture resolution, since a fallback allocation can flush the recording.
- A `--headless` test path uses a real BGRA GPU image as framebuffer 0. It enables rendering tests without a Win32
  presentation surface; it does not verify acquisition or presentation.
- Regression tests in `indra/vulkan_skeleton/framebuffer_test.cpp`: 39 checks, including all seven sized viewer color
  formats, depth sampling, unsigned-integer snapshot depth, nested bindings, array-layer copies, shared depth, and clear
  state persistence. Test CLI rejects unknown scenes and nonpositive frame counts.

### Captured verification

- Full viewer build: `BUILD_EXIT=0`, zero `error C` / `error LNK`, `firestorm-bin.exe` updated at 11:28 local time.
  GL steps 1â€“3 were built together; there were no separate intermediate commits or builds.
- Final skeleton build succeeded. The existing C4805 bool/int warning in the pipeline color-mask expression remains.
- `vulkan_skeleton.exe --headless --test --dump C:\phoenix-firestorm\shaderdump`: **96/96 pixel/state checks passed**,
  39 of them framebuffer checks; 85 draws, nine pipelines, **0 validation errors, 0 warnings**, `RESULT: PASS`.
- `vulkan_skeleton.exe --headless --link-all --dump C:\phoenix-firestorm\shaderdump`: **206/206** programs link,
  reflect and round-trip their SPIR-V blobs; four geometry-stage programs remain unsupported. Zero validation messages.
- Existing `translator_test.exe C:\phoenix-firestorm\shaderdump`: self-check passed, **210/210** programs link,
  **424/424** stages compile and validate, zero uniform layout mismatches, `RESULT: PASS`.
- Mutation check: changing depth-clear clamping to always zero made the GPU depth-only/shared-depth tests fail.
  Source was restored, rebuilt and the full suite passed. This checks that the tests reject incorrect behavior.
- `git diff --check` passed. Viewer GUI was never launched; no login occurred.

### Display failure and verification limits

The windowed skeleton currently fails at `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` with `VK_ERROR_UNKNOWN` (-13).
An isolated build of the unchanged HEAD skeleton reproduces the same failure, with and without validation.
The cause is not established. A desktop lock/disconnection was asked about, but has not been confirmed.
Headless tests use the physical RX 6700 XT and the validation layer, but **windowed presentation remains unverified**.
One early invocation accidentally ran the previous executable before compilation completed and performed zero draws;
its PASS was explicitly rejected as evidence. Final evidence comes from the freshly built headless executable.

Other skeleton limits: one frame in flight; whole-image layout tracking; blocking uploads/readbacks; framebuffer copies
require an active frame and use a supported nearest blit for format conversion/orientation; overlapping self-copies and
compressed destinations are rejected. True 3D textures, stencil, front-buffer reads and multisampling are not added.
The current attachment textures and their framebuffers must outlive commands that use them. The implementation is still
outside the viewer's Vulkan path; it must not be enabled as a production viewer backend yet.

### Next steps â€” full integration is NOT complete

1. User must run the newly built OpenGL viewer in-world, per this handoff's launch restriction. Check terrain, avatars,
   UI, shadows/PBR, preview windows, avatar baking and snapshots. Earlier pipeline/shader/texture refactors also still
   lack this user runtime verification. Do not launch the viewer unattended.
2. Diagnose the windowed baseline's display-surface failure and rerun all scenes with the actual swapchain.
3. Design and implement the viewer window/device lifetime: backend choice before any backend getter is used, swapchain
   acquire/present/resize, synchronization, frames in flight, fence-based resource deletion, and remaining GL queries.
4. Package glslang for autobuild with its licenses, move the translator/compiler/backends into the viewer, support geometry
   stages and required device features, and version shader-cache keys by translator/compiler version.
5. Integrate and compare UI, world/terrain, avatars, water/sky, deferred/shadows/PBR and bake/snapshot paths against GL;
   address unsupported primitives/point size/stencil/polygon offset, then measure real-world performance.
6. Commit/PR/push/merge only when explicitly requested. The current worktree is intentionally uncommitted.

Commands for the next session:

    doc\vulkan\tools\build-skeleton.cmd
    build-vulkan-skeleton\Release\vulkan_skeleton.exe --headless --test --dump C:\phoenix-firestorm\shaderdump
    build-vulkan-skeleton\Release\vulkan_skeleton.exe --headless --link-all --dump C:\phoenix-firestorm\shaderdump
    build-vulkan-skeleton\Release\vulkan_skeleton.exe --test --dump C:\phoenix-firestorm\shaderdump

The last command still needs a working display surface. Test logs are saved in
`C:\Users\TheTaZe\Documents\Codex\2026-10-12\look\outputs`.

## Historical status

- Source cloned, fork created, remotes set (`origin` = fork, `upstream` = Firestorm).
- First build succeeded (baseline `FirestormFSdev.exe`, before any change).
- PR #1 merged into fork `master`: `RECEIVE_BUFFER_SIZE` 800000 -> 4000000 in `indra/llmessage/net.cpp:80`.
  Not measured in-world yet. Check the log line for the buffer size the OS granted (`net.cpp` getsockopt) and compare packet loss.
- GL call catalog done: `doc/vulkan/gl-call-catalog.md` (regenerate with `doc/vulkan/glcatalog.py`).
- Dead code removed: `renderFull4TU()`, `renderFull2TU()`, `renderSimple()` in `indra/newview/lldrawpoolterrain.cpp` plus their
  declarations in `lldrawpoolterrain.h` (they had no callers anywhere). See "Uncommitted work" below.

### Git state

- Dead-code removal is committed locally on branch `remove-dead-terrain-code` (commit `05fdecc6c9`), build verified (ReleaseOS, clean).
  Pushed to our fork and opened as PR #2 (https://github.com/thehorseofcourse45/phoenix-firestorm/pull/2), not merged yet at time of writing.
  The commit removes exactly 58 GL call sites. Run `git status` / `git log` / `git branch -vv` to confirm current state.
- `doc/vulkan/`, `CLAUDE.md` and `fs-build-env.cmd` are untracked (the latter two are in `.git/info/exclude`).

## Key findings (from the catalog)

- 784 GL call sites, 175 distinct functions, in 59 files. 431 are in `llrender/` (the abstraction layer); 353 are outside it.
- 126 `newview` files already use only the wrapper layer (`gGL`, `LLVertexBuffer`, `LLGLSLShader`, `LLRenderTarget`, `LLImageGL`).
  A new backend under `llrender/` serves them with no changes.
- Biggest direct-GL users outside `llrender/`: `pipeline.cpp` 56, `llviewerwindow.cpp` 43, `llviewerdisplay.cpp` 19,
  `gltfscenemanager.cpp` 17, `llspatialpartition.cpp` 16, `llreflectionmapmanager.cpp` 16.
- Biggest category: fixed pipeline state, 188 sites (blend/depth/cull/raster; `glPolygonMode` alone is 63). In Vulkan this becomes baked pipeline state.
- 224 GLSL shader files in `indra/newview/app_settings/shaders`; `llglslshader.cpp` injects `#define`s itself.
- Wire protocol (LLUDP, login) is fixed by servers. Only client-side changes are safe.

## Plan (agreed approach)

1. Profile first: is the viewer GPU-API-bound or CPU-bound (object updates, LOD, draw calls in `pipeline.cpp`)? Tracy zones exist.
2. Refactor behind a thin render-hardware interface with OpenGL as the only backend. No visual change; verify with screenshot diffs.
   Move the direct GL in `newview` (the migration list in the catalog) through `llrender/`.
3. Add Vulkan in layers: window + swapchain + 2D UI/fonts first, then terrain, objects, avatar skinning, water, sky, shadows, deferred/PBR.
4. Runtime switch; OpenGL default until Vulkan matches.

Chosen design: a THIN custom abstraction covering only what the viewer uses (not NVRHI/Diligent, not Zink).
Tools to reuse: Vulkan SDK (validation layers, glslang, spirv-cross), VMA, volk, RenderDoc. GLSL -> SPIR-V via glslang.

## Vertex buffer abstraction (in progress)

Design: `doc/vulkan/vertexbuffer-abstraction.md` (read it; has the 5-step plan, leaks list, proposed interface, risks).
Step 1 (delete the disabled `ENABLE_GL_WORK_QUEUE` code, 207 lines in `llvertexbuffer.cpp`): commit `6762d77086` on branch
`vb-remove-gl-workqueue`, build verified, pushed, PR #3 into our fork (https://github.com/thehorseofcourse45/phoenix-firestorm/pull/3), not merged at time of writing.
Check current state with `git branch -vv` and `gh pr list --repo thehorseofcourse45/phoenix-firestorm`.
Step 1 is merged (PR #3). The user APPROVED the design.
Step 2 (neutral `BufferKind` / `IndexType`, `invalidateBufferBinding()`): commit `c6cda69011` on branch `vb-neutral-types`, build verified,
pushed and MERGED via PR #4 (merge commit `999da40a2a`) into our fork's `master`. Check `git branch -vv`.
Step 3 (all GL calls behind `LLVertexBufferBackend`; new `llvertexbufferbackend.h` + `llvertexbuffer_gl.cpp`) plus a follow-up null-guard commit: MERGED via PR #5
(merge commit `13c7adf7e2`) into our fork's `master`. Full build verified before merge (the guard commit only got a `llrender` compile check). NOT run in-world: this is the riskiest step so far, so test
rendering (terrain, avatars, UI, model preview) and rebuild from `master` before trusting it. `LLVertexBuffer` now has zero GL references.
Step 4 (route remaining vertex-array state through the backend: dummy VAO via `initContextState()`, fullscreen-triangle draw via `drawArraysNoInput()`, `usesDeferredUpload()` in `LLRender::genBuffer`, deleted dead `vertexAttrib4f/4fv`): commit `3bc8cc63d5` on branch
`vb-step4-vertex-array-users`, full build verified (`BUILD_EXIT=0`), pushed and MERGED via PR #6 (merge commit `cfb954e510`) into our fork's `master`. master now holds steps 1-4; none of it has been run in-world yet. The original step 4 scope in the design doc was too broad: `llglslshader.cpp` already only used public LLVertexBuffer calls.
KNOWN LEFTOVER (not fixed, needs a visual test): `llspatialpartition.cpp` ~2601-2606 draws convex physics hulls with legacy `glVertexPointer` + client-side `glDrawElements` under a core-profile context, so it very likely draws nothing. Fix = draw with `gGL.begin(LLRender::TRIANGLES)`.
Step 5 (Vulkan skeleton) not started; the design says wait for window/swapchain, shader and pipeline-state layers.

## Next layer: fixed pipeline state (design approved, in progress)

TESTED BY THE USER: after steps 1-4 of the vertex buffer layer were merged and rebuilt from master (commit `cfb954e510`), the user ran the viewer and
reported it "works perfectly fine". So the vertex buffer layer is verified in-world.
Design: `doc/vulkan/pipeline-state-abstraction.md` (eager-setter state backend, GL behavior unchanged, Vulkan mapping verified against the SDK header).
The user APPROVED the design (including keeping GL capability constants as the state vocabulary).
Step 1 (add `LLRenderStateBackend` + `llrenderstatebackend_gl.cpp`; `LLGLState`/`LLGLDepthTest` call it for glEnable/glDisable/glIsEnabled/glDepthFunc/glDepthMask and depth queries; CPU-side caches stay in llgl.cpp): committed on local branch
`ps-state-backend`, incremental build verified (`BUILD_EXIT=0`), NOT pushed, no PR yet. Not run in-world. Check `git branch -vv`.
Step 2 (blend func, color mask, line width through the backend; scene monitor raw glColorMask now uses gGL.setColorMask, a small correctness change): second commit on the same branch `ps-state-backend`, incremental build verified (`BUILD_EXIT=0`), NOT pushed, no PR yet. Steps 1+2 would go in one PR.
Step 3 (raw state calls -> LLRender setters): DONE on the same branch `ps-state-backend`, one commit per kind, each built (`BUILD_EXIT=0`):
infrastructure (`fd48fb5878`: setViewport/Scissor/CullFace/PolygonMode/PolygonOffset/PointSize/Stencil* on LLRender + backend), scissor+stencil (7 sites), cull+point size (16), viewport (17), polygon offset (29), polygon mode (63).
No raw calls of those kinds remain outside `llrenderstatebackend_gl.cpp`. The new setters flush the immediate-mode batch first (the old raw calls did not).
Branch has 8 commits on top of master (steps 1-3), pushed and MERGED via PR #7 (merge commit `073f1b5a86`) into our fork's `master`. master now holds the vertex buffer layer (steps 1-4) and pipeline state steps 1-3. NOT run in-world since the pipeline state work (the user tested only the vertex buffer layer). `FirestormFSdev.exe` was refreshed from this build.
Still open in this layer: step 4 (check legacy `GL_CLIP_PLANE0`/`LLGLUserClipPlane` in llgl.cpp, llmaniprotate.cpp, fsmaniprotatejoint.cpp and `GL_ALPHA_TEST`; they may not work in a core profile), and the glClear/glClearStencil, framebuffer, texture and shader layers.
The user asked to continue with "step 5" after step 3: that means the vertex buffer plan's step 5 (Vulkan skeleton). The design says it needs the window/swapchain and shader layers designed first. Discuss with the user before starting.

## Vulkan skeleton (done, committed locally)

Branch `vk-skeleton` (pushed and MERGED via PR #8, merge commit `d6350a98ca`, into our fork's `master`): commit `82adc980cc` moves `BufferKind`/`IndexType`/`BufferHandle` into the dependency-free `llvertexbuffertypes.h`
(full viewer build verified, `BUILD_EXIT=0`), commit `479e23c8e6` adds the standalone program `indra/vulkan_skeleton/` (not referenced by the viewer build).
It draws one triangle on the user's AMD RX 6700 XT through real Vulkan implementations of `LLVertexBufferBackend` and `LLRenderStateBackend`; `--test` reads pixels back and passes
with the validation layer on and 0 errors/warnings. Findings and the gaps it exposed (no current-shader concept, no frame/render-target concept, GL clip depth -1..1 vs Vulkan 0..1, ...):
`doc/vulkan/vulkan-skeleton-findings.md`. Build dir `build-vulkan-skeleton/` is excluded from git. Build/run commands are in that doc.
Suggested next: design the shader layer and the framebuffer layer, extend the skeleton (textured, depth-tested, uniform-driven quad) before touching more viewer code.

## Shader layer (done through step 5: GL backend in the viewer, translator / compiler / Vulkan backend in the skeleton)

Branch `shader-layer` (local, NOT pushed, no PR), 2 commits on top of master: `a6b1676c4a` developer hook (env vars `FS_SHADER_DUMP_DIR` + `FS_SHADER_DUMP_EXIT`, build verified), `d91290d93f` probe tools in `indra/vulkan_skeleton/shader_probe/`.
RESULT: with the viewer's real shaders (484 objects, 210 programs, 424 stages) a source-to-source pass (preprocess + merge objects + #version + loose uniforms to blocks + drop legacy extensions)
makes **424/424 stages compile, 424/424 SPIR-V valid, 210/210 programs link across stages**. Nothing was run yet. 99% of stages are linked from 2-13 objects, so merging objects is the central problem.
Full findings, risks and the proposed design (LLShaderBackend, LLShaderTranslator, one block per program, depth via VK_EXT_depth_clip_control): `doc/vulkan/shader-layer-design.md`.
Decision pending for the user: glslang as an in-process library + SPIR-V cache (recommended) vs offline glslc.

APPROVED by the user 2026-10-12: glslang as an in-process library + SPIR-V cache.
Step 2 (LLShaderBackend): DONE in the working tree of branch `shader-layer`, NOT committed. New `llshaderbackend.h` + `llshaderbackend_gl.cpp`; `llglslshader.cpp`, `llshadermgr.cpp`, `llpostprocess.cpp`, `lldrawpoolmaterials.cpp` no longer call program/shader-object/uniform GL functions (only dead `#if DEBUG_SHADER_INCLUDES` code and the profiling queries remain). Build verified (`BUILD_EXIT=0`, 0 errors, exe 00:21). NOT run in-world. Failure-path log text for shader create/compile changed slightly.

Step 2 MERGED (PR #9, merge commit `33419de637`). Step 3 (translator): MERGED via PR #10 (commit `59fc3640aa` on branch `shader-translator`). `indra/vulkan_skeleton/translator/` = `llshadertranslator.{h,cpp}` (std-only text transform: merge objects per stage, #version 450, ONE std140 uniform block per program (`LLProgramUniforms`, set 0 binding 0), samplers `set 1 binding N`, existing blocks `set 2 binding <UniformBlock>`, vertex attribute locations from the reserved table; varyings left to glslang auto-map) + `translator_test.cpp` (preprocess each object with glslang, translate, compile, link, run spirv-val; built from the Vulkan SDK libs). RESULT on the 210-program corpus: 424/424 stages compile, 424/424 valid, 210/210 programs link, max 66 block members; whole run ~6 s. Build/run: see `indra/vulkan_skeleton/translator/CMakeLists.txt`. Not in the viewer build yet (moves into llrender when a viewer path calls it, step 5).

Step 4 (a real viewer shader on the GPU): MERGED via PR #11 (commit `974083b885` on branch `vk-step4-shader`). Skeleton now has a depth buffer, `VK_EXT_depth_clip_control`, textures (`vkprogram.{h,cpp}`), a per-draw uniform ring and a `ui` scene that draws the viewer's "UI Shader" (compiled from the dump by `LLShaderCompiler`). `vulkan_skeleton.exe --test --dump C:\phoenix-firestorm\shaderdump` = PASS, 0 validation errors; `--no-depth-clip-control` shows GL clip depth being clipped (expected control result). Translator change: explicit varying locations by name (glslang auto-map disagreed between stages; caught by the validation layer). Details: `vulkan-skeleton-findings.md`, section "Step 4". Next: step 5, `LLVulkanShaderBackend`.

Step 5 (Vulkan LLShaderBackend, in the skeleton): MERGED via PR #12 (commit `4174f014bd` on branch `vk-step5-shader-backend`). `vkshaderbackend.{h,cpp}`, `translator/llstd140pack.h`, `--link-all` mode: 206/206 non-geometry programs link + reflect + blob round trip on the GPU, UI shader pixel-exact from compile and from the blob, 0 validation errors; translator_test 210/210 incl. a packing unit test. Compile 31 ms/program cold, 0.6 ms from the blob. Details: `vulkan-skeleton-findings.md`, section "Step 5". Next: glslang 3p package + move translator/compiler/backend into llrender (see end of `shader-layer-design.md`), or start the texture layer.

Texture layer: survey done, design written, awaiting the user's approval (nothing coded): `doc/vulkan/texture-layer-design.md` (4 decisions: keep GL bound-texture model, no driver compression under Vulkan, no threaded uploads in the first Vulkan version, framebuffer-copy coupling). Steps 1-3 are GL-only refactors that touch every texture the viewer draws and need an in-world run by the user.

Texture layer step 1 (APPROVED design): `LLTextureBackend` (`lltexturebackend.h`, `lltexturebackend_gl.cpp`) + `LLTexUnit` (llrender.cpp) now has zero GL texture calls (activateUnit, bind, setWrap, setFilter, setMaxAnisotropy, initContextState). MERGED via PR #13 (commit `bc035c89dd`), full build verified (`BUILD_EXIT=0`, exe 09:01), NOT run in-world. Next: step 2 (LLImageGL uploads/readback/mips).

Texture layer step 2: LLImageGL (llimagegl.cpp) has no GL texture calls left except debug queries (viewport, texture binding), fence sync (sync layer), glGetError and glFlush. LLTextureBackend grew by 18 methods (names, pixel transfer state, image2D/compressedImage2D/subImage2D, mips, swizzle, level range, framebuffer copy, readback, residency, replaceWithMipLevel which owns the scratch PBO). Deviation from the design text: swap-bytes and row length stay STATE (setUnpackSwapBytes / setUnpackRowLength), not upload parameters, to keep setManualImage and the GL call order untouched. Branch `tex-step2-imagegl`, full build verified (`BUILD_EXIT=0`, 0 errors, exe 09:28), NOT committed, NOT run in-world. Next: step 3 (cube maps, render target mips, postprocess, raw calls in newview).

Texture layer step 3: every remaining texture GL call outside the backend is converted (cube maps, cube map arrays, render target mips, post process, lltexlayer, gltfscenemanager, pipeline shadow compare mode, reflection/hero probe managers, terrain paint map, scene monitor, bump pool, llappviewer crash test). Backend gained setMagFilter, setCompare, image3D, subImage3D, copyFromFramebuffer3D, copyImageFromFramebuffer2D. Only comments mention glTex*/glBindTexture now. Existing quirk kept as is: lltexlayer.cpp passes LLTexUnit::TT_TEXTURE (0) as the target of its level queries. Steps 2+3 MERGED via PR #14 (commits `14c64b5822`, `2c0b3900fa`), full build verified (`BUILD_EXIT=0`, 0 errors, exe 09:44), NOT run in-world. Still raw GL by design: glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS), debug queries, fence sync, glGetError. Next: Vulkan texture backend in the skeleton (step 4).

Texture layer step 4 (Vulkan LLTextureBackend in the skeleton): MERGED via PR #15 (commit `ec359e27c4`). `vktexturebackend.{h,cpp}`, `glconsts.h`, scene `textures` in main.cpp; `VkContext::unitTextures` and the old createTexture are gone (textures only through LLTextureBackend). `vulkan_skeleton.exe --test --dump C:\\phoenix-firestorm\\shaderdump` = PASS, 0 validation errors/warnings; `--link-all` 206/206. Details: `vulkan-skeleton-findings.md` section "Texture step 4". Next: framebuffer layer (LLRenderTarget, clears, read/draw buffers, attachment formats in the pipeline key), which also finishes copyFromFramebuffer*.

Framebuffer layer: survey done, design written and APPROVED by the user on 2026-10-12 (nothing coded yet): `doc/vulkan/framebuffer-layer-design.md` (~110 GL call sites in 19 files; 5 decisions: keep the GL state machine, clears ignore write masks under Vulkan with an assertion, setClearColor/setClearDepth on the backend, default framebuffer = window layer, layout tracking in the texture backend). Steps 1-3 are GL-only refactors of every renderer pass and need an in-world run by the user.

INCIDENT (read this): on 2026-10-11 launching the viewer to dump shaders led to the user's saved account logging in to Second Life for ~6 minutes (log evidence; whether the user clicked or it was auto-login is
unconfirmed); I force-killed it without a logout. The viewer must never be launched unattended; use `FS_SHADER_DUMP_EXIT=1` (quits before the login screen; verified: 0 login states in the log). The viewer exits
with code -1073741819 about 8 s after start in that mode; harmless.

## Next steps

Current implementation and evidence: `HANDOFF.md`, latest native GPU timing/occlusion section.
The user validated the OpenGL framebuffer work and reports consistent 35-40 FPS with the preceding Vulkan pacing build.
Now test native query-enabled culling and profiling in the same heavy sim, including camera movement, water, reflection
probes, snapshots, resize/minimize and shutdown. Use VulkanPacing GPU versus CPU/wait metrics to select the next measured
optimization. Native startup bandwidth calibration, pipeline disk caching, frame scheduling and synchronous readbacks
remain separate work. Preserve OPENSIM OFF and the uncommitted vulkan-framebuffer working tree; do not launch/login the viewer.

## Risks

- Large: months of work. Deferred/PBR/shadow passes are the long tail.
- Merge conflicts with upstream Firestorm because the refactor touches many files. Keep commits small; `git fetch upstream` and rebase often.
- Vulkan will not fix CPU-bound slowness by itself; step 1 of the plan decides whether this pays off.
- Third-party viewer policy: a unique channel name (`FSdev`), no Firestorm branding, LGPL source obligations.
- macOS would need MoltenVK; not a current target.
