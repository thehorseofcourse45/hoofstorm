# Two frames in flight: design

Status: step 1 implemented 2026-10-13 (completedFrame/submittedFrame, refreshCompleted, waitForFrame; one slot, behavior unchanged). Step 2 implemented (VkContext::FrameSlot holds cmd/fence/imageAvailable/samplerPool/release; access via slot(); slotCount is 1). Step 3 implemented (ring split: ringTotal buffer, ringSize = region capacity, FrameSlot::ringBase, ringOffset absolute). Step 4 implemented: FS_VK_FRAMES_IN_FLIGHT=2 (or VkContext::requestedFramesInFlight) enables two slots; default stays 1. beginFrame waits only the reused slot; per-slot release lists, sampler pools, acquire semaphores, ring regions and frame-timer queries; completedFrame advances per slot fence; depth/headless color layout transitions carry memory access; swapchain asks for slotCount+1 images. Stress fixture llvulkan_frames_test (doc/vulkan/tools/run-frames-test.cmd): 32 checks at 1 and 2 slots, 23/23 frames overlap at 2, sync validation clean; mutations (always-reusable vertex storage, shared ring region) fail it. Steps 5-6 (in-world test, then default 2) pending. Behavior notes: release callbacks run slotCount frames later; GPU frame time lags slotCount frames. Written 2026-10-13 from the code in `indra/llrender/vulkan/` and the settled in-world profile in `profile-settled.log`.

## Why

The settled main-thread profile (186 focused samples of 200) puts 30.5% of the time in `VkContext::beginFrame`, waiting on `frameFence` (`vkcontext.cpp:564`). The CPU cannot start recording frame N+1 until the GPU has finished frame N. Frame time is therefore roughly CPU + GPU instead of max(CPU, GPU).

Measured, user's heavy region, about 20 FPS: GPU average 17-28 ms, CPU recording 54-88 ms (this includes the fence wait and other stalls, categories overlap). After the change the floor is the CPU work, about 70% of today's frame. Realistic best case is a 25-30% shorter frame (about 20 -> 26-28 FPS). It will not reach GL's 45-48 FPS alone; draw recording (22%), texture upload (14%) and avatar updates (10%) remain. Cost: one more frame of input-to-photon latency.

## Current lifetime model (what must change)

Everything relies on one invariant: when `beginFrame` returns, the previous frame has fully completed on the GPU.

| Site | Assumption it makes |
| --- | --- |
| `vkcontext.cpp:564` `beginFrame` | waits `frameFence`, then resets ring offset, `framePool`, and the one `cmd` |
| `vkcontext.cpp:703` `deferRelease` | release now if not recording and `frameFence` is signaled, else queue; `collectReleasedResources` runs everything queued after the wait |
| `vkbackends.cpp:296-298` VB/IB `upload` | storage is safe if `usedFrame < frameCounter` (previous frame is done) or the fence is signaled |
| `vkbackends.cpp:306-316` | pool of retired versions capped at 64 MiB; on overflow flush or wait `frameFence`, then mark everything unused |
| `vkquerybackend.cpp:62,76,193` | query pages reusable when `page->frame < frameCounter`; slots carry `submissionSerial+1` |
| `vkprogram.cpp:172,210` | sampler set and uniform snapshot are cached per `frameCounter`; ring offsets reset each frame |
| `vktexturebackend.cpp:437` | staging ring usable when idle only if `frameFence` is signaled |
| `vkcontext.h:131-139` | one 16 MiB uniform/staging ring, one `framePool`, one `cmd`, one `imageAvailable` |
| `flushCommands`, `submitNow` | submit then `vkQueueWaitIdle` (blocking partial submits) |

## Design

### 1. One completion counter instead of "previous frame"

Add to `VkContext`:

- `submittedFrame` (monotonic, number of the last frame handed to `vkQueueSubmit` with a fence)
- `completedFrame` (highest frame whose fence is known signaled)
- `refreshCompleted()`: polls the fences of in-flight slots with `vkGetFenceStatus` and advances `completedFrame`. Never waits.
- `waitForFrame(n)`: waits that slot's fence, advances `completedFrame`.

