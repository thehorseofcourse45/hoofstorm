# FSR 3 frame generation (Vulkan)

Date: 2026-10-19. Branch: vulkan-framebuffer. Status: draft for review, **gated**.
Depends on: `2026-10-19-fsr-temporal-upscaler-design.md` (must be built and verified in-world first).

## Goal

Raise the displayed frame rate by inserting an interpolated frame between each pair of rendered frames, on the Vulkan renderer, when the GPU is the limit.

This spec is deliberately shorter and less final than the upscaler spec. It fixes the interfaces and the order of work, and it starts with a spike that can end the project.

## Decisions already made

- Vulkan only. Frame generation needs queue and swapchain control that OpenGL cannot give.
- AMD FidelityFX SDK **v1.1.4** (last release with a Vulkan backend and the Vulkan frame-interpolation swapchain; MIT). Frame interpolation and optical flow sources are vendored under `indra/llrender/vulkan/ffx/` when this work starts.
- UI handling v1: **HUD-less color** texture (scene before the UI). AMD detects UI and composes it. Minor artifacts on semi-transparent UI are accepted. The present-callback option (UI drawn at present rate) is a later phase.
- Depends on the upscaler: motion vectors and depth come from it. Frame generation therefore requires the temporal upscaler to be active. (AMD allows frame generation without an upscaler; this project does not.)

## What the viewer lacks today

- **Queues:** `VkContext` has one `VkQueue queue` ([vkcontext.h:68](../../../indra/llrender/vulkan/vkcontext.h)) for graphics and present. The SDK's Vulkan swapchain wants four: game, async compute, present, image acquire.
- **Swapchain:** created at `vkcontext.cpp` ~508, acquired at ~666, presented at ~748. Frame generation replaces these with AMD's proxy swapchain.
- **Frame counter:** AMD requires the frame ID to increase by exactly 1 every presented frame, or its logic resets. The viewer's render thread submits frames, so the counter has one owner: the render thread.
- **Thread safety:** AMD states the frame-generation and swapchain contexts are not thread safe, and present must not run during prepare or destroy. All calls go through one mutex-guarded entry point.

## Phase 0: feasibility spike (decides whether this spec continues)

Throwaway code, labelled as such, on a branch or behind a build flag. Answers three questions:

1. Can the device be created with extra queues (async compute, present, acquire) on the user's GPU, and what does it do when the driver offers fewer?
2. Does AMD's proxy swapchain replace ours and present a test pattern at the window size without validation errors?
3. Does an interpolated frame appear (visible alternate frames) and what is the displayed frame pacing from the existing pacing log?

Exit criteria: all three answered in a short findings note. If any is a hard no, this spec stops there and the note says why. Nothing from the spike is kept unless a follow-up request says so.

## If the spike passes

### Device and queues

Request the extra queues at device creation when available. Fallback: **synchronous mode** (`allowAsyncWorkloads = false`), where optical flow and interpolation run on the game queue and use less memory. Async mode is an option only after profiling; AMD advises profiling first.

### Swapchain provider seam

One seam in `VkContext`: swapchain create, acquire, present and destroy go through a provider that is either the existing code or AMD's proxy. Toggling frame generation uses AMD's **passthrough** mode, not swapchain recreation, so there is no hitch. Teardown order follows AMD's: disable, clear the HUD-less color, destroy contexts, release the proxy, recreate the normal swapchain.

### Per-frame flow

1. Upscaler finishes (spec A).
2. Copy the scene before UI into the HUD-less color target (double-buffered when async, because it must persist until interpolation completes).
3. Frame generation prepare dispatch (frame ID matches configure).
4. Configure with the frame ID, HUD-less color, and reset flag.
5. Present through the proxy swapchain.

### Rules

- Reset on: teleport, camera cut, resize, mode toggle, present-mode change.
- Allowed present modes are decided after the spike; the SDK page the author read did not state a vsync rule. Until then, FIFO only.
- Latency: one extra frame. Accepted for a virtual-world viewer and documented in the tooltip.
- Only enabled while GPU time dominates CPU time: when measured GPU frame time is well under CPU frame time (the frame-pacing log already records both), the setting shows a warning because the generated frames cannot help a CPU-bound viewer. The threshold is set in review.

### Settings and UI

`RenderFSR3FrameGen` (bool, default false), enabled only when the temporal upscaler is on, the renderer is Vulkan, and the spike's capability checks passed. One checkbox in the Upscaling tab with a latency tooltip.

### Fallbacks

Chain: **frame generation → upscaler only.** Any create, configure or present error unticks the setting, logs one warning, and the swapchain returns to the normal provider.

## Testing (high level; detailed when the plan is written)

- Dual-queue device creation test with a forced "fewer queues" hook.
- Headless proxy-swapchain test (validation on) if the SDK allows a headless context; otherwise a windowed test the user runs.
- Frame-pacing check from the existing pacing log (present timings, fence waits).
- Regression: all existing Vulkan tests with frame generation off.
- In-world, user-run: UI artifacts on semi-transparent panels, teleport and resize, 40 minutes of VRAM stability, and a before and after frame-rate comparison.

## Risks

1. The spike may fail on driver or queue support.
2. Latency and CPU-bound scenes: frame generation can look worse than no frame generation.
3. HUD-less UI composition artifacts.
4. Frozen v1.1.4 SDK; the Vulkan swapchain had fixes in 1.1.2 and 1.1.4, so later bugs are ours.
5. Memory: AMD's table shows about 120 MB (1080p performance) to about 970 MB (4K native AA) for the DX12 measurement; the Vulkan figure is measured in the spike and added to the VRAM log as an owner.
6. Details not yet verified from AMD's docs: the required Vulkan extensions, the exact swapchain install steps in the Vulkan sample, and minimum frame-rate guidance. The spike reads the SDK's Vulkan sample for these.

## Out of scope

OpenGL; FSR 4 frame generation (ML, SDK v2.x only); UI present callback; HDR output; frame generation without the temporal upscaler.
