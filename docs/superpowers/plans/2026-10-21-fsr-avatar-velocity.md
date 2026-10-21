# FSR 2 Avatar Velocity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give moving avatars real per-vertex motion vectors in the FSR 2 motion target, so the temporal upscaler keeps valid history on them and they stop blurring.

**Architecture:** After the deferred G-buffer is filled and `generateFsr2Motion` has written camera motion, a new avatar velocity pass redraws the visible avatars (body, rigged mesh, rigid attachments) with velocity shaders that skin each vertex twice, with this frame's and last frame's bone palette, and write `prevUv - curUv` into `mFSR2MotionMap`. The pass has no depth attachment; its fragment shader discards pixels behind the scene depth. It is modelled on the shadow pass, which already re-submits the same avatar geometry with replacement skinning programs.

**Tech Stack:** C++ (newview/llrender), GLSL 4.00 class1 shaders compiled for Vulkan through the Hoofstorm backend, the Vulkan test harnesses `llvulkan_frames_test` / `llvulkan_fsr2_test` / `viewer_vulkan_activation_test`.

**Spec:** `docs/superpowers/specs/2026-10-19-fsr-temporal-upscaler-design.md` (motion vector section) and the findings in this session: camera-only motion (`fsr2MotionF.glsl`) makes moving avatars blur only with FSR 2 on, at any distance, with dynamic scaling off; a reactive mask and a per-avatar rigid-movement box (`collectFsr2AvatarBoxes`, uncommitted) did not fix it.

## Global Constraints

- Motion vectors are UV units, `prevUv - curUv`, scaled by render size in the wrapper; Y sign comes from `kFsr2MotionYSign` only (`llfsr2.h`).
- Both ends of a velocity use the **unjittered** view-projection (`mCurViewProjUnjittered`, `mPrevViewProj`); forward-pass geometry stays on the jittered matrix.
- Array uniforms in shaders use a literal size (`foo[64]`); the Vulkan backend does not read an expression.
- Programs for the FSR 2 path are built only while FSR 2 is on and Vulkan is active; a program that fails to compile must leave the camera-only motion working (no crash, no black avatars).
- `destroy()` / `resize()` on the wrapper run only on the render thread; the new pass runs on the main thread inside `renderFsr2Upscale` before the dispatch is queued.
- Never launch the viewer or log in. No commit, push or release without the user's approval. Build with `doc/vulkan/tools/build-viewer.cmd` in the background; pass is `BUILD_EXIT=0` and no `error C` / `error LNK`.
- Tests that must stay green: `llvulkan_fsr2_test` 59, `llvulkan_frames_test` (162 now), `viewer_vulkan_activation_test` 48, all with zero validation messages.
- Performance gate (pending user decision): the pass must not cost more than ~1 ms at 12 visible avatars on the RX 6700 XT; measure with the existing Tracy GPU zone.

## Review Focus

- An avatar behind a wall or another avatar: its velocity must not overwrite the wall's motion (depth discard).
- Avatar teleports, region crossing, first frame after login, camera cut: previous palette missing or unrelated → velocity must be exactly zero, never a streak across the screen.
- Distant-animation throttle (`mFarAnimSkipped`, poses update every 3 to 6 frames): the palette is reused on skipped frames → velocity exactly zero on those frames, one full step on the update frame.
- Rigged mesh whose `LLMeshSkinInfo` has more joints than the body palette, mesh with no skin info loaded yet, avatars with zero rigged attachments.
- Impostor avatars and avatars with `AOA_INVISIBLE` / jelly-dolled: skipped by the pass, camera motion stays.
- Alpha-masked attachments (hair, lashes): cut-out pixels must not write velocity.
- FSR 2 turned off or falling back mid-session: pass and prev-palette bookkeeping stop cleanly, no stale state when it is turned back on.

---

## File Structure

| File | Responsibility |
|---|---|
| `indra/newview/llvoavatar.h/.cpp` | `MatrixPaletteCache` keeps the previous frame's palette (`mPrevGLMp`) |
| `indra/newview/app_settings/shaders/class1/deferred/fsr2AvatarVelocityV.glsl` (+ `...F.glsl`) | body and rigged velocity programs (rigged via `mRiggedVariant`) |
| `indra/newview/llviewershadermgr.cpp/.h` | the new `gDeferredFSR2AvatarVelocityProgram` and its rigged variant |
| `indra/newview/lldrawpoolavatar.cpp/.h` | `renderFsr2Velocity()` entry that draws body and rigged avatars with the velocity program |
| `indra/newview/pipeline.cpp/.h` | `renderFsr2AvatarVelocity()` driver, called from `renderFsr2Upscale`; removal of `collectFsr2AvatarBoxes` |
| `indra/llrender/vulkan/tests/frames_test.cpp`, `shader_test.cpp` | numeric and compile tests for the velocity shaders |

---

### Task 1: Spike, can the avatar passes be re-driven with a replacement program?

Decides the open interface for Tasks 3 and 4. Produces a short note, no committed code.

