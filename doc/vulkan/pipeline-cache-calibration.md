# Pipeline cache, stall logs and native calibration

Built viewer: `C:\phoenix-firestorm\build-vc170-64\newview\Release\firestorm-bin.exe`  
Alias: `C:\phoenix-firestorm\build-vc170-64\newview\Release\FirestormFSdev.exe`  
Build: 2026-10-13T00:27:34.058552-04:00, FSdev 7.2.5.81822, OPENSIM OFF.  
SHA256: `93412f964af1a166592af5d6d56897469dcfdc74c133694e9ae11d4957038356`  
Previous executable: `C:\phoenix-firestorm\build-vc170-64\FirestormFSdev.pre-pipeline-calibration.exe`.

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


## Test the viewer

1. Use `Run-Firestorm-Vulkan.cmd` in this folder. Normal runs keep validation off.
2. Visit the same heavy sim and test camera movement, hidden/reappearing objects, water/reflections, snapshots and resize/minimize.
3. Exit normally and restart. With RenderShaderCacheEnabled on, driver data saves beside the existing shader cache as `vulkan-pipelines.bin`.
4. After at least 300 frames, run `Analyze-Vulkan-Timings.ps1`. Its optional `-LogPath` accepts another log location.
   It shows only renderer timings. Later intervals from the same scene are most useful; the first contains startup work.
5. Compare pipeline-build time/cache hits and CPU recording against GPU time and blocking categories. No scheduling or
   asynchronous readback change was made without an in-world trace.

## Evidence

- `pipeline-cache-results.json` and `cache-*.log`: real cache hits, cold/warm costs and 21 integrity/persistence scenarios.
- `cache-native-*.log`: frontend cache lifetime, GL/fallback compatibility and reason-specific pacing logs.
- `calibration-results.json`, `calibration-vulkan.log`, `calibration-normal.log`, `calibration-gl.log`: real production workload checks.
- `query-validation.log`, `query-release.log`: 28 checks each, including zero wait metrics for availability polling.
- `pacing-cache-calibration*.log`: pixels, ordinary nonblocking paths, explicit readback/buffer reuse attribution and memory cap.
- `activation-headless-regressions.log`, `activation-windowed-regressions.log`: fresh 123/144 broad gates.
- `cache-calibration-build.log` and `cache-calibration-build-manifest.json`: full Release build and matching aliases.

Implementation follows [Khronos pipeline cache contracts](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateInfo.html)
and [cache export rules](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html).
GPU measurements use the [Vulkan query contract](https://docs.vulkan.org/spec/latest/chapters/queries.html).
