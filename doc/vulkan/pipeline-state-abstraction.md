# Fixed pipeline state abstraction: design

Status: DRAFT, needs the user's approval before step 1. Read `PROJECT-NOTES.md` first.
Context: the vertex buffer layer (see `vertexbuffer-abstraction.md`) is done and was tested in the viewer by the user
("works perfectly fine"). This is the next layer: blend, depth, stencil, cull, polygon mode, polygon offset, viewport, scissor,
line/point size, color mask.

## What exists today

The viewer already routes most state changes through helpers that keep a CPU-side shadow of GL state, and it calls
`gGL.flush()` (flushes the immediate-mode vertex batch) before every change. Vulkan needs exactly that discipline.

| Mechanism | Where | What it does |
|---|---|---|
| `LLGLState` / `LLGLEnable` / `LLGLDisable` (RAII) | `llgl.h`, `llgl.cpp:2552` | `glEnable`/`glDisable` of one capability with a global cache `sStateMap`; restores the old value in the destructor. `sStateMap` is used only inside `llgl.cpp`. |
| `LLGLDepthTest` (RAII) | `llglstates.h`, `llgl.cpp:2822` | depth test enable, write enable, compare function, with static shadow values. About 160 uses. |
| Named bundles: `LLGLSPipeline`, `LLGLSUIDefault`, `LLGLSDefault`, ... | `llglstates.h` | combinations of the above. |
| `LLRender` members | `llrender.h` | `setSceneBlendType`, `blendFunc` (neutral `eBlendFactor` enum already!), `setColorMask`, `setLineWidth`; shadow in `mCurrBlend*`, `mCurrColorMask`. |
| Raw GL calls that bypass all of this | `newview/` | see below. |

Usage numbers (measured with grep over `indra/`):

- 71 `.cpp` files use the RAII state classes. Capabilities passed: `GL_BLEND` 78, `GL_CULL_FACE` 50, `GL_STENCIL_TEST` 18,
  `GL_POLYGON_OFFSET_FILL` 16, `GL_POLYGON_OFFSET_LINE` 8, `GL_SCISSOR_TEST` 5, `GL_CLIP_PLANE0` 3 (legacy), others 1 each.
- Raw calls in `newview/`, `llui/`, `llwindow/`, `llappearance/` (comments excluded):
  `glPolygonMode` 63 (`llviewerwindow.cpp` 20, `pipeline.cpp` 15, `llspatialpartition.cpp` 12, `llmodelpreview.cpp` 8, `gltfscenemanager.cpp` 4,
  `llselectmgr.cpp` 2, `llviewerdisplay.cpp` 1, `llface.cpp` 1), `glPolygonOffset` 29, `glViewport` 14, `glCullFace` 8,
  `glPointSize` 7, `glStencilOp` 2, `glStencilFunc` 2, `glColorMask` 2, `glStencilMask` 1, `glScissor` 1.

So the problem is small and regular: about 130 raw calls plus a vocabulary of roughly ten states.

## What Vulkan changes (verified against the SDK header `vulkan_core.h`, Vulkan 1.4, header version 357)

Vulkan has no `glEnable`. State lives in a pipeline object, except the states marked dynamic.

- Core dynamic state (can change inside a command buffer without a new pipeline): viewport, scissor, line width, depth bias (+ enable),
  blend constants, stencil compare/write mask and reference, cull mode, front face, primitive topology, depth test enable,
  depth write enable, depth compare op, stencil test enable, stencil op.
- Dynamic only through the `_EXT` extension (extended dynamic state 3): **polygon mode, blend enable, blend equation, color write mask**.
- Not state at all: **point size** (the vertex shader writes `gl_PointSize`).
- Blend factors/equation and color write mask are therefore part of the pipeline key (or use the extension where present).

Consequences for the viewer:
1. A Vulkan backend resolves "pipeline = shader + vertex layout + blend + polygon mode + color mask + render target formats" at draw time and
   caches it. The more state the viewer changes per draw, the more pipelines it builds; state should change rarely and in coarse bundles.
