# Texture layer: survey and design (DRAFT, needs the user's approval before step 1)

Read `PROJECT-NOTES.md` first. Same method as the vertex buffer, pipeline state and shader layers: put every texture GL call behind a backend interface with a
behavior-neutral OpenGL implementation, then write a Vulkan implementation in the skeleton and test it on the GPU.

## What exists today (measured on this tree)

About **120 GL call sites in 17 files** (`glcatalog.py`: textures / samplers / images 110, plus the `glGetTex*` readbacks):

| Where | Calls | What it does |
|---|---|---|
| `llrender/llrender.cpp` (`LLTexUnit`) | 28 | `glActiveTexture`, `glBindTexture` (14), `glTexParameteri/f` for wrap, min/mag filter and anisotropy, `glPixelStorei` alignment |
| `llrender/llimagegl.cpp` | 45 | texture names (`glGenTextures` in a pool of 1024 per thread, `glDeleteTextures` after a 3 frame delay), `glTexImage2D` / `glCompressedTexImage2D` / `glTexSubImage2D` (rows batched to keep GL commands small), `glPixelStorei` row length + swap bytes, `glGenerateMipmap`, base/max level, swizzle for the deprecated `GL_ALPHA` / `GL_LUMINANCE` formats, `glCopyTexSubImage2D` from the framebuffer, readback (`glGetTexImage`, `glGetCompressedTexImage`, `glGetTexLevelParameteriv`), GPU `scaleDown` |
| `llrender/llcubemap.cpp`, `llcubemaparray.cpp`, `llrendertarget.cpp`, `llpostprocess.cpp` | 12 | cube map mips, `glTexImage3D` / `glTexSubImage3D` / `glGetTexImage` for cube map arrays, mip generation of render targets, texture rectangle |
| `newview/` (gltfscenemanager 10, pipeline 8, llreflectionmapmanager 7, llheroprobemanager 2, llterrainpaintmap, llscenemonitor, lldrawpoolbump, llappviewer) | ~30 | raw `glBindTexture` / `glTexParameteri` on `getTexName()` (glTF samplers, shadow compare mode), `glCopyTexSubImage3D` (framebuffer to cube map array layers), mip generation |
| `llappearance/lltexlayer.cpp` | 3 | `glGetTexImage` readback of the baked avatar texture |

Facts that shape the design:

- **Textures are already `U32` names everywhere** (`getTexName()` is used in 35 places, `bindManual(type, name)` in 45, `LLTexUnit::sWhiteTexture`). Like the program handles of the shader layer, the name can stay a plain `U32` handle: a Vulkan backend hands out ids.
- The viewer already has the neutral vocabulary: `LLTexUnit::eTextureType` (2D, rect, cube, cube array, multisample, 3D), `eTextureAddressMode`, `eTextureFilterOptions`. `LLImageGL` caches the address mode and filter option and `LLTexUnit::bind()` applies them when they changed (`mTexOptionsDirty`): **GL stores sampler state on the texture; the viewer already treats it as a property of the texture.**
- Editing is GL's bind-to-edit model: `LLImageGL::setImage` binds the texture on unit 0 through `LLTexUnit`, then calls `glTexImage2D` on the target, which acts on whatever is bound.
- Textures are **created on a background thread** with a shared GL context (`LLImageGLThread`, setting `RenderGLMultiThreadedTextures`, only used if GL > 3.95): `createGLTexture(..., defer_copy)`, then a fence (`glFenceSync` / `glClientWaitSync`) before the main thread swaps `mTexName`.
- **Compression is done by the driver at upload**: with `RenderCompressTextures` the internal format becomes `GL_COMPRESSED_RGBA` and the driver compresses inside `glTexImage2D`. Vulkan has no such thing.
- Legacy formats (`GL_ALPHA`, `GL_LUMINANCE`, `GL_LUMINANCE_ALPHA`) are emulated with texture swizzle; Vulkan has the same feature as component mapping of the image view.
- Several operations read the framebuffer (`glCopyTexSubImage2D/3D`, 12 sites): they depend on which framebuffer is bound for reading, so they touch the framebuffer layer.

## Design

### One interface, two implementations

