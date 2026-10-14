# Avatar animation on worker threads: audit

Status: audit only, no code changed. Written 2026-10-14 from `llvoavatar.cpp`, `llmotioncontroller.cpp`, `llkeyframemotion.cpp` and the motion subclasses.

## What it could buy

Latest threaded profile (main thread, 300 samples, about 11 ms per frame): `LLVOAvatar::idleUpdate` 20%, of which `updateCharacter` 10-15%. The part that is per-avatar math is `updateMotions(NORMAL_UPDATE)` (5-8%) plus `mRoot->updateWorldMatrixChildren()` (3%), about **8-11% of the main thread, roughly 1 ms**. With 8 workers the best case saves about 0.8 ms per frame (about 8% more FPS). The rest of `idleUpdate` (name tags, root position, sitting, footsteps, attachments, lip sync) touches world state and stays on the main thread.

## What `updateMotions` does per avatar

`LLMotionController::updateMotions` -> `updateLoadingMotions`, `updateMotionsByType` (additive, then regular), `LLPoseBlender::blendAndApply`. Everything writes only this avatar's own objects: its motions, its pose, its skeleton joints (`LLJoint::touch` marks dirty up its own tree). Keyframe animation reads a `JointMotionList` that is shared by every avatar playing the same animation, but only reads it in `onUpdate`/`applyKeyframes`; per-avatar state (`mJointStates`, `mLastLoopedTime`) is not shared.

## Hazards found

| # | Hazard | Where | Needed |
| --- | --- | --- | --- |
| 1 | Global RNG `ll_frand` is not thread safe | `LLEyeMotion::onUpdate` (jitter, blinks, look-away) | per-motion RNG |
| 2 | `updateLoadingMotions` starts asset fetches (`gAssetStorage`), edits the global motion registry (`sRegistry.markBad`), deletes motions | `LLMotionController::updateLoadingMotions`, `LLKeyframeMotion::onInitialize` | keep on the main thread, run before the parallel phase |
| 3 | Motion stop/deactivate paths call `requestStopMotion` | `LLVOAvatar::requestStopMotion` is empty for other avatars; self sends network messages | exclude self |
| 4 | `LLRefCount` is not atomic | motions and shared data touched from two threads | check which refcounted objects are shared between avatars (the `JointMotionList` cache is a raw pointer, looks fine) |
| 5 | Physics and emote motions read viewer statics and settings (`LLVOAvatar::sPhysicsLODFactor`, `AvatarPhysics`) | `llphysicsmotion.cpp`, `llemote.cpp` | read-only while the main thread waits, safe in fork-join |
| 6 | Animation data pointers (`LookAtPoint`, `PointAtPoint`) written by HUD effects on the main thread | `getAnimationData` | safe only if the main thread is parked during the parallel phase |
| 7 | Control avatars (animesh) follow a linked object | `LLControlAvatar::updateCharacter` | exclude |
| 8 | Static counters and profiling zones | `LLJoint` stats, `LL_PROFILE_*` | benign |

`LLKeyframeMotion::onUpdate`, the blender and the matrix update showed no shared writes.

## Proposed design (not started)

Fork-join inside `LLViewerObjectList::update`, not a free-running thread:
1. Serial pass (main): for each avatar run today's `updateCharacter` up to but excluding `updateMotions`, and `updateLoadingMotions` equivalent; collect avatars eligible for the parallel step (not self, not control avatar, not impostor-skipped, `mIsBuilt`).
2. Parallel pass: pool tasks call `mMotionController.updateMotions(false)` and `mRoot->updateWorldMatrixChildren()` per eligible avatar; the main thread waits.
3. Serial pass (main): the remainder of `updateCharacter` (head offset, footsteps, `mNeedsSkin`), then the other `idleUpdate` work.

The split of `updateCharacter` changes the order in which an avatar's steps run relative to other avatars. Check that nothing in steps 1 or 3 reads another avatar's pose.

## Verdict

About 1 ms for a moderate change and a real risk of subtle bugs (a twitching or frozen avatar shows only under load). Worth doing only after the cheaper items. Hazard 1 and 2 are straightforward; hazard 4 needs a refcount audit of `LLMotion` sharing before any code.