Every "safe to reuse" test becomes `usedFrame <= completedFrame`. `usedFrame` already stores `frameCounter` at use time, so the data is there; only the comparison changes. `queueWaitIdle` sites set `completedFrame = submittedFrame` after they return (all submitted work is done).

### 2. Frame slots

```
struct FrameSlot {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore imageAvailable;
    VkDescriptorPool samplerPool;      // today's framePool
    VkDeviceSize ringBase, ringSize;   // region of the shared ring
    std::vector<std::function<void()>> release;  // today's pendingRelease
    uint64_t frame;                    // frame number last submitted from this slot
};
```

`kFramesInFlight` is 1 or 2, chosen once at `init` (env `FS_VK_FRAMES_IN_FLIGHT`, default 2 after validation, 1 reproduces today exactly). `frameSlot = frameCounter % kFramesInFlight`. `renderDone` stays per swapchain image.

`beginFrame` changes: wait only the fence of the slot about to be reused (frame N-2), not N-1. That slot's `release` list runs after the wait. Then reset that slot's pool and command buffer as today.

### 3. Per-slot resources

- **Uniform/staging ring:** split the existing 16 MiB buffer into two 8 MiB regions, one per slot. `ringOffset` resets to the slot's base. Overflow behavior (flush, reset, `++uniformGeneration`) is unchanged but now only has to wait for work from this slot's own region, which is already complete after the fence wait. Texture payloads over 8 MiB already fall back to independent staging.
- **Sampler descriptor pool:** one pool per slot, reset in `beginFrame`.
- **Deferred release:** a resource released while frame N is recording goes on slot N's list. It is destroyed when that slot's fence is next waited, after the GPU can no longer use it. A release issued while nothing is recording goes on the last submitted slot's list, or runs immediately if `completedFrame >= submittedFrame`. This is the conservative extension of today's rule.
- **Command buffers:** one per slot. `flushCommands` keeps its current shape (end, submit without fence, `vkQueueWaitIdle`, restart the same buffer). It sets `completedFrame = submittedFrame` for already-submitted frames; the current frame's fence is still signaled by `endFrame`.

### 4. Hazards to audit and how each is handled

1. **Vertex/index versions** (`vkbackends.cpp:296`). Replace `usedFrame < frameCounter || submittedComplete` with `usedFrame <= completedFrame` after `refreshCompleted()`. With two frames the pool needs headroom for both in-flight generations; the 64 MiB cap stays and the overflow path becomes `waitForFrame(usedFrame)` instead of waiting the single fence. The thrash seen in the latest log (evictions about equal to allocations) may get worse; check the counters before and after, and consider scaling the cap, but do not change it blindly.
2. **Queries** (`vkquerybackend.cpp`). Pages reusable when `page->frame <= completedFrame`. Slot serials already compare against `submissionSerial`; keep, but result reads must not assume the immediately preceding frame is complete. Results stay "available when the fence says so"; the existing nonblocking availability polling already fits. Timestamp GPU time already lags a frame; it may lag two.
3. **Descriptor sets.** Set 1 comes from the per-slot pool. Set 0 uses dynamic offsets into the per-slot ring region, so the static set is not rewritten. **Audit set 2 (named blocks):** if any `vkUpdateDescriptorSets` targets a set that an in-flight command buffer uses, it is invalid without update-after-bind. Fix by allocating those from the per-slot pool.
4. **Program uniform shadows** (`vkprogram.cpp:210`). `buffer.frame != frameCounter` already forces a re-copy each frame; with per-slot regions the copy lands in the current slot's region. No change beyond the offset base.
5. **Texture images, views, staging.** All replacement paths already go through `deferRelease`. Verify none call `vkDestroy*` directly (grep before implementing).
6. **Image layouts.** Tracked on the CPU in recording order and executed in queue order on the single queue, so frame N+1's barriers follow frame N's. No change. The shared depth read-only transitions and the feedback guards stay as is.
7. **Swapchain.** `recreateSwapchain` already `vkDeviceWaitIdle`s; fine. Request `minImageCount + 1` images (at least 3) so FIFO acquire does not stall with two frames in flight. `renderDone[imageIndex]` per image is already correct.
8. **Blocking paths** (`flushCommands`, `submitNow`, `readPixels`, framebuffer copies, snapshots). Correct as they are: `vkQueueWaitIdle` also drains the previous frame. They just cost more. Keep their stall counters; blocking submits per interval (5-29 now) should not rise.
9. **Shutdown and device loss.** `shutdown` already `vkDeviceWaitIdle`s and then collects releases. Collect all slot lists, destroy all slot objects.
10. **Minimize and skipped frames.** `beginFrame` returning false must not reset the slot fence or advance `frameCounter`; unchanged.
11. **CPU-side state shared across frames:** `LLRender`/`LLVertexBuffer` shadow copies are CPU memory and are never read by the GPU. Mapped GPU-visible buffers are the only CPU-written memory the GPU reads: those are covered by item 1 and the ring.

