# Shader layer: findings and design

Status: design APPROVED (glslang in-process + SPIR-V cache, 2026-10-12). Steps 1 to 4 done, see the end of this file.  Read `PROJECT-NOTES.md` first.
Tools and how to reproduce: `indra/vulkan_skeleton/shader_probe/README.md` (committed on branch `shader-layer`).

## The question

The Vulkan skeleton (`vulkan-skeleton-findings.md`) showed the buffer and state interfaces are enough to draw, and that the interfaces have no notion
of a current shader. Before designing one, the risk to measure was: **can the viewer's real GLSL become Vulkan SPIR-V with only mechanical
source transformations, or does it need rewriting?**

## How the viewer builds a shader today

- 224 `.glsl` files under `newview/app_settings/shaders`. None contains `#version`.
- `LLShaderMgr::loadShaderFile` (`llshadermgr.cpp`) assembles each shader object's source in C++: `#version 420` (or 400/330/150 by GL version),
  per-program `#define`s, `FRAGMENT_SHADER`/`VERTEX_SHADER`, G-buffer flag defines, a generated `diffuseLookup()` for indexed textures, a `struct GBufferInfo` line
  in every object, `#include` expansion, and a few vendor tweaks. Then `glShaderSource` / `glCompileShader`.
- A program is built from several objects per stage and linked by GL (`glAttachShader` for the program's own objects, plus library objects attached by
  `attachShaderFeatures`).
- Attributes are bound by name to fixed locations (`glBindAttribLocation`, order of `mReservedAttribs`). Uniform locations and texture units are
  found by reflecting the linked program at runtime (`mapUniforms`: `glGetActiveUniform`, samplers get the next free texture unit).
- Uniform setters (`uniform1f` ...) keep a CPU-side value cache (`mValue`) and call `glUniform*`.

## Measured with the viewer's real shaders

Method: the viewer was run with the developer hook (`FS_SHADER_DUMP_DIR` + `FS_SHADER_DUMP_EXIT`, see the README) to dump the exact text it sends to GL, and a
manifest of which objects each program attached. Then the probe tools in `shader_probe/`.

| Measurement | Result |
|---|---|
| Shader objects the viewer compiles at startup | **484** (238 vertex, 242 fragment, 4 geometry) |
| Programs / program stages | **210 programs, 424 stages** (210 vertex, 210 fragment, 4 geometry) |
| Stages built from **2 or more objects** | **418 of 424 (99%)**, typically 3 to 13 objects; a few library objects are attached to over 100 programs (`textureUtilV`, `globalF` to 209 each) |
| Control: each object validates as plain GLSL | 484 / 484 |
| Vulkan, each object compiled alone, source untouched | 70 / 484 |
| Vulkan, each object compiled alone, after mechanical translation | 261 / 484 (the rest are library objects that call functions defined in other objects) |
| **Vulkan, each program stage merged from its objects, then translated** | **424 / 424 compile** |
| SPIR-V validity (`spirv-val`, Vulkan 1.3) | **424 / 424 valid** |
| **Programs linked across stages (vertex outputs matched to fragment inputs by name)** | **210 / 210** |

In other words: **no shader needed to be rewritten.** Everything the experiment needed was done by a source-to-source pass.

An earlier intermediate number in this investigation said 94% of stages use a single object. That was wrong: it came from the dump hook missing the library
attach path (`attachVertexObject`/`attachFragmentObject` call `glAttachShader` directly). The numbers above are from the corrected hook.

### What the source pass has to do (all of it was needed)

1. **Merge the objects of each stage into one source.** GL links separate objects; a Vulkan stage is one SPIR-V module. The experiment preprocessed each object
   (`glslangValidator -E`, which resolves `#if`/`#define`), then concatenated and dropped repeated top-level declarations (identical declarations; structs by name,
   because every object carries its own copy of `GBufferInfo`). Doing the merge **before** preprocessing failed for 18 stages, where the same uniform sits in two
   objects under different `#if` branches. Preprocess first.
2. **`#version 450`** (the viewer emits 420/400/330/150).
3. **Loose uniforms become uniform blocks.** Vulkan GLSL does not allow non-opaque uniforms outside a block. The source has 686 loose uniform declarations (8,095 in the
   merged stages, counting repeats). Per merged stage: vertex median 4 (max 30), fragment median 37 (max 58).
4. **Remove legacy no-op `#extension` lines** (81 of them: `GL_ARB_shader_texture_lod`, `GL_EXT_gpu_shader4`, `GL_ARB_texture_rectangle`).
5. **Bindings and locations.** Samplers and blocks need `binding`; inputs/outputs need `location`. The experiment let glslang assign them
   (`--auto-map-bindings --auto-map-locations`); for the real thing see the open points.