2. `glPointSize` call sites must become a shader uniform (the viewer's point shaders) in the Vulkan path.
3. Viewport and scissor are cheap and dynamic. Keep them as explicit setters.
4. `glClear` honors scissor, color mask and depth mask in GL. That coupling does not exist in Vulkan (clear is part of the render pass or
   `vkCmdClearAttachments`). That belongs to the framebuffer layer; it is a known interaction, not part of this layer.

## Design: eager setters on a state backend (Strategy A)

Add a second backend interface next to `LLVertexBufferBackend`. The GL backend applies each change **immediately**, exactly as the current code does,
so OpenGL behavior stays identical. A Vulkan backend just records the change into a pending state struct and resolves the pipeline at the next draw.
This avoids the main risk of a "lazy apply at draw" design for GL: GL calls that read state outside draws (`glClear`, `glReadPixels`, `glCopyTexSubImage`)
would see stale state.

Neutral vocabulary (all names provisional):

    enum class CullMode  { None, Front, Back };
    enum class PolygonMode { Fill, Line };
    struct DepthState   { bool test; bool write; U32 func; };          // func uses the existing GL value for now
    struct StencilState { bool enable; U32 func; S32 ref; U32 mask; U32 sfail, dpfail, dppass; };
    struct RasterState  { CullMode cull; PolygonMode poly; bool offsetFill, offsetLine; F32 offsetFactor, offsetUnits; F32 lineWidth; };
    struct BlendState   { bool enable; eBlendFactor cs, cd, as, ad; bool colorMask[4]; };   // eBlendFactor already exists in llrender.h
    struct Viewport/Scissor { S32 x, y, w, h; bool scissorEnable; };

Backend interface (sketch):

    class LLRenderStateBackend {
        virtual void setCapability(U32 cap, bool on) = 0;      // GL_BLEND, GL_CULL_FACE, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_POLYGON_OFFSET_*, GL_CLIP_*
        virtual bool getCapability(U32 cap) const = 0;
        virtual void setDepth(bool test, bool write, U32 func) = 0;
        virtual void setBlendFunc(...)  / setColorMask(...) = 0;
        virtual void setCullFace(CullMode) = 0;
        virtual void setPolygonMode(PolygonMode) = 0;
        virtual void setPolygonOffset(F32 factor, F32 units) = 0;
        virtual void setStencil(...) = 0;
        virtual void setViewport(S32,S32,S32,S32) = 0;  virtual void setScissor(S32,S32,S32,S32) = 0;
        virtual void setLineWidth(F32) = 0;  virtual void setPointSize(F32) = 0;   // Vulkan backend: point size via shader uniform
    };

Decision on vocabulary: **keep the GL capability constants (`GL_BLEND`, ...) as the viewer's state vocabulary for now.**
Renaming roughly 180 call sites in 71 files to new constants is a pure mechanical change that buys nothing until the last GL dependencies
(textures, shaders, framebuffers) are also gone. The Vulkan backend maps the GL values to its own state. Revisit at the end.

The existing RAII classes keep their API (so those 71 files need no edits). Internally they stop calling `glEnable`/`glDisable`/`glDepthFunc`
and call the state backend instead. The `gGL.flush()` call before every change stays.

## Steps (each a separate, buildable commit; GL behavior must not change)

1. Add `LLRenderStateBackend` and the GL implementation. Move the body of `LLGLState::setEnabled`, `~LLGLState`, `LLGLDepthTest` ctor/dtor,
   and the `sStateMap` / depth statics into it. The RAII classes call the backend. `LLGLState::checkStates` / `glIsEnabled` debug checks stay in the GL backend.
2. Route `LLRender::setSceneBlendType`, `blendFunc`, `setColorMask`, `setLineWidth` and the two `glColorMask` raw calls through it.
3. Replace the raw calls with `gGL` / backend calls, one commit per kind, smallest first:
   `glScissor` (1), `glStencil*` (5), `glCullFace` (8), `glPointSize` (7), `glViewport` (14), `glPolygonOffset` (29), `glPolygonMode` (63).
   Preserve the exact order and the restore-to-previous patterns (many sites set a value and put it back).
4. Legacy: `GL_CLIP_PLANE0` / `LLGLUserClipPlane` (3 files) and `GL_ALPHA_TEST`: check whether they work in the core-profile context at all
   before touching them (same situation as the physics hull draw, see `PROJECT-NOTES.md`).

## Verification

Same as the vertex buffer layer: full build, run the viewer, compare. Specific things to look at for this layer: transparency (blend), alpha-tested
foliage, shadows (polygon offset, cull face), wireframe views (Develop > Render Tests, Advanced > Hide Selected / Wireframe), object selection outlines and
manipulator widgets (stencil, cull), and UI clipping (scissor).

## Risks and open questions

- State restore patterns: many sites set state and put it back by hand (`glCullFace(GL_FRONT)` ... `glCullFace(GL_BACK)`). They assume the default is
  `GL_BACK`. A Vulkan backend must treat these as a pair. Prefer converting them to scoped RAII helpers so the restore is guaranteed.
- `glPolygonMode` sites in `llviewerwindow.cpp` (20) and `pipeline.cpp` (15) are mostly debug visualizations. Check whether each is reachable
  before porting; remove if dead (like the terrain code in PR #2).
- The state backend and the vertex buffer backend will end up as parts of one device object once textures, shaders and render targets are done.
  Keep them as separate interfaces for now; merge only when a Vulkan device class exists.
- Pipeline explosion: the Vulkan backend needs a pipeline cache keyed by state. Measure the number of distinct combinations in a typical frame
  (log them from the GL backend in a debug build) before designing that cache.