### 5. What does not change

The GL-facing interfaces, shader and pipeline layout, pipeline cache, shader cache, framebuffer feedback guards, the synchronous readback contract, the native frontend and all viewer code. No graphics settings, frame cap or background yield change.

## Implementation steps

1. Introduce `completedFrame`/`submittedFrame`, `refreshCompleted`, `waitForFrame`, and rewrite the sites in the table to use them, with `kFramesInFlight == 1`. Behavior must be identical; run the full suite. This isolates the invariant change from the resource duplication.
2. Introduce `FrameSlot` and move cmd/fence/semaphore/pool/release list into it, still with one slot.
3. Split the ring per slot. Still one slot in flight.
4. Enable `kFramesInFlight = 2` behind the env variable. Add the swapchain image count change.
5. Add the stress fixture and sync-validation runs below. Fix what they find.
6. Make 2 the default only after the user's in-world run; keep the variable as the kill switch.

## Verification

- Existing gates at both 1 and 2 slots: `llvulkan_upload_ring_test` (46), `llvulkan_rgb10_test` (41, plus the two intentional-abort feedback fixtures), query tests (28 validated / 28 normal), native frontend (45 / 45, GL 10, fallback 10), calibration (21 / 21 / 17), 123 headless / 144 windowed broad gates. Zero validation errors and warnings, with `VK_LAYER_VALIDATE_SYNC=1` (synchronization validation catches missed CPU-GPU and GPU-GPU hazards across frames).
- **New stress fixture** (`llvulkan_frames_test`): each frame rewrites a vertex buffer, an index buffer, a texture (upload, mip, resize), a uniform block and a query with frame-unique data, while the frame's shader is made slow enough (long loop) that the GPU is still on frame N when the CPU records N+1 and N+2. Read back every frame and check the pixels belong to that frame. Mutation check: make one `usedFrame <= completedFrame` test always true and confirm the fixture fails.
- Fixture for the release path: create and destroy textures, buffers and programs while two frames are pending; assert the destroy callbacks run only after the owning frame completed (counter plus fence status).
- Pacing fixture (`llvulkan_frame_pacing_test`) at 1 and 2 slots: median and p95 frame time on stream and texture workloads. Expect improvement only when GPU time is a meaningful fraction of CPU time.
- Live: same region, same settings, viewer focused. Compare `VulkanPacing` fence-wait average (should fall sharply), CPU recording, GPU average, blocking calls, buffer storage alloc/reuse/evict, and FPS. Visual check of terrain, avatars, UI, shadows, water, snapshots, resize and minimize.

## Risks

- A missed hazard shows up as flicker, stale textures or corrupt geometry that appears only under load. Mitigation: the stress fixture, sync validation, and the 1-slot kill switch.
- If CPU recording is the real bound after the fence wait disappears, FPS gain will be smaller than the 30% ceiling. The profile says 70% of the frame is CPU, so that is expected.
- Buffer pool pressure doubles. Watch evictions; they are already high (7864 evictions against 7104 allocations in the latest interval).
- One more frame of latency.

## Not in scope

Async or batched startup uploads, a threaded renderer, a transfer queue, replacing `vkQueueWaitIdle` in the partial-submit paths with fences, and the CPU draw-recording cost. Each needs its own measurement.