Things that turned out NOT to be a problem: `discard`, `texelFetch`/`textureLod`, `in`/`out` qualifiers, the 5 existing `layout(std140)` blocks (glTF, reflection probes),
`flat` varyings and the geometry shader. Two shaders in the dump use `GL_KHR_shader_subgroup_*` and `GL_NV_shader_atomic_int64`; they compile, but running them
needs the matching Vulkan device features at pipeline creation (untested).

## What the measurement does NOT prove

- **Nothing was run.** Compiling and linking is not rendering correctly. The skeleton has to be extended to execute real translated viewer shaders before anyone
  believes the pass is right.
- **Depth range.** GL clip depth is -1..1, Vulkan's is 0..1. The viewer's shaders reconstruct positions from depth. This GPU supports
  `VK_EXT_depth_clip_control` (queried with `vulkaninfo`), whose `negativeOneToOne` makes Vulkan use GL's convention, which would leave every shader untouched.
  Not tested yet. Fallback if an extension is unavailable: fix the projection matrices and the position-reconstruction code in the shaders.
- **Attribute locations.** The viewer binds attributes by name to fixed locations (position = 0, normal = 1, ... = `AttributeType`). The experiment used auto
  locations. The real pass must inject `layout(location = N)` by name so the shaders agree with the vertex input the buffer backend builds.
- **One block per uniform is an experiment artifact, not the design.** It produced up to 58 uniform blocks per stage; Vulkan only guarantees 12 per stage
  (this GPU allows far more). See the design below.
- **Samplers.** Up to 17 sampler declarations in one fragment stage (median 5). The spec's guaranteed minimum for a stage is 16 sampled images, so this is above the
  portable minimum by one; real GPUs allow vastly more (this one reports no practical limit). Texture unit assignment is a runtime decision in the viewer today.
- **Compile time and permutations.** The viewer compiled all 484 objects in about 2 seconds here (from `initShaderCache` to the last shader; the GPU driver keeps its own
  shader cache, so a cold compile may be slower). glslang plus the translation pass will cost more than that; not measured. Programs are built from per-program `#define`s
  that can change with settings, so the set of shaders is not fixed at build time.

## Design

### One interface, two implementations (same pattern as the other layers)

