# Vulkan skeleton: what it proved and what it found

Code: `indra/vulkan_skeleton/` (standalone; the viewer build does not reference it).
Build: `set VULKAN_SDK=C:\VulkanSDK\1.4.357.0`, `set CMAKE_GENERATOR=Visual Studio 17 2022`, `set CMAKE_GENERATOR_PLATFORM=x64`,
then `cmake -S indra\vulkan_skeleton -B build-vulkan-skeleton` and `cmake --build build-vulkan-skeleton --config Release`.
Run: `set VK_LAYER_PATH=C:\VulkanSDK\1.4.357.0\Bin` then `build-vulkan-skeleton\Release\vulkan_skeleton.exe --test` (opens a window briefly;
exit code 0 = pass). Without `--test` it draws until you close the window. `--no-validation` turns the validation layer off.

## What it is

A Win32 window, a Vulkan 1.3 device (dynamic rendering, no render passes), a swapchain, and one triangle. The triangle is drawn **only through the
two backend interfaces** the viewer now has: `LLVertexBufferBackend` and `LLRenderStateBackend`. `vkbackends.cpp` is a real Vulkan implementation of both.
`main.cpp` makes the same kinds of calls `LLVertexBuffer` and `LLRender` make (allocate, bind, upload, setBuffers, drawIndexed, setViewport,
setCapability, setBlendFunc, ...).

## Result (tested on this machine: AMD Radeon RX 6700 XT, Vulkan 1.4.315)

`--test` passes with the Khronos validation layer on and **0 validation errors, 0 warnings**. It reads the swapchain image back and checks five pixels
against values computed from the triangle's barycentric weights. All five matched within 1/255:

| Sample | Expected | Got |
|---|---|---|
| corner | clear color (26,26,51) | (26,26,51) |
| empty area right of triangle | clear | (26,26,51) |
| near the top vertex | blue (13,13,230) | (13,13,229) |
| near the bottom-left vertex | red (223,19,13) | (223,20,12) |
| near the bottom-right vertex | green (19,223,13) | (19,224,12) |

The vertex colors are red/green/blue at bottom-left/bottom-right/top, so the same readback proves orientation is GL-compatible: Y up, counter-clockwise
front faces, and back-face culling on, with no flip needed in the vertex data or the shaders.

Counters from the run: 3 draw calls, **1 pipeline created** (the pipeline cache hit twice), 27 state calls (9 per frame), 72 bytes of vertex data.

## What the interfaces proved they can do

- A buffer is allocated through `allocate()`, written through its CPU copy, and uploaded with `upload()`; the Vulkan backend needs nothing GL-specific.
- The vertex layout survives the move: `setBuffers(vb, ib, attribMask, offsets)` is enough to build the vertex input. LLVertexBuffer stores each attribute
  as its own array inside one buffer, which maps to one Vulkan vertex binding per attribute, bound at `offsets[type]`. Binding index = attribute location =
  the `AttributeType` value, which the viewer's shaders already use.
- **Recording state eagerly and resolving a pipeline at draw time works** (the design bet in `pipeline-state-abstraction.md`). Distinct state combinations
  map to pipeline cache entries; viewport and scissor are dynamic.
- GL viewport and scissor rectangles (origin bottom-left) work unchanged with a negative-height viewport (core since Vulkan 1.1) and a converted scissor.

## Gaps the skeleton exposed (in order of importance)

1. **There is no "current shader" in the interfaces.** In GL it is global state (`LLGLSLShader::sCurBoundShader`), so none of the interfaces mention it. A Vulkan
   pipeline needs the shader modules, so the skeleton injected them through `VkContext::vertexShader/fragmentShader`, a side channel that would not exist in
   the viewer. The shader layer needs its own backend interface (bind program, set uniforms) and the pipeline key needs a shader id.
2. **There is no frame, command buffer or render target concept.** The backends needed `ctx.cmd` and an active dynamic-rendering scope. GL has an implicit
   context and an implicit current framebuffer. A Vulkan pipeline also needs the attachment formats (`VkPipelineRenderingCreateInfo`), so the pipeline key must
   include the render target's formats. The framebuffer layer (`LLRenderTarget`, `bindTarget`) has to define begin/end rendering.