**Files:**
- Read: `indra/newview/lldrawpoolavatar.cpp:287-420` (shadow pass), `indra/newview/pipeline.cpp:11640-11840` (shadow render-type mask), `indra/newview/lldrawpool.cpp:650-760` (`uploadMatrixPalette`), `LLVOAvatar::renderSkinned`.
- Write: `docs/superpowers/notes/2026-10-21-avatar-velocity-spike.md`

**Interfaces:**
- Produces: the note answers, with file:line evidence: (a) which pool call draws rigged mesh for a given avatar (per-avatar or per-spatial-group), and whether it can be limited to visible avatars; (b) how a rigged pass picks `mRiggedVariant` and whether a global override program can be forced; (c) where rigid attachments get their model matrix and whether the previous one is kept anywhere.

- [ ] **Step 1: Answer (a), (b), (c) from the code and write the note**
- [ ] **Step 2: Choose one of two drive strategies and state it in the note**
  - *Strategy S (shadow-style):* re-run the shadow render path with the velocity program substituted. Pick this if (b) shows a single override point.
  - *Strategy R (reduced):* velocity for the body mesh and rigged mesh only, rigid attachments keep camera motion plus the existing box approximation. Pick this if (c) shows no previous model matrix.
- [ ] **Step 3: Show the note to the user and wait for approval before Task 3**

---

### Task 2: Keep the previous frame's bone palette

**Files:**
- Modify: `indra/newview/llvoavatar.h:954-975` (`MatrixPaletteCache`), `indra/newview/llvoavatar.cpp:11498-11552` (`updateSkinInfoMatrixPalette`)
- Modify: `indra/llrender/llfsr2.h` (pure helper, testable without the viewer)
- Test: `indra/llrender/vulkan/tests/fsr2_test.cpp`

**Interfaces:**
- Produces: `std::vector<F32> MatrixPaletteCache::mPrevGLMp` (same layout as `mGLMp`, 12 floats per joint); `bool MatrixPaletteCache::mPrevValid`.
- Produces: `inline bool fsr2PrevPaletteUsable(bool had_palette, unsigned prev_count, unsigned cur_count, unsigned frames_since_build)` in `llfsr2.h`: true only when a palette existed, joint counts match and `frames_since_build <= kFsr2PrevPaletteMaxAge`.
- Produces: `constexpr unsigned kFsr2PrevPaletteMaxAge = 8;`

- [ ] **Step 1: Write failing tests in `fsr2_test.cpp`**: `fsr2PrevPaletteUsable(false,0,12,1)` false; `(true,12,12,1)` true; `(true,12,13,1)` false (mesh changed); `(true,12,12,kFsr2PrevPaletteMaxAge+1)` false (stale after a long gap).
- [ ] **Step 2: Run `llvulkan_fsr2_test`, expect FAIL (symbol undefined)**
- [ ] **Step 3: Add the helper and constant to `llfsr2.h`**
- [ ] **Step 4: Run it, expect PASS (63 checks)**
- [ ] **Step 5: In `updateSkinInfoMatrixPalette`, before rebuilding, move `mGLMp` into `mPrevGLMp` and set `mPrevValid = fsr2PrevPaletteUsable(...)`.** When the throttle path reuses the palette (`entry.mFrame = gFrameCount` shortcut) set `mPrevGLMp = mGLMp` so velocity is exactly zero.
- [ ] **Step 6: Build the viewer; expect `BUILD_EXIT=0`**
- [ ] **Step 7: Commit** (needs user approval)

---

### Task 3: Velocity shaders (body and rigged) with numeric tests

**Files:**
- Create: `indra/newview/app_settings/shaders/class1/deferred/fsr2AvatarVelocityV.glsl`, `fsr2AvatarVelocityF.glsl`
- Modify: `indra/newview/llviewershadermgr.cpp` (declare `gDeferredFSR2AvatarVelocityProgram`, its rigged variant `gDeferredFSR2AvatarVelocitySkinnedProgram`, unload and build next to `gDeferredFSR2ReactiveProgram`), `llviewershadermgr.h`
- Test: `indra/llrender/vulkan/tests/frames_test.cpp`, `shader_test.cpp`

**Interfaces:**
- Vertex shader inputs: the same attributes and skinning includes the shadow programs use (`avatarSkinV.glsl` for the body, `objectSkinV.glsl` for rigged). Uniforms: `view_proj_cur`, `view_proj_prev` (unjittered), `matrixPalette` (current), `matrixPalettePrev` (same size and type), `motion_y_sign`.
- Fragment shader: samples `depthMap` (scene depth), discards when `gl_FragCoord`-depth is more than `depth_bias` behind it, discards alpha-cut pixels where the shadow program does; outputs `vec4(prevUv - curUv, 0, 0)` with the Y sign applied exactly as `fsr2MotionF.glsl` does.
- Produces: C++ `gDeferredFSR2AvatarVelocityProgram` (`LLGLSLShader`), `.mRiggedVariant` set.