`LLShaderBackend` (new interface), `LLGLShaderBackend` (today's behavior, moved), `LLVulkanShaderBackend` (new).

    createProgram(sources per stage: list of objects)  -> ProgramHandle + ProgramInfo
        GL:     today's compile / attach / link (unchanged), ProgramInfo from glGetActiveUniform and friends
        Vulkan: translate (below), compile to SPIR-V, reflect SPIR-V, ProgramInfo from the reflection
    bind(program), destroy(program)
    setUniform(program, uniform, type, count, data)          // GL: glUniform*, Vulkan: write into the program's uniform block shadow
    ProgramInfo { uniform name -> {location | offset, type, size}, sampler name -> binding, attribute mask }

`LLGLSLShader` keeps its public API (the roughly 450 call sites of `uniform1f` and friends in `newview` and `llrender`, plus `bind`, do not change). It stops calling GL directly.
The CPU-side value cache it already has is exactly what a Vulkan backend needs.

### The translator is its own module with no GL dependency

`LLShaderTranslator`: input = the objects of one program stage as the viewer assembles them; output = one Vulkan GLSL source plus a description of what it changed.
It is the C++ version of what `shader_vk_probe.py` / `shader_vk_link_probe.py` do. The corpus dumped by the hook becomes its regression test: **all 424 stages must compile, all 210
programs must link**, run without a GPU.

Real design choices that differ from the probe:

- **One uniform block per program, not per uniform.** The union of all loose uniforms of all stages of the program, declared identically in every stage
  (std140, same member order). Names stay valid in the shader code because the block has no instance name. Fits the 12-block limit easily and makes
  "uniform declared in two stages" the same member in both, as in GL.
- **Per-draw uniform data.** The viewer changes uniforms between draws (matrices on `syncMatrices`, colors, material parameters). Plan: keep each program's block as a CPU
  shadow, and at draw time copy a snapshot into a per-frame ring buffer and bind it with a dynamic offset. Up to 256 bytes (this GPU's `maxPushConstantsSize`) of the hottest data
  (the two matrices) could use push constants later if measurement says so.
- **Sampler bindings are the texture unit.** `layout(binding = N)` where N is the texture unit the viewer assigns today in `mapUniformTextureChannel`. The translator must
  reproduce the same assignment order so `gGL.getTexUnit(n)->bind()` keeps meaning the same thing. The descriptor set holds one combined image sampler per unit.
- **Attribute locations are injected from the reserved attribute table.**
- **Depth range via `VK_EXT_depth_clip_control` first** (see above), shader math second.

### Where this plugs into the existing layers (gaps found by the skeleton)

- The pipeline key gains the **program id** (skeleton gap 1). The vertex buffer backend needs to read the current program from the shader backend.
- Descriptor set layouts come from the program reflection, so pipeline layouts are per program.
- Texture binding goes through the same descriptor set: the **texture layer** (`LLTexUnit`, `LLImageGL`) is the next dependency after this one.

### Compile strategy (decision for the user)

Programs are built from `#define`s decided at runtime, so translation cannot be fully ahead of time. Two options:

1. **glslang as an in-process library** and a SPIR-V cache keyed by a hash of the merged source (the viewer already caches linked GL program binaries the same way). Needs a new
   third-party package in the viewer's dependency system.
2. **Offline compile of the default configuration with `glslc`** at build time, runtime fallback only for unusual settings. No new runtime dependency, but two paths to maintain.

Recommendation: option 1 for correctness and a single path; option 2 only if the dependency is unacceptable.

## Steps (each a separate, buildable commit; GL behavior must not change until step 5)

1. **Done:** dump hook, probe tools, measurements (this document).
2. Extract `LLShaderBackend` and move the GL calls (about 43 program/shader-object call sites and 58 uniform call sites in the catalog) into `LLGLShaderBackend`. No behavior change.
3. Port the translator to C++ as `LLShaderTranslator` with the corpus test (424 stages compile, 210 programs link). No viewer behavior change; it is not called by the viewer yet.
4. Extend the skeleton: run a real translated viewer program (start with a simple fullbright one) with a depth buffer, a texture and the uniform block, with
   `VK_EXT_depth_clip_control` on. This is the first time a viewer shader executes under Vulkan.
5. `LLVulkanShaderBackend` and reflection into `ProgramInfo`.

## Risks

- Step 4 is where assumptions break (depth convention, uniform layout, sampler order). Do it before step 5, not after.
- The merge/dedupe rules are heuristic on source text. The corpus test is the guard; any new shader in the viewer must pass it.
- Third-party viewer policy and licensing for glslang (Apache-2.0 / BSD-style): check before adding it as a 3p package.


## Status after step 4 (2026-10-12)

- Step 2 done and merged (PR #9): `LLShaderBackend` (`llshaderbackend.h`) with the GL implementation; `LLGLSLShader`, `LLShaderMgr`, `LLPostProcess`, `LLDrawPoolMaterials` no longer call GL program/uniform functions.
- Step 3 done and merged (PR #10): `LLShaderTranslator` (`indra/vulkan_skeleton/translator/`). 210/210 programs, 424/424 stages compile, link and validate, run by `translator_test`.
- Step 4 done: the viewer's "UI Shader" runs on the GPU with a texture, the uniform block, depth and `VK_EXT_depth_clip_control` (`vulkan-skeleton-findings.md`, section "Step 4").

Differences between this design and what was built:

- Stage interface (varying) locations are assigned by the translator, by name for the whole program, not left to glslang's auto-map (which pairs by declaration order per stage). Fragment outputs and anything not in the attribute table are still auto-mapped.
- Descriptor sets: set 0 = the program uniform block (one dynamic uniform buffer, per-draw snapshot in a 1 MiB ring, reset every frame), set 1 = one combined image sampler per sampler, binding in order of first appearance; existing uniform blocks (`ReflectionProbes`, ...) are set 2 with the `UniformBlock` enum as binding (translated, but not yet run on the GPU).
- `LLShaderTranslator::computeBlockLayout()` gives the std140 offsets for the CPU-side shadow; it replaces reflection for the block (it is checked against glslang's reflection in `translator_test`).
- The compile path (preprocess each object with glslang, translate, compile, link, SPIR-V) is `LLShaderCompiler`; it is the prototype of what `LLVulkanShaderBackend::createProgram()` will do (step 5), with the SPIR-V cache in front of it.
- The translator and compiler are still outside the viewer build (standalone, glslang from the Vulkan SDK). Step 5 moves them into llrender and needs a glslang 3p package (license check still open).

Next (step 5): `LLVulkanShaderBackend` implementing `LLShaderBackend` (createProgram from the viewer's objects, `use()`, uniform setters into the shadow, `getActiveUniform`-style reflection from the layout and the sampler list), then the texture layer.


## Status after step 5 (2026-10-12)

`VulkanShaderBackend` implements `LLShaderBackend` in the skeleton and is driven like `LLGLSLShader` drives the GL backend: 206/206 programs link, reflect and round-trip through the SPIR-V blob on the GPU, the UI shader renders pixel-exact from a fresh compile and from the blob. Details: `vulkan-skeleton-findings.md`, section "Step 5".

Remaining before the shader layer can go into the viewer: (a) a glslang 3p package (autobuild) and its license check, then move `LLShaderTranslator`, `LLShaderCompiler`, `llstd140pack.h` and `VulkanShaderBackend`'s program logic into `llrender`; (b) a choice of backend at start-up (`getShaderBackend()` is created as GL on first use today); (c) geometry stages; (d) cache key must include the translator/glslang version; (e) draw more programs on the GPU, starting with the deferred ones. After the shader layer: the texture layer (`LLTexUnit`, `LLImageGL` fill `VkContext::unitTextures`), then the framebuffer layer (`LLRenderTarget`), then the real Vulkan device and swapchain in the viewer.