3. **Depth, stencil and polygon offset were recorded but could not apply**: the skeleton has no depth attachment. The pipeline has to know whether a depth
   attachment exists, and polygon offset needs a depth bias in the pipeline. Untested here.
4. **Clip space depth range is not tested.** GL clip depth is -1..1, Vulkan's is 0..1. The viewer's projection matrices and shaders assume GL. With no depth
   buffer in the skeleton this did not show up, but it will for every shader. Options: change the projection and shader math, or use a clip-control extension
   where available. This is the biggest open risk for the shader layer.
5. **The vertex attribute vocabulary is duplicated.** `AttributeType`, `sTypeSize[]` and the per-attribute formats live inside the `LLVertexBuffer` class, so a backend
   that does not link the viewer had to repeat them (three tables in `vkbackends.cpp`). When the real backend is written, move them to a light header like
   `llvertexbuffertypes.h`.
6. **GL constants are the state vocabulary.** The backend needed `<GL/gl.h>` for constants such as `GL_BLEND`. The Windows SDK header has every constant used,
   so it worked, but a Vulkan backend still depends on GL header constants. Revisit when the last GL dependencies are gone (decision recorded in
   `pipeline-state-abstraction.md`).
7. **Primitive types without a direct Vulkan equivalent.** `LLRender::LINE_LOOP` has none (needs an index rewrite): 6 uses in `llrender2dutils.cpp`,
   `fsmaniprotatejoint.cpp`, `gltf/primitive.cpp` and `llmodelpreview.cpp`. `TRIANGLE_FAN` (15 uses) is core Vulkan but not available on every device (for
   example portability implementations). `POINTS` (12 uses) needs `gl_PointSize` written by the vertex shader.
8. **GL-only rules in the interface.** `upload()` requires the buffer to be bound (asserted in the GL backend). Vulkan does not care; the skeleton counted 0
   violations only because it followed the rule. `drawIndexed(start, end, ...)` passes a min/max vertex hint that Vulkan has no use for.
9. **Memory.** One host-visible allocation per buffer and a `vkDeviceWaitIdle()` on free is fine for a triangle only. A real backend needs a sub-allocator (VMA),
   device-local memory with a staging copy, and fence-based deferred deletion. The GL backend's 4-frame delete queue is the hint for where that goes.

## What it does not prove

- Textures, uniforms (descriptor sets), render-to-texture, depth testing, MSAA, resizing, or more than one frame in flight. None of them is in the skeleton.
- Performance. Nothing here was measured beyond "it draws".
- That the viewer's real shaders work. They are GLSL for GL (several use features such as loose uniforms) and need translation.

## Suggested next steps

1. Design the **shader layer** (gap 1 and 4): GLSL to SPIR-V with `glslc`/`glslang`, uniforms to descriptor sets or push constants, and the clip-space depth fix.
2. Design the **framebuffer layer** (gap 2 and 3): begin/end rendering, attachment formats in the pipeline key, depth.
3. Extend the skeleton with those (a textured, depth-tested, uniform-driven quad) before touching more viewer code. Each addition tells us what the next
   interface must express, which is cheaper than finding out inside the viewer.


## Step 4: a real viewer shader runs (2026-10-12)

The skeleton now has a second scene, `ui`: the viewer's own **"UI Shader"** program, taken from the shader dump, merged and translated by
`LLShaderTranslator`, compiled by glslang (`LLShaderCompiler`, both in `indra/vulkan_skeleton/translator/`), drawn on the RX 6700 XT with a depth
buffer, a 2x2 texture, the program's uniform block and `VK_EXT_depth_clip_control`. Two quads in GL clip space: the NEAR one (z = -0.5) is drawn first, the
FAR one (z = +0.5) second, so only the depth test can keep the near one on top where they overlap.

Run: `vulkan_skeleton.exe --test --dump C:\phoenix-firestorm\shaderdump` (runs both scenes). `--scene ui`, `--program "<manifest name>"`, `--no-depth-clip-control`.

