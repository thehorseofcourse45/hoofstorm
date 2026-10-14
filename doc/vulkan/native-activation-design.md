# Experimental native viewer activation

The normal viewer selects LL_WINDOW_VULKAN from the immutable startup request before API objects exist. NativeRenderer
owns the Vulkan context and complete backend set; the context outlives all its resources. Candidate initialization
is transactional, rejects existing GL objects/contexts and allows one HWND owner. Vertex ownership transfers to
LLVertexBuffer's existing cleanup mechanism. Startup failure can return to WGL before native handles escape;
isVulkanRenderBackendActive() reports the installed API independently of the requested policy. No runtime API switch.

Device capabilities come from Vulkan properties, enabled features and local memory heaps. The legacy numeric GL/GLSL
capability fields express shader-source compatibility for the existing assembler; displayed version strings identify
Vulkan. Single-sample targets and fixed FIFO are intentional current limits. Native sessions disable WGL upload workers,
GL timer/occlusion queries and associated GPU timer benchmarks. Texture uploads stay on the graphics thread.

The frame begins before idle. Native reflection/snapshot passes precede final display because they can reuse framebuffer
zero. Native swap requests are deferred until the final window image is drawn, then submitted/presented once. OpenGL
retains its display/update order. Resize preserves HWND and recreates swapchain resources at acquisition; minimized
windows defer preview/downscale work and refuse snapshots until a frame is active.

Named UBO handles retain caller ownership. Vulkan shadows preserve std140 bytes and pad to the device's uniform range
(capped at the viewer's 64 KiB limit). Active set-2 bindings are discovered from already validated SPIR-V decorations.
Each update is snapshotted before drawing; descriptor sets and offsets used by earlier draws remain unchanged. Buffer
snapshots reuse frame/generation offsets until updated. Ring exhaustion submits/waits before recycling offsets and
increments the generation. Sampler fallback uploads and ring recycling finish before any descriptors/pipeline state
are bound. Unbound named slots use zero-filled storage, matching an explicit safe fallback.

Vertex and index updates now preserve earlier GPU reads by renaming to pooled backing allocations. Completed versions
are reused, partial uploads preserve cached uploaded bytes, and extra Vulkan allocation sizes are capped at 64 MiB;
the bounded-memory fallback still submits/waits. Texture upload/clear/mip/copy work batches in the recording frame,
with staging and replaced image/view retirement at the frame fence. CPU readbacks retain synchronous completion.
Normal Vulkan windows follow the existing graphics-debug setting for validation; tests explicitly enable it. Line loops use a line-strip draw plus a
two-index closing segment. Configured arrays are preserved by bypass draws used for texture downscaling. Vertex shader
translation sets a default point size and invalidates prior shader keys through translator revision 3.

Required ownership order: release client shaders, textures, LLRender and LLVertexBuffer resources while native backends
exist; shutdown capabilities while the native context remains valid; then release backends/context before the HWND.
The API is graphics-thread-only. GPU profiling/occlusion, multiple-frame scheduling, full multisampling, native fullscreen
transitions and distribution packaging remain separate work. See HANDOFF.md for captured checks and manual test limits.

Uniform descriptor limits/alignment follow the [Vulkan descriptor specification](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html).