`LLTextureBackend` (new `lltexturebackend.h`), `LLGLTextureBackend` (today's calls, moved), later a Vulkan implementation in the skeleton. Texture ids are `U32`, targets and formats keep the GL constants as the vocabulary (as the pipeline state layer did), and the GL "bound texture of the active unit" model stays:

    // names
    generate(count, names)  /  destroy(count, names)                      LLImageGL keeps its pool and its 3 frame delay on top
    // units (what LLTexUnit does)
    activate(unit)  /  bind(target, texture)                              the backend owns "texture bound to unit N"
    // sampler state of the bound texture
    setWrap(target, s, t, r)  setFilter(target, min, mag, maxAnisotropy)  setCompare(target, enabled, func)
    setSwizzle(target, r, g, b, a)  setLevelRange(target, baseLevel, maxLevel)
    // storage and upload into the bound texture
    image2D(target, level, internalFormat, w, h, format, type, data, rowLength, swapBytes)     allocate (+ fill if data)
    compressedImage2D(...)  subImage2D(...)  image3D(...)  subImage3D(...)
    generateMipmaps(target)
    copyFromFramebuffer2D / 3D(target, level, ...)                        reads the current read framebuffer
    // readback
    getLevelSize(target, level, w, h, compressed, bytes)  readImage2D(...)  readCompressedImage2D(...)

Why this shape and not an explicit-handle API (`setImage(texture, ...)`): the GL structure of `LLImageGL` / `LLTexUnit` stays exactly as it is (they already bind through `LLTexUnit`, which caches the binding), so the GL path cannot change behavior and the refactor is mostly moving calls. The Vulkan backend implements "bound texture" as a per-unit table (that table replaces `VkContext::unitTextures` of the skeleton) and the edit calls act on the texture in the active unit. `glPixelStorei` stops being state: row length and swap-bytes become parameters of the upload call (Vulkan's `bufferRowLength` is a parameter too).

### What the Vulkan backend has to do (so the interface does not promise anything it cannot)

- `image2D` with a changed size recreates the `VkImage`; uploads go through a staging buffer; `generateMipmaps` is a chain of `vkCmdBlitImage`; `copyFromFramebuffer*` is `vkCmdCopyImage` from the read target; readback is a copy to a host buffer plus a wait.
- Sampler state lives with the texture and is turned into a `VkSampler` from a cache keyed by the state when the texture is bound (this is the GL model, and `VkSampler` creation is cheap with a cache). Base/max level become the image view's level range; swizzle becomes the view's component mapping.
- Format table: GL internal format to `VkFormat`, including `GL_RGB8` (no 3 component format in Vulkan: expand on CPU) and the deprecated luminance/alpha formats (R8 / RG8 plus swizzle).

### Decisions for the user

1. **Interface model.** Recommended: keep GL's "operate on the bound texture" model as above. Alternative: explicit handles (`image2D(texture, ...)`), cleaner for Vulkan but every `LLImageGL` / `LLCubeMap` upload site changes shape and the GL path is no longer a pure move.
2. **Texture compression under Vulkan.** The driver compression of `RenderCompressTextures` does not exist there. Options: (a) upload uncompressed (simple; about 4x the VRAM for RGBA textures, a real cost in texture-heavy regions), (b) compress on the CPU to BC1/BC3/BC7 (needs an encoder library and CPU time, ideally on the texture worker threads). The GL path is unchanged either way. Recommended: (a) for the first Vulkan version, (b) as a later optimization, once everything draws. Pre-compressed data (`glCompressedTexImage2D`, DDS-style) maps directly to BC formats.
3. **Threaded texture uploads.** Keep them in GL. The first Vulkan implementation runs uploads on the render thread (that is what `RenderGLMultiThreadedTextures` = off does today) and a transfer-queue upload path comes later; the `syncToMainThread` fence belongs to the queries/sync layer.
4. **Framebuffer coupling.** The roughly 12 `glCopyTex*` sites go through the texture backend now, documented as "reads the current read framebuffer"; the framebuffer layer will define what that is. `LLRenderTarget` keeps creating its attachments through `LLImageGL::setManualImage`, which this layer covers.

## Steps (each a separate, buildable commit; GL behavior must not change)

1. `LLTextureBackend` interface + `LLGLTextureBackend`; `LLTexUnit` (activate / bind / sampler parameters / white texture) uses it. About 28 call sites in `llrender.cpp`.
2. `LLImageGL`: names, uploads, mip generation, swizzle, level range, readback, `scaleDown` (the biggest file, 45 sites). `glPixelStorei` becomes upload parameters.
3. The other users: cube maps (4 files, 12 sites), `LLRenderTarget` mip generation, `LLPostProcess`, and the raw calls in `newview` (glTF samplers, shadow compare mode, probe manager, terrain paint map, scene monitor, bump pool, `lltexlayer`).
4. Vulkan texture backend in the skeleton: images, staging upload, sampler cache, mip chain, format table, unit table feeding the shader backend. Test on the GPU: a 2D texture with mipmaps and different filters and wrap modes, a cube map array lookup, a readback, a compressed (BC) upload.
5. (Later, with the framebuffer layer) copy from the framebuffer, render target attachments.

Verification as before: the full viewer build after each step (`BUILD_EXIT=0`). Steps 1 to 3 change every texture the viewer draws, so, like the vertex buffer layer, they need to be run in-world by the user before they are trusted; I will not launch the viewer.

## Risks

- This is the layer where "builds but breaks rendering" is most likely: LLImageGL's upload paths (stagger, swap bytes, row length, alpha/luminance conversion, discard levels) have many branches. Moving calls without touching the branches is the mitigation.
- The `U32` name cannot distinguish a GL texture from a Vulkan one; only one backend is active at a time, so this is fine, but a debug assert on the backend that created a name would catch mixups.
- Texture memory: uncompressed Vulkan textures (decision 2), and GL's implicit mip chains versus explicit allocation in Vulkan (the viewer sets `GL_TEXTURE_MAX_LEVEL` to hide mips below the current discard level; the Vulkan image needs all levels allocated up front or reallocation on discard change).


## Status after step 4 (2026-10-12)

Steps 1 to 3 are merged (PRs #13 and #14). Step 4 done: `VulkanTextureBackend` in the skeleton passes 33 GPU checks with a clean validation layer (`vulkan-skeleton-findings.md`, section "Texture step 4"). Deviations from the text above: pixel store row length and swap bytes stay state setters in the interface; `setMagFilter`, `setCompare`, `image3D`, `subImage3D`, `copyFromFramebuffer3D`, `copyImageFromFramebuffer2D` and `replaceWithMipLevel` were added as the users needed them. Remaining: step 5 (framebuffer copies and render target attachments, with the framebuffer layer), a depth fallback and shadow sampler test, transfer-queue uploads, and CPU-side BCn compression as the Vulkan replacement for `RenderCompressTextures`.
