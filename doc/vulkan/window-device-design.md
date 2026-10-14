# Windows Vulkan device and window runtime

The viewer and GPU test renderer now build the same runtime from `indra/llrender/vulkan`.
This is the foundation for startup backend selection; OpenGL remains selected until that separate step is integrated.

The existing `LLWindow::getPlatformWindow()` supplies the HWND. The window remains owned by `llwindow`; Vulkan owns
the instance, logical device, graphics/present queue, surface, swapchain, image views and size-dependent attachments.
No new native-window wrapper is needed.

`LL_VULKAN_RUNTIME=ON` builds `llvulkan` and links it through `llrender`. It requires the installed Vulkan SDK on Windows.
The option defaults off so the existing OpenGL-only build does not acquire an SDK requirement. This option compiles
the module; it does not switch the viewer's active renderer. glslang/compiler packaging remains a later step.

The first runtime uses one frame in flight. The frame fence gates reuse of the command buffer, uniform ring and
frame descriptor pool. Pending resource destruction is collected after that fence completes; buffers, image views,
images, device memory, program layouts, descriptor layouts and shader modules retain their GPU handles until then.
Deletion during command recording cannot destroy resources referenced by unsubmitted commands. Cleanup also drains
the queue before destroying the descriptor pools/device.

Acquire semaphores belong to the frame. Presentation-complete semaphores belong to swapchain images, as required for
[safe presentation semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

Resize recreates the swapchain, its image views, default depth image, snapshot readback buffer, and presentation
semaphores together. It currently waits for the device, following the conservative
[swapchain recreation approach](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/04_Swap_chain_recreation.html).
Out-of-date acquisition recreates without resetting the frame fence, then retries after processing window events.
Suboptimal acquisition is still used and schedules recreation. Suboptimal/out-of-date presentation schedules
recreation for the next frame. A minimized or zero-area client skips acquisition and leaves its fence signaled.
The window event loop continues while rendering is skipped.

Vulkan call failures propagate as exceptions, so the later startup selector can report failure and choose OpenGL.
The noncopyable context cleans up on destruction, including partially initialized instances/devices. Explicit
shutdown is idempotent. The standalone test executable catches errors and exits with a failure result.

Verification covers initial acquire/present/readback, four window sizes, minimize/restore, dirty-swapchain recovery,
fence state on skipped frames, destruction of a buffer referenced by recorded commands, partial initialization cleanup,
and repeated shutdown. The existing framebuffer, texture, triangle and real UI-shader tests use this shared runtime.

Remaining integration: construct this context using the viewer's HWND during startup backend selection, install
all matching rendering backends before their first use, and supply the begin/end-frame calls in the viewer loop.
The old WGL context creation and `SwapBuffers` path remain active until that selection path is ready. Several frames
in flight, asynchronous upload scheduling, and presentation-fence-based asynchronous recreation remain later work.
The current device-idle recreation follows the standard tutorial, but does not prove low-latency resize behavior.

The earlier VK_ERROR_UNKNOWN surface-capabilities failure was reproduced in the unchanged baseline and is no longer
reproducible on the user's unlocked, connected desktop. It was not attributed to a specific cause or patched speculatively.
The user confirmed the previous OpenGL framebuffer build's in-world rendering and snapshots looked correct.
