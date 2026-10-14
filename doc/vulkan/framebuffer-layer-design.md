# Framebuffer layer: survey and design (DRAFT, needs the user's approval before step 1)

Read `PROJECT-NOTES.md` first. Same method as the other layers: put every framebuffer GL call behind a backend interface with a behavior-neutral OpenGL
implementation, then write a Vulkan implementation in the skeleton and test it on the GPU. This layer also closes two things the texture layer left open:
the `copyFromFramebuffer*` calls and the shadow sampler (it needs a depth texture that can be rendered to).

## What exists today (measured on this tree)

About **110 call sites in 19 files**:

| Where | Calls | What it does |
|---|---|---|
| `llrender/llrendertarget.cpp` (`LLRenderTarget`) | 36 | `glGenFramebuffers` / `glDeleteFramebuffers`, `glBindFramebuffer` (14), `glFramebufferTexture2D` (9: attach or detach one color attachment or the depth texture), `glDrawBuffers` / `glDrawBuffer` / `glReadBuffer` in `bindTarget()` and `flush()`, `glClear`, `glCheckFramebufferStatus` (debug only) |
| clears: `pipeline.cpp`, `llviewerdisplay.cpp`, `llviewerwindow.cpp`, `lldynamictexture.cpp`, `llgltfmaterialpreviewmgr.cpp`, `llmodelpreview.cpp`, `llfloaterimagepreview.cpp`, `lldrawpoolwlsky.cpp`, `llpostprocess.cpp`, `llterrainpaintmap.cpp`, window code | about 30 `glClear` + about 27 `glClearColor` | clear color and depth of whatever is bound |
| `glReadPixels` | 8 + 1 | snapshots (`LLViewerWindow::rawSnapshot`, color and 24 bit depth, one row at a time), the 1 pixel debug readout, picking support, the avatar bake (`llviewertexlayer.cpp`, `lltexlayer.cpp`), the fast timer view |
| `newview` raw framebuffer state | 5 | `llscenemonitor.cpp` binds the default framebuffer for reading to copy it into the capture target; `pipeline.cpp` has a `glCheckFramebufferStatus` helper and a `glDrawBuffers(1, ...)` in the impostor path |
| `llwindow` (`llwindowwin32.cpp`, `llwindowsdl2.cpp`) | 4 | clear the window right after creation (part of the window layer, not this one) |

Facts that shape the design:

- **Almost everything goes through `LLRenderTarget`**: 65 `bindTarget()` and about 240 `flush()` calls in `newview`. Targets nest (`flush()` re-binds the previous one: an explicit stack through `mPreviousRT`), and `sCurFBO`, `sCurResX`, `sCurResY` are CPU-side copies of the GL state that other code reads (`pipeline.cpp`, `llscenemonitor.cpp`).
- `bindTarget()` always sets the draw buffers to the target's color attachments (or none) and the viewport; `flush()` generates mipmaps (`TMG_AUTO`), and with no previous target binds the default framebuffer and `GL_BACK`.
- A target's color and depth attachments are **ordinary textures** made with `LLImageGL::generateTextures` + `setManualImage(..., NULL)` and then attached; the rest of the viewer samples them with `bindManual` / `LLTexUnit::bind(LLRenderTarget*)`. The same texture is a render target in one pass and a sampled texture in the next.
- Color formats in use: `GL_RGBA8`, `GL_RGBA16F`, `GL_RGB16F`, `GL_RG16F`, `GL_R16F`, `GL_R8`, `GL_R11F_G11F_B10F`, plus unsized `GL_RGBA` / `GL_RGB`; depth is always `GL_DEPTH_COMPONENT24`. No multisampling, no renderbuffers, no `glBlitFramebuffer`, no stencil attachment.
- **Clears honor GL state**: `glClear` is limited by the scissor test and by the color and depth write masks. I searched the 25 lines before each of the ~28 `glClear` calls for anything that changes a write mask or the scissor: no call turns a write mask **off**; three (`pipeline.cpp:5100`, `llviewerdisplay.cpp:831` and `:1346`) explicitly turn the color mask **on** first; two clear with the scissor test enabled (`llviewerdisplay.cpp:1991` and the rarely taken branch of `LLRenderTarget::clear`). This is a keyword search, not a data flow analysis: a mask left off by a caller further up the stack would not show.
- Copies from the framebuffer into textures (`glCopyTexSubImage2D/3D`, moved behind `LLTextureBackend` already, about 12 sites) read the **read framebuffer**: mostly the bound render target, in `llscenemonitor.cpp` explicitly the default framebuffer.
- The window layer owns the default framebuffer (swap chain, `SwapBuffers`, clearing at start-up). Under Vulkan it becomes the swapchain image of the frame.

## Design

### One interface, two implementations

`LLFramebufferBackend` (new `llframebufferbackend.h`), `LLGLFramebufferBackend` (today's calls, moved), later a Vulkan implementation in the skeleton. Framebuffer ids are `U32` (0 = the window's default framebuffer, as in GL), texture ids are the texture layer's, enums stay GL constants. The GL model is kept: a current draw framebuffer, a current read framebuffer, draw buffers, a read buffer, and clears and reads that act on them.

    // objects
    createFramebuffer()  /  destroyFramebuffer(fb)
    attach(fb, attachment, textureTarget, texture, level)     attachment = GL_COLOR_ATTACHMENTi or GL_DEPTH_ATTACHMENT; texture 0 detaches
    isComplete(fb)                                            what check_framebuffer_status() asks (debug)
    // binding
    bind(fb)  /  bindRead(fb)  /  getBound()                  (replaces the sCurFBO copy of the state)
    setDrawBuffers(count)  setDrawBufferNone()  setDrawBufferBack()     count = the first N color attachments
    setReadBuffer(attachment or none or back)
    // clear and read
    setClearColor(r, g, b, a)  /  setClearDepth(d)
    clear(mask)                                               GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
    readPixels(x, y, w, h, format, type, data)                from the read framebuffer's read buffer