| Result | |
|---|---|
| Both scenes, validation layer on | **PASS, 0 validation errors, 0 warnings** |
| 4 texels of the near quad (red, green, blue, yellow), vertex color white | exact (255,0,0) (0,255,0) (0,0,255) (255,255,0) |
| Overlap region | the near quad (255,0,0), not the far quad (128,128,0): depth test + GL clip depth work |
| Far quad alone (texel x vertex color 128) | exact (128,0,0) and (0,0,128) |
| `--no-depth-clip-control` (control) | near quad is **clipped**, the overlap shows the far quad: this is what GL's -1..1 clip depth does without the extension |

What this proves: the viewer's attribute table, vertex formats (including the normalized `LLColor4U`), the program uniform block (`mat4` x2 written by name at
CPU-computed std140 offsets), the sampler at `set 1 binding 0`, texture coordinates, the texture matrix, depth state, and **GL's clip depth range via
`VK_EXT_depth_clip_control`, with no change to any shader or matrix**. Gap 4 of this document is closed for this GPU; a device without the extension still
needs the fallback (adjust the projection matrices and the depth reconstruction in the shaders).

Things step 4 found (all fixed):

- **glslang's auto-mapped varying locations do not agree between stages** when the stages declare their varyings in a different order. `spirv-val` and the
  compile/link checks of step 3 did not notice (424/424 passed); the **Vulkan validation layer** did, at pipeline creation. `LLShaderTranslator` now gives every
  stage-interface variable an explicit `layout(location = N)`, one N per name for the whole program, and `translator_test` checks it on all 210 programs.
- `out vec4[2] name;` (array on the type) was not recognized as a varying; same for uniforms (`uniform vec4[2] name;`, fixed in step 3).
- Block layout: `computeBlockLayout()` (std140, CPU side) is compared with glslang's reflection for every program: 0 mismatches. glslang drops members no stage
  uses, and the whole block when none is used (block size -1); the check skips those.

Not covered yet: more than one program, a texture change between draws of one program (the descriptor sets are allocated per frame from a reset pool, one set per
distinct binding), arrays of samplers, geometry shaders on the GPU, `uniform bool`/`bvec2` members, the two shaders that need subgroup / int64 device features,
`LINE_LOOP`, `gl_PointSize`, resizing, more than one frame in flight, and the program/uniform entry points of `LLShaderBackend` itself (the skeleton sets
`ctx.program` directly, the stand-in for `LLShaderBackend::use()`).


## Step 5: a Vulkan `LLShaderBackend` (2026-10-12)

`VulkanShaderBackend` (`indra/vulkan_skeleton/vkshaderbackend.{h,cpp}`) implements the viewer's real `LLShaderBackend` interface. The skeleton drives it with the
same call sequence `LLGLSLShader::createShader()` / `mapAttributes()` / `mapUniforms()` use against the GL backend (`create_program()` in `main.cpp`).

How the GL-shaped interface maps to Vulkan:

| Interface call | Vulkan backend |
|---|---|
| `createObject` / `compileObject` / `attachObject` | stores the object's source and stage; Vulkan compiles whole stages, so nothing is compiled yet |
| `bindAttribLocation` | builds the program's attribute table (location = index), which is given to the translator |
| `link` | `LLShaderCompiler` (glslang preprocess, `LLShaderTranslator`, glslang compile + link) then `VkProgram::create` (shader modules, descriptor set layouts, pipeline layout) |
| `getActiveUniformCount` / `getActiveUniform` / `getUniformLocation` | answered from the translator's description: samplers first (binding order), then the block members; arrays reported as `name[0]`, GL type constants (`GL_FLOAT_MAT4`, `GL_SAMPLER_2D`, ...) |
| `getAttribLocation` | index in the attribute table if the vertex stage reads the name, else -1 |
| `uniform1i` on a sampler | sets the texture unit the sampler reads (`VkProgram::samplerUnit`); `VkContext::unitTextures[unit]` is what the texture layer will fill |
| `uniform*` / `uniformMatrix*` | typed writes into the program's CPU copy of the std140 block (`llstd140pack.h`: matrix columns on 16 byte boundaries, array stride 16, transpose, excess ignored); every draw snapshots the block into the ring buffer |
| `use` | sets `VkContext::program` (the "current program" gap found by the first skeleton) |
| `getProgramBinary` / `loadProgramBinary` | a **SPIR-V blob** (reflection + std140 layout + both SPIR-V modules, 3.5 KB for the UI shader): the SPIR-V cache of the design, through the hooks `LLShaderMgr` already has for GL program binaries |

