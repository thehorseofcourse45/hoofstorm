# Render thread: design

Status: steps 1-5 implemented 2026-10-14, default on; `FS_VK_RENDER_THREAD=0` switches it off. Verified: queue test, `viewer_vulkan_activation_test` 45/45 threaded and direct, in-world 85-95 FPS vs 45-48 OpenGL. Written from the code in `indra/llrender/` and the main-thread profile of the heavy region.

## Why

The viewer is bound by its main thread. A 300-sample profile with a frame anywhere inside our Vulkan backend (`Vulkan*`, `VkProgram`, `VkContext`, `VkQuery`) shows **31% of main-thread time** in the backend: draw recording about 17%, texture and buffer work, uniform upload, barriers. Driver time itself (`vk_icdGetInstanceProcAddrSG`) is only 4-7%, so moving only the `vkCmd*` calls would gain almost nothing. The gain comes from moving **all** backend work, including the state resolution in `recordCommon` and `VkProgram::prepareDraw`, to a second thread.

Expected result: main thread loses most of that 31%, minus the cost of queueing calls (one small record per call). Realistic: 3-5 ms of a 19 ms frame, and the frame becomes bound by max(main, render) instead of main + render.

## Approach

All viewer code reaches the backend through five interfaces (`LLRenderStateBackend`, `LLShaderBackend`, `LLTextureBackend`, `LLFramebufferBackend`, `LLVertexBufferBackend`) plus the free functions in `llrenderbackend.h`. They are created in one place, `llrenderbackend.cpp`. This is the seam.

- A **proxy** implements each interface on the main thread. A void call becomes a record in a command queue.
- One **render thread** owns the real Vulkan backends and runs the records in order. Real backend code stays single-threaded and unchanged. It never runs on the main thread.
- On by default since 2026-10-14 (verified in-world). `FS_VK_RENDER_THREAD=0` runs every backend call on the viewer thread, as before.

## The queue

A command is `{thunk pointer, payload}` appended to a byte arena. `post(f)` stores a trivially copyable lambda; `postData(src, bytes, f)` also copies caller memory and passes the copy to `f`. No heap allocation per call.

The main thread fills a **batch**. A batch goes to the render thread at: end of frame, a size limit (a few MiB), or a sync call. The render thread executes batches in order and returns emptied arenas for reuse.

**Frame limit.** The main thread may run at most one frame ahead of the render thread. `endFrame` waits while two or more frames are unexecuted. This bounds memory and input latency. The GPU fence wait moves to the render thread.

## Calls that return something

| Kind | Calls | Handling |
| --- | --- | --- |
| Allocate a name | texture `generate`, framebuffer create, vertex buffer `allocate`, uniform buffer create, query `generate` | Proxy keeps the same counter as the backend, hands the name out itself, posts the call with that name; the render thread checks the name matches and aborts if not |
| Cheap state queries | `isCapabilityEnabled`, `queryDepthState`, framebuffer `getBound`, vertex buffer `isBound`, `getVramBytesUsed` | Proxy mirrors state from the setters it forwards; VRAM bytes is an atomic written by the render thread |
| Query results | `available`, `result` | Query backend gets a mutex; `result` flushes first if the result is not ready, because the real call may wait for the GPU |
| Rare, needs a value | shader compile/link/locations/uniform info/program binary, `checkStatus`, `getLevelParameter`, `isResident`, `readImage`, `readPixels` | Sync: post, flush, wait for execution, return |
| Frame start | `beginRenderBackendFrame` returns bool | Proxy checks the window (`IsIconic`, zero client size) itself and returns false without posting; otherwise posts and returns true |

**Vertex buffer `allocate`** returns a CPU pointer the viewer writes into. The proxy allocates that memory itself (same alignment) and posts "adopt this handle and pointer". `free` runs on the render thread after any earlier upload record, which already holds its own copy.

## Memory the viewer hands over

`upload`, `image2D`/`subImage2D`, `compressedImage2D`, uniform arrays: the caller may reuse or free the memory right after the call returns, so the proxy copies the range into the arena. Today the backend already copies into mapped GPU memory on the main thread; the main thread now copies into cached memory and the render thread does the slow copy.

## Not changed

Real backend logic, pipeline cache, shaders, frames in flight, ring buffers, the viewer, GL fallback.

## Errors and shutdown

An exception on the render thread is fatal: it prints the message and aborts (the backend already does this for hard errors). `waitRenderBackendIdle`, resize, `shutdownRenderBackend`: sync, then the render thread is joined last.

## Steps

1. Queue, batch hand-off, render thread, sync wait. Unit test with plain lambdas.
2. Proxy for state and framebuffer (small). Run viewer with real draws proxied.
3. Proxy for vertex buffers and textures (data copies, name prediction).
4. Proxy for shaders (hot: uniforms and `use`; sync: everything else) and the free functions (uniform buffers, queries, frame calls).
5. Counters: records per frame, bytes per frame, sync calls per frame, main-thread wait time. Log in `VulkanPacing`.
6. Verify: existing test suites through the proxy where they can; `llvulkan_thread_test` (frames rendered through the proxy match direct mode pixel for pixel); in-world profile at the same spot.

## Risks

- A sync call on a per-frame path stalls the main thread. The step 5 counters find these.
- Name or lifetime mismatches show up as aborts at the first mismatch, not as silent corruption.
- Window resize and minimize cross threads. Handled by sync calls and the window check.
- Gain depends on the queueing cost. If enqueue plus copies eat more than half of the 31%, the thread is not worth keeping on; the switch stays.