### What the Vulkan backend has to do (so the interface does not promise anything it cannot)

- With Vulkan 1.3 dynamic rendering a framebuffer is just a list of attachments. **A rendering scope is opened lazily at the first draw or clear after a bind and ended when the binding changes**, when a copy, a readback, or a texture read of one of its attachments needs the result, or at the end of the frame. Rendering is resumed with `loadOp = LOAD` after such an interruption.
- `clear()` at the start of a scope becomes `loadOp = CLEAR` (free); a clear inside a scope is `vkCmdClearAttachments` restricted to the scissor rectangle when the scissor test is on. **GL's write masks cannot be honored by a Vulkan clear**: clears with a mask off would have to be drawn as a quad. The call sites do not use that today (decision 2).
- Attachments are the texture backend's images, which therefore need `COLOR_ATTACHMENT` / `DEPTH_STENCIL_ATTACHMENT` usage and **layout tracking**: `SHADER_READ_ONLY` while sampled, `*_ATTACHMENT_OPTIMAL` while rendered to, transitions inserted when a framebuffer is bound and when a draw samples an image that was an attachment. This is the main new bookkeeping.
- The pipeline key gets the **attachment formats** and the number of draw buffers (`VkPipelineRenderingCreateInfo`, one blend state per attachment; the color mask becomes per attachment), taken from the bound framebuffer.
- `copyFromFramebuffer2D/3D` and `copyImageFromFramebuffer2D` become `vkCmdCopyImage` from the read framebuffer's read attachment after ending the scope; `readPixels` becomes a copy to a host buffer plus a wait (an explicit pipeline stall, like GL's, now visible in the code).
- Framebuffer 0 is the swapchain image of the current frame plus a depth image. Acquire and present stay with the window layer (a later layer); until then the skeleton keeps doing it in `VkContext`.

### Decisions for the user

1. **Interface model.** Recommended: keep GL's state machine (bind, draw buffers, clear, read) as above; `LLRenderTarget` and its 240 call sites do not change, the refactor is moving calls. Alternative: explicit render pass objects (`beginPass(target, loadOps)` / `endPass()`), the natural Vulkan shape, but `LLRenderTarget`'s nesting, mid-pass copies and ~60 loose clears would need real restructuring and could change behavior.
2. **Clears.** `clear(mask)` keeps GL's scissor behavior and ignores the write masks under Vulkan; a debug assertion fires if a clear is requested with a write mask off. By the keyword search above no call site relies on a mask being off; the assertion is the safety net for the cases the search could not see.
3. **Clear color and depth.** `glClearColor` is global GL state set before `glClear`. Recommended: `setClearColor` / `setClearDepth` on the backend with the same set-then-clear order (about 27 sites become a one line change each). Alternative: pass the values to `clear()`, which changes every call site's shape.
4. **The default framebuffer.** This layer treats id 0 as "the window's drawable" and leaves swap, acquire and present to the window layer. `llwindow`'s two start-up clears stay as they are for now.
5. **Layout tracking lives in the texture backend** (it owns the images): the framebuffer backend asks it to transition an image to attachment use and back. Needs the texture interface to grow a way to create attachment-capable images (usage by format) and the layout hooks; I would add these in step 4, not touch the GL texture backend.

## Steps (each a separate, buildable commit; GL behavior must not change)

1. `LLFramebufferBackend` interface + `LLGLFramebufferBackend`; `LLRenderTarget` (36 sites) and the `sCurFBO` / `sCurResX/Y` readers use it.
2. Clears: `setClearColor`, `setClearDepth`, `clear` for the ~55 sites in `newview`, `llrender`, `llwindow`'s callers that are not window start-up.
3. `readPixels` (9 sites), the framebuffer state in `llscenemonitor.cpp` and `pipeline.cpp`, `glCheckFramebufferStatus`.
4. Vulkan framebuffer backend in the skeleton: attachment-capable images and layout tracking in the texture backend, lazy rendering scopes, per attachment formats in the pipeline key, clears, `readPixels`, `copyFromFramebuffer*`. Test on the GPU: render a triangle into an offscreen target and sample it in a second pass, two color attachments (MRT) read back, depth attachment with a depth test and a shadow sampler comparison, a mid-pass clear with a scissor, a nested bind / flush, `copyFromFramebuffer2D` into a texture, `readPixels` of the default framebuffer.
5. (Later, with the window layer) swapchain acquire / present, resize, the front/back buffer reads.

Verification as before: full viewer build after each step (`BUILD_EXIT=0`). Steps 1 to 3 touch every pass of the renderer, so, like the texture and vertex buffer layers, they must be run in-world by the user before they are trusted; I will not launch the viewer.

## Risks

- This is the layer where GL's implicit rules matter most: what is bound when `glClear` / `glReadPixels` / `glCopyTexSubImage` run, draw buffers reset by `bindTarget()`, the viewport set there. Moving calls without touching their order is the mitigation.
- The lazy rendering scope plus layout transitions is where a Vulkan backend breaks first (a scope left open across a copy, a texture sampled while still an attachment); the validation layer catches those, which is the reason to test with it on.
- Performance: every scope switch is a barrier and a new `vkCmdBeginRendering`; the viewer switches targets often (shadows, probes, post process). Measure when a real device exists, not in the skeleton.
- The Vulkan path for `glReadPixels` is a stall; the viewer uses it for snapshots and picking where that is acceptable, and for the 1 pixel debug readout every frame, which should become asynchronous or be limited to debug builds.