Results on the RX 6700 XT, validation layer on, **0 errors, 0 warnings**:

- `--test --dump <dump>`: triangle, UI shader compiled from the dump, UI shader loaded from the blob (no GLSL, no glslang): all pixel-exact, same as step 4. Reflection checks pass (5 block members + 1 sampler active, `GL_FLOAT_MAT4`, attribute locations).
- `--link-all --dump <dump>`: every program of the dump without a geometry stage goes through the backend interface on the real device: **206 / 206 link, 206 / 206 reflection consistent, 206 / 206 blob round trips give identical reflection**.
- Cost: cold compile **31 ms per program** (6.3 s for 206; the first program pays about 230 ms for glslang start-up), blob load **0.6 ms per program** (0.12 s for 206, 4.8 MB of blobs). Most programs share the same few hundred KB of library code, so a warm cache turns shader start-up from seconds into a fraction of a second.
- `llstd140pack.h` is unit-tested without a GPU in `translator_test` (mat3 columns, transpose, float and vec3 arrays, mat3x4, int, clamping).

What `--link-all` found (device setup, now fixed): glslang turns `discard` into `OpDemoteToHelperInvocation`, so the device needs the Vulkan 1.3 `shaderDemoteToHelperInvocation` feature; `samplerCubeArray` (reflection probes) needs the core feature `imageCubeArray`; the static descriptor pool must free the sets of deleted programs. At most **17 samplers** in one program (one more than the guaranteed minimum of 16 per stage; this GPU has no practical limit). Geometry stages (4 programs) are not supported by `VkProgram` yet.

