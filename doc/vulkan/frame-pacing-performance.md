# Vulkan frame pacing optimization

The updated viewer is built. Use **Run-Firestorm-Vulkan.cmd** from this folder and retest the same scene where you
saw frame skipping. The agent has not launched the viewer. Your prior report confirms activation worked with visible
frame skipping; it does not supply measured FPS or a complete visual-parity check.

Executable: `C:\phoenix-firestorm\build-vc170-64\newview\Release\firestorm-bin.exe`  
Alias: `C:\phoenix-firestorm\build-vc170-64\newview\Release\FirestormFSdev.exe`  
Built: 2026-10-12T19:50:03-04:00  
SHA256: `845f9fb5669c46fba3d499bcb1f71f8804bf14a9ff8d47ed37fa8d44e9f3d35a`  
Your tested activation EXE is preserved as `C:\phoenix-firestorm\build-vc170-64\FirestormFSdev.pre-pacing-optimization.exe` (same original activation hash).

Changes:

- Texture uploads, image clears, mip generation and downscale copies record into the active frame command buffer
  instead of draining the queue and waiting for a separate submission. Staging buffers and replaced images/views
  survive until the frame fence completes. CPU texture readbacks/snapshots retain their required blocking completion.
- Vertex/index mutation uses another backing allocation when earlier draws still reference the old one. Completed
  backing versions are reused, unchanged bytes are preserved, and cached CPU upload shadows avoid reading slow
  GPU-visible memory. Extra retained Vulkan allocation sizes are capped at 64 MiB; the cap fallback safely fences reuse.
- Normal viewer windows enable Vulkan validation only for the existing graphics debug mode. Native integration tests
  explicitly retain validation. Run **Run-Firestorm-Vulkan-Validation.cmd** for an SDK-backed debug session; it sets the
  existing one-shot RenderDebugGLSession setting. The normal path also passed with no SDK layer search path available.
- Viewer logs now contain a **VulkanPacing** summary every 300 presented frames: recording average/max, fence/acquire/
  present waits, blocking submissions, buffer versions and pipeline creation cost. These are CPU wall-clock metrics,
  not GPU timer-query results. The first interval can include startup resource creation in its cumulative counters.

Measured on this AMD RX 6700 XT with a headless offscreen framebuffer, five warm-up frames, 24 measured frames per run,
three independent trials per workload, no validation for timing, and an explicit frame-fence wait in both versions.
Streaming: 256 update/draw operations per frame. Textures: eight 128x128 RGBA update/draw operations per frame.
The table reports the median of each trial's median/p95 measurements. It is **not an in-world FPS claim**.

| Workload | Before median / p95 | After median / p95 | Blocking partial submissions, 24 frames |
|---|---:|---:|---:|
| stream | 46.80 / 57.42 ms | 1.38 / 2.18 ms | 6120 -> 0 |
| textures | 10.87 / 12.95 ms | 6.99 / 8.63 ms | 384 -> 0 |

The isolated texture-batching attempt reached about 8.68 ms median before buffer pooling was added; it removed all
384 partial blocking submissions from its 24 measured frames. Both attempts are retained: measured improvements
exceed the observed trial variation, while pixel and validation guards stay green. GPU/CPU clock and load variation
remain possible; actual viewer benefit must be confirmed by your same-scene retest.

Validation:

- Release viewer: BUILD_EXIT=0, zero compiler/linker errors, fresh EXE and matching alias hash.
- Both pacing workloads: three timing runs and a validated run; preserved early/late draw pixels, partial updates,
  memory-cap fallback, and zero unexpected partial blocking submissions in the ordinary measured frame workloads.
- Native frontend: 29 validated checks; 8 OpenGL and 8 forced-fallback checks. Additional native runs prove the
  300-frame pacing log (30 checks) and operation without SDK validation layers (29 checks).
- Broad GPU suite: 123 headless / 144 windowed checks; zero Vulkan validation errors/warnings.
- Shader corpus: 210/210 programs link/reflect/cache-roundtrip; 424/424 stages compile/validate, zero layout mismatches.
- Startup selector/factory suite: 12 tests / 276 checks. Whitespace checks pass. No commit/push/merge.

If skipping remains, preserve the viewer log containing VulkanPacing lines after several hundred frames in the
problem scene. The timings distinguish recording cost, GPU-fence/presentation waits and new pipeline compilation.
FIFO/single-frame pacing, cold pipeline builds, snapshot/readback waits, upload-ring exhaustion and memory-cap fallback
remain possible stall sources. Pooling retains an additional CPU upload shadow per buffer and up to 64 MiB of extra
Vulkan backing allocations. Live in-world smoothness/FPS was not checked. OPENSIM remains OFF.

The retirement/synchronization rules follow the [Vulkan synchronization guide](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
and [Khronos wait-idle performance sample](https://github.khronos.org/Vulkan-Site/samples/latest/samples/performance/wait_idle/README.html).
Regression target: llvulkan_frame_pacing_test [stream|textures] [--validate], EXCLUDE_FROM_ALL; native logger probe:
viewer_vulkan_activation_test --pacing. Pixel checks and zero-partial-submit assertions are the portable gate; absolute
timing values are reference measurements on this machine, not portable CI thresholds.
