# LLVertexBuffer abstraction: design

Status: design APPROVED by the user. Step 1 merged (PR #3). Step 2 in progress on branch `vb-neutral-types`. Steps 3-5 not started. Read `PROJECT-NOTES.md` first.

## What `LLVertexBuffer` does today

Files: `indra/llrender/llvertexbuffer.h` (346 lines), `llvertexbuffer.cpp` (about 1,700 lines after dead-code removal).

- Holds a CPU-side **shadow copy** of vertex data (`mMappedData`) and index data (`mMappedIndexData`). Callers write
  through striders (`getVertexStrider()` etc.) or `setXxxData()`. `mapVertexBuffer`/`unmapBuffer` collect dirty byte ranges
  (`mMappedVertexRegions`).
- On `unmapBuffer()` the dirty ranges are uploaded to a GL buffer (`flush_vbo()` -> `glBufferSubData`, in 64 KB blocks).
  No `glMapBuffer` is used. This maps well onto Vulkan staging uploads.
- Buffer storage comes from a pool, `LLVBOPool` (already an interface), with two implementations: `LLDefaultVBOPool`
  (reuses buffers by size class) and `LLAppleVBOPool` (no pooling; allocates the GL buffer late, at first upload).
- Binding is state-cached: statics `sGLRenderBuffer` / `sGLRenderIndices` remember what is bound, `sLastMask` remembers
  which vertex attribute arrays are enabled.
- `setBuffer()` binds the buffer and calls `setupVertexBuffer()`, which issues one `glVertexAttribPointer` per attribute
  that the **currently bound shader** needs (`LLGLSLShader::sCurBoundShaderPtr->mAttributeMask`).
- Drawing: `draw/drawRange/drawRangeFast` -> `glDrawRangeElements`; `drawArrays` -> `glDrawArrays`.
  Primitive mode comes from `LLRender::eGeomModes` via the table `sGLMode[]`.

## Where GL leaks out of the class

Inside `llvertexbuffer.cpp`: 38 raw GL calls (buffer gen/delete/bind/data/subdata, attrib pointers, enable/disable
arrays, draw). Everything else already goes through a neutral API.

Leaks in the public/protected interface (`llvertexbuffer.h`):

| Leak | Fix |
|---|---|
| `U32 mGLBuffer`, `mGLIndices` (GL names) | opaque handle type |
| `U32 mIndicesType = GL_UNSIGNED_SHORT` | `enum class IndexType { U16, U32 }` |
| `flush_vbo(GLenum target, ...)` and `GL_ARRAY_BUFFER`/`GL_ELEMENT_ARRAY_BUFFER` in pool API | `enum class BufferKind { Vertex, Index }` |
| `static sGLMode[]` (public) | move into the GL backend |
| `static sGLRenderBuffer`, `sGLRenderIndices` (public) | make private to the backend |

Leaks outside the class (checked with grep over all of `indra/`):

- `llmodelpreview.cpp` lines ~4675, 4805, 4877, 5030: `LLVertexBuffer::sGLRenderBuffer = 0;` to force a rebind
  (Linden issue #912). Replace with a named method, e.g. `LLVertexBuffer::invalidateBinding()`. Same behavior.
- `LLVertexBuffer::unbind()` is called from about 55 places. It is already neutral; keep it.
- `getMappedData()` / `getMappedIndices()` are used for `memset` of the CPU shadow copy (`llvoavatar.cpp`, `llvotree.cpp`,
  `llmodelpreview.cpp`, `llglsandbox.cpp`). That is CPU memory; keep.
- `LLVertexBuffer::setupClientArrays()` is called from `llglslshader.cpp:1068`. Backend-specific; will move behind the interface.
- `friend class LLRender` and `LLNavShapeVBOManager` / `LLNavMeshVBOManager` (external package) depend on protected members.
  Do not remove those signatures without checking.

Good news: **no other module reaches into the GL handles.** The abstraction boundary is small.

## What Vulkan changes (why this is not a rename of GL calls)

GL binds buffers and attribute pointers as global state at `setBuffer()`. Vulkan has no such state: vertex input layout is
part of the *pipeline object*, and buffers are bound inside a command buffer at draw time. So the interface must record
**what to draw**, not **how GL state is set**:

- The vertex layout must be described as data: `(typemask, offsets[TYPE_MAX], strides)`. The GL backend turns it into
  `glVertexAttribPointer` calls; a Vulkan backend uses it to build/look up a pipeline (key = shader + layout + render state).
- `setBuffer()` becomes "make these buffers current for the next draw" and `draw*()` becomes "draw now".
- Buffer lifetime in Vulkan needs GPU-fence-aware deferred deletion. GL already does a crude version of this:
  `delete_buffers()` waits 4 frames (`sFreeList[4]`).
- Uploads: GL does `glBufferSubData`. Vulkan will stage into a host-visible buffer and `vkCmdCopyBuffer`.
- The per-attribute location in GL comes from the fixed enum order (`loc = TYPE_xxx`), so shaders already bind attributes
  by fixed location. That helps SPIR-V (`layout(location = N)`).

## Proposed interface

Internal header `llvertexbufferbackend.h`, one abstract class, one GL implementation `llvertexbuffer_gl.cpp` (moved out of
`llvertexbuffer.cpp`). `LLVertexBuffer` owns no GL calls afterward.

    enum class BufferKind { Vertex, Index };
    enum class IndexType  { U16, U32 };
    struct VertexLayout   { U32 typeMask; U32 offsets[TYPE_MAX]; };   // strides come from sTypeSize[]
    struct BufferHandle   { U64 id; };                                // GL name now; VkBuffer + allocation later

    class LLVertexBufferBackend {
        // storage (today's LLVBOPool)
        allocate(BufferKind, U32 size, BufferHandle&, U8*& cpuShadow);
        free(BufferKind, U32 size, BufferHandle, U8* cpuShadow);
        U64  vramBytesUsed();
        // upload
        upload(BufferKind, BufferHandle, U32 offset, U32 size, const void* data);
        createStatic(BufferKind, BufferHandle&, U32 size, const void* data);   // Apple late-allocation path
        // current-buffer state
        bind(BufferHandle vb, BufferHandle ib, const VertexLayout&);
        unbind();  invalidateBinding();
        // draw
        drawIndexed(mode, start, end, count, IndexType, indexOffset);
        drawArrays(mode, first, count);
    };

One global backend pointer (like today's `sVBOPool`), chosen at `initClass()`. OpenGL is the only backend until step 5.

## Steps (each is a separate, buildable commit)

1. **DONE** Delete dead code: disabled `ENABLE_GL_WORK_QUEUE` machinery (about 200 lines). Branch `vb-remove-gl-workqueue`.
2. Neutral types in the header: `BufferKind` and `IndexType` replace the GL enums in `LLVertexBuffer` and the pool
   (`mIndicesType` is now `mIndexType`); `sGLMode[]` leaves the header; `invalidateBufferBinding()` replaces the four
   `sGLRenderBuffer = 0` writes in `llmodelpreview.cpp` (it resets only the vertex-buffer cache, exactly like the old code).
   Deliberately NOT in step 2: `BufferHandle` (the pool still passes raw `GLuint` names; it moves in step 3 with the pool)
   and making `sGLRenderBuffer` / `sGLRenderIndices` private (the file-local pool classes write them directly; fix in step 3).
   No behavior change.
3. Introduce `LLVertexBufferBackend` and move all 38 GL calls into `llvertexbuffer_gl.cpp`. `LLVBOPool` becomes part of it.
   No behavior change. Verify: build, run, compare screenshots.
4. Same refactor for the non-class users of vertex-array state: `llglslshader.cpp:1063-1068` (`setupClientArrays`,
   `unbind`) and `LLRender`'s immediate-mode path (`llrender.cpp:~1766`, which creates its own vertex buffers).
5. Skeleton `llvertexbuffer_vk.cpp` behind a runtime switch, only after the window/swapchain layer exists. Needs the
   shader and pipeline-state layers; do not start before those are designed.

## Verification plan

- Build `ReleaseOS`; run; log in to a local OpenSim region (or any grid) and compare frames against a baseline build
  from `master` at fixed camera positions.
- `PacketDropPercentage`-style debug aids are irrelevant here. Useful: `gDebugGL`, `STOP_GLERROR` (already in the code),
  and the Statistics bar "Draw calls" and VRAM numbers (`getBytesAllocated()` / `getVramBytesUsed()` must not change).
- Keep every step behavior-neutral. If a frame differs, the step is wrong.

## Risks and open questions

- `LLAppleVBOPool` allocates the GL buffer lazily (macOS path). The interface must keep that or macOS regresses.
  We build on Windows only, so this is untestable here; keep the code path intact and mark it untested.
- `LLNavShapeVBOManager` / `LLNavMeshVBOManager` are `friend`s from "an external package" (comment in the header).
  Check whether that package exists in this repo before changing protected members.
- Thread rule from the header comment: GL buffer creation must happen on the GL thread. A Vulkan backend needs its own
  rule for which thread may record commands.
- `sLastMask` / `setupVertexBuffer()` depend on the currently bound shader's `mAttributeMask`. The backend interface must
  receive the attribute mask (or the pipeline key) explicitly, not read `LLGLSLShader::sCurBoundShaderPtr`.