- [ ] **Step 1: Write failing tests**
  - `shader_test.cpp`: both programs compile and link; uniform names `view_proj_cur`, `view_proj_prev`, `matrixPalettePrev`, `depthMap` found.
  - `frames_test.cpp` `velocity: zero when the palettes are equal` (all pixels of a synthetic triangle read exactly `0,0`); `velocity: translated palette` (previous palette = current shifted 0.3 m along x gives `-0.5 * P[0] * 0.3 / distance` within 1e-4, the same expectation the existing moving-avatar test uses); `velocity: hidden triangle writes nothing` (scene depth in front of the triangle leaves the pre-filled 9s untouched).
- [ ] **Step 2: Run both tests; expect FAIL (files missing)**
- [ ] **Step 3: Write the shaders.** Skin the vertex twice (current and previous palette), project with the unjittered matrices, pass `curNdc` and `prevNdc` as varyings, compute the UV difference in the fragment shader so it interpolates perspective-correctly.
- [ ] **Step 4: Register and build the programs in `llviewershadermgr.cpp`; a compile failure must only leave the program incomplete (the pass skips)**
- [ ] **Step 5: Run `llvulkan_frames_test`, `shader_test`; expect PASS and zero validation messages**
- [ ] **Step 6: Commit** (needs user approval)

---

### Task 4: Avatar velocity pass driver

Depends on the Task 1 decision (strategy S or R).

**Files:**
- Modify: `indra/newview/lldrawpoolavatar.cpp/.h` (new `static void LLDrawPoolAvatar::renderFsr2Velocity(const glm::mat4& view_proj_cur, const glm::mat4& view_proj_prev)`), `indra/newview/pipeline.cpp/.h` (new `void LLPipeline::renderFsr2AvatarVelocity(LLRenderTarget* depthSource, LLRenderTarget* motion)`)
- Test: `indra/llrender/vulkan/tests/frames_test.cpp` (chain test, motion target not overwritten when the program is incomplete)

**Interfaces:**
- Consumes: `MatrixPaletteCache::mPrevGLMp`, `mPrevValid` (Task 2); `gDeferredFSR2AvatarVelocityProgram` (Task 3).
- Produces: `renderFsr2AvatarVelocity` binds `motion` without clearing, sets blend off, depth test off, binds the velocity program with the scene depth, uploads `matrixPalettePrev` next to every `uploadMatrixPalette` call it makes, restores state, and flushes. Avatars with `mPrevValid == false` are skipped (they keep camera motion). Impostors, invisible and dead avatars are skipped with the same conditions `renderShadow` uses (`lldrawpoolavatar.cpp:374-395`).

- [ ] **Step 1: Write the failing chain test:** with the velocity program deleted, the chain still dispatches and the motion target equals the camera-only result byte for byte
- [ ] **Step 2: Run it; expect FAIL or build error (function missing)**
- [ ] **Step 3: Implement `renderFsr2AvatarVelocity` and call it in `renderFsr2Upscale` right after `generateFsr2Motion`, only when `gDeferredFSR2AvatarVelocityProgram.isComplete()`**
- [ ] **Step 4: Build; run all three test binaries; expect 59 / ≥162 / 48 and zero validation messages**
- [ ] **Step 5: Add a Tracy GPU zone `"FSR 2 avatar velocity"` and log the avatar count once per ten seconds at debug level**
- [ ] **Step 6: Commit** (needs user approval)

---

### Task 5: In-world check and removal of the approximation

**Files:**
- Modify: `indra/newview/pipeline.cpp` (delete `collectFsr2AvatarBoxes`, `mFsr2AvatarPrevPos`, the box uniforms), `indra/newview/pipeline.h`, `indra/newview/app_settings/shaders/class1/deferred/fsr2MotionF.glsl` (delete `avatar_box` loop), `indra/llrender/vulkan/tests/frames_test.cpp` (delete the moving-avatar box test), `doc/vulkan/PROJECT-NOTES.md` (status, checklist)

- [ ] **Step 1: User test (needs user):** deploy `Hoofstorm.exe`, walk and run among avatars at several distances, wave and dance in place, wear rigged hair and a rigid hat, stand still. Report: moving avatars sharp? limbs sharp? any smear on the ground, any streak across the screen, any flicker when avatars appear or teleport?
- [ ] **Step 2: If it holds, delete the box approximation and its test, rebuild, rerun all three test binaries**
- [ ] **Step 3: If limbs or attachments still blur, record which and open a follow-up task; do not delete the approximation**
- [ ] **Step 4: Update Status and Next steps in `PROJECT-NOTES.md`**
- [ ] **Step 5: Commit** (needs user approval)

---

## Self-Review

- **Spec coverage:** motion vectors for moving avatars (Tasks 2 to 4); distance throttle behaviour (Task 2 step 5, Review Focus); teleport and missing history (Task 2 helper, Task 4 skip rule); removal of unproven approximations (Task 5).
- **Open risk, stated:** Task 1 may find that rigged passes cannot be redrawn with an override program without invasive changes in several pools. In that case strategy R is the fallback, and the plan stops at Task 4 with body and rigged mesh only; rigid attachments stay on camera motion.
- **Not in this plan:** reactive mask for alpha-blended surfaces and particles, motion vectors for non-avatar moving objects, per-bone vectors for animesh / control avatars (`RENDER_TYPE_CONTROL_AV`; add after Task 5 if they blur).