Not covered: the 4 geometry programs, running anything but the UI shader on the GPU (the others only link and reflect), sampler arrays, `uniform bool`/`bvec2`/`uvec4` writes on the GPU, the shaders that need subgroup / int64 features, cache invalidation (the blob has a format version; the key is the viewer's existing `mShaderHash`, which does not include the translator or glslang version), and the viewer build: the translator, compiler and backend still live in `indra/vulkan_skeleton`, outside the viewer, and need a glslang 3p package before `llrender` can contain them.


## Texture step 4: a Vulkan `LLTextureBackend` (2026-10-12)

`VulkanTextureBackend` (`indra/vulkan_skeleton/vktexturebackend.{h,cpp}`) implements the viewer's real `LLTextureBackend` interface. It replaces `VkContext::unitTextures`: the backend owns "texture bound to unit N" and `VkProgram::prepareDraw()` asks it for an image view and a sampler per sampler uniform (`unitDescriptor(unit, samplerType)`).

How the GL model maps to Vulkan:

| GL | Vulkan backend |
|---|---|
| texture name | a map entry; the `VkImage` is created when level 0 is defined, with the **full mip chain** allocated and cleared to zero (GL levels start as zeros) |
| `glTexImage2D` / `SubImage2D` / `3D`, `CompressedTexImage2D` | CPU conversion to the Vulkan format (RGB to RGBA, float to half, BGRA, ...), staging buffer, `vkCmdCopyBufferToImage`; row length and byte swap are applied during the conversion |
| `glGenerateMipmap` | a `vkCmdBlitImage` chain (linear filter) over all layers |
| wrap / filter / anisotropy / compare (state of the texture) | stored on the texture, turned into a `VkSampler` from a cache when a draw needs it (a min filter without mips becomes `maxLod = 0.25`) |
| base / max level, swizzle | a `VkImageView` from a cache keyed by (base, last, swizzle): the luminance / alpha emulation of `LLImageGL` is the view's component mapping |
| `GL_COMPRESSED_RGBA` (driver compression request) | stored uncompressed (design decision 2); pre-compressed `GL_COMPRESSED_*_S3TC_*` data is uploaded as BC1 / BC2 / BC3 |
| cube maps, cube map arrays | one image with 6 (or 6 x count) layers; `image2D` with a face target and `image3D` / `subImage3D` on `GL_TEXTURE_CUBE_MAP_ARRAY` |
| `glGetTexImage`, level queries, `replaceWithMipLevel` | copy to a host buffer and convert back; sizes from the image; a new image plus `vkCmdCopyImage` |
| a unit with no texture, or the wrong kind for the sampler | a 1x1 white fallback texture (2D, cube, cube array), counted in `kindMismatches` |

Test (`--test --dump <dump>`, scene `textures`): 33 checks, all PASS, validation layer **0 errors, 0 warnings**, on the RX 6700 XT. Only `LLTextureBackend` is used to build the textures.
Pixels drawn with the viewer's UI shader: each of four hand-uploaded mip levels is chosen by the size of the quad on screen; `generateMipmaps` gives (128,0,128) at level 3 and the correct halves at level 2; point vs linear magnification (143,112,0 for the linear blend); REPEAT / CLAMP / MIRROR at two coordinates each; RGB bytes in an RGBA image; R8 with a luminance swizzle; a BC1 block; fallback to white for an unbound unit and for a cube map read by a 2D sampler. Pixels drawn with a `samplerCubeArray` shader (built from GLSL text through `LLShaderBackend`): layers 0 and 1, four different faces, exact colors. API checks: `readImage` round trip, `subImage2D` with a source row length, level queries, RGBA16F float round trip, BC1 readback, `replaceWithMipLevel`.
`--link-all` and the shader scenes are unchanged (206/206, UI shader from compile and from the cache).

Not covered: the shadow sampler (`sampler2DShadow` / `setCompare`; needs a depth texture, so the framebuffer layer), 3D textures, texture rectangles in shaders, multisampling, render target attachments, `copyFromFramebuffer*` (abort with a message), swap-bytes (a 16 bit or wider type), sRGB sampling, the CPU cost of the conversions for big textures, and threaded uploads. Every edit is a blocking one-time submit and textures cannot be edited while a frame is being recorded; a real backend needs a transfer queue, a staging ring and fence-based deletion. Skeleton quirk: a draw with no vertex attribute for something the pipeline's vertex input declares gives a validation warning; the viewer passes the program's own attribute mask so it will not happen there.

## Current continuation: 2026-10-12 framebuffer layer

Work is **uncommitted on local branch `vulkan-framebuffer`**, based on `8a8d32790f`.
Master and the remote have not been changed. The existing `.gitignore` and `.ignore` changes were preserved.

### Implemented

- `LLFramebufferBackend` and the GL implementation in `llrender`; framebuffer calls in `LLRenderTarget`, clears,
  readers and the raw framebuffer helpers now go through it. Only the window startup clears remain direct GL.
- Binding ownership moved from `LLRenderTarget::sCurFBO` to the backend. Temporary attachment binds save the actual
  draw framebuffer and restore it. `sCurResX/Y` remain render-target viewport bookkeeping, independent of the API.
- The interface exposes `checkStatus()` instead of a boolean so the existing specific framebuffer error messages survive.
  `attach(fb, ...)` requires `fb` to be bound, preserving the bind/attach/check/restore order and GL diagnostics.
- Standalone Vulkan framebuffer backend: lazy rendering scopes, first-clear load operations, bounded scissor clears,
  attachment-format pipeline keys, MRT, depth-only/shared depth, synchronous pixel readback, and the three texture-copy
  operations previously marked unsupported. Texture image layouts are tracked by the texture backend.
- Offscreen images use GL's bottom row as texture row zero; viewport and winding differ from the presentation target.
  Copying the presentation target flips rows and converts its BGRA format. Both directions are tested.
- Texture edits made during a frame submit pending recording before editing or replacing resources. Pipeline/descriptor
  binds happen after fallback texture resolution, since a fallback allocation can flush the recording.
- A `--headless` test path uses a real BGRA GPU image as framebuffer 0. It enables rendering tests without a Win32
  presentation surface; it does not verify acquisition or presentation.
- Regression tests in `indra/vulkan_skeleton/framebuffer_test.cpp`: 39 checks, including all seven sized viewer color
  formats, depth sampling, unsigned-integer snapshot depth, nested bindings, array-layer copies, shared depth, and clear
  state persistence. Test CLI rejects unknown scenes and nonpositive frame counts.

### Captured verification

- Full viewer build: `BUILD_EXIT=0`, zero `error C` / `error LNK`, `firestorm-bin.exe` updated at 11:28 local time.
  GL steps 1–3 were built together; there were no separate intermediate commits or builds.
- Final skeleton build succeeded. The existing C4805 bool/int warning in the pipeline color-mask expression remains.
- `vulkan_skeleton.exe --headless --test --dump C:\phoenix-firestorm\shaderdump`: **96/96 pixel/state checks passed**,
  39 of them framebuffer checks; 85 draws, nine pipelines, **0 validation errors, 0 warnings**, `RESULT: PASS`.
- `vulkan_skeleton.exe --headless --link-all --dump C:\phoenix-firestorm\shaderdump`: **206/206** programs link,
  reflect and round-trip their SPIR-V blobs; four geometry-stage programs remain unsupported. Zero validation messages.
- Existing `translator_test.exe C:\phoenix-firestorm\shaderdump`: self-check passed, **210/210** programs link,
  **424/424** stages compile and validate, zero uniform layout mismatches, `RESULT: PASS`.
- Mutation check: changing depth-clear clamping to always zero made the GPU depth-only/shared-depth tests fail.
  Source was restored, rebuilt and the full suite passed. This checks that the tests reject incorrect behavior.
- `git diff --check` passed. Viewer GUI was never launched; no login occurred.

### Display failure and verification limits

The windowed skeleton currently fails at `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` with `VK_ERROR_UNKNOWN` (-13).
An isolated build of the unchanged HEAD skeleton reproduces the same failure, with and without validation.
The cause is not established. A desktop lock/disconnection was asked about, but has not been confirmed.
Headless tests use the physical RX 6700 XT and the validation layer, but **windowed presentation remains unverified**.
One early invocation accidentally ran the previous executable before compilation completed and performed zero draws;
its PASS was explicitly rejected as evidence. Final evidence comes from the freshly built headless executable.

Other skeleton limits: one frame in flight; whole-image layout tracking; blocking uploads/readbacks; framebuffer copies
require an active frame and use a supported nearest blit for format conversion/orientation; overlapping self-copies and
compressed destinations are rejected. True 3D textures, stencil, front-buffer reads and multisampling are not added.
The current attachment textures and their framebuffers must outlive commands that use them. The implementation is still
outside the viewer's Vulkan path; it must not be enabled as a production viewer backend yet.

### Next steps — full integration is NOT complete

1. User must run the newly built OpenGL viewer in-world, per this handoff's launch restriction. Check terrain, avatars,
   UI, shadows/PBR, preview windows, avatar baking and snapshots. Earlier pipeline/shader/texture refactors also still
   lack this user runtime verification. Do not launch the viewer unattended.
2. Diagnose the windowed baseline's display-surface failure and rerun all scenes with the actual swapchain.
3. Design and implement the viewer window/device lifetime: backend choice before any backend getter is used, swapchain
   acquire/present/resize, synchronization, frames in flight, fence-based resource deletion, and remaining GL queries.
4. Package glslang for autobuild with its licenses, move the translator/compiler/backends into the viewer, support geometry
   stages and required device features, and version shader-cache keys by translator/compiler version.
5. Integrate and compare UI, world/terrain, avatars, water/sky, deferred/shadows/PBR and bake/snapshot paths against GL;
   address unsupported primitives/point size/stencil/polygon offset, then measure real-world performance.
6. Commit/PR/push/merge only when explicitly requested. The current worktree is intentionally uncommitted.

Commands for the next session:

    doc\vulkan\tools\build-skeleton.cmd
    build-vulkan-skeleton\Release\vulkan_skeleton.exe --headless --test --dump C:\phoenix-firestorm\shaderdump
    build-vulkan-skeleton\Release\vulkan_skeleton.exe --headless --link-all --dump C:\phoenix-firestorm\shaderdump
    build-vulkan-skeleton\Release\vulkan_skeleton.exe --test --dump C:\phoenix-firestorm\shaderdump

The last command still needs a working display surface. Test logs are saved in
`C:\Users\TheTaZe\Documents\Codex\2026-10-12\look\outputs`.

