# FSR 2 in-world checklist (user runs it, Claude reads the logs afterwards)

Status on 2026-10-21: session A DONE (all good), session B DONE (see PROJECT-NOTES; chat line not seen), session C baseline only (ran with FSR 2 off; FSR 2 VRAM return-to-0 not observed, accepted). Performance accepted by judgement. The numbers match the older list in `PROJECT-NOTES.md`.

Before you start: the viewer is closed, `Hoofstorm.exe` and the Preferences XML are the latest build (Claude copies them). Do not log in until told.
Settings: Vulkan renderer, Graphics > Upscaling > "Temporal upscaler (FSR 2/3)" ticked. Start with Quality and dynamic scaling OFF.

## Session A: one login, about 30 minutes

Stay in one busy region (avatars, foliage, water, a window or glass if you can find one).

1. **Preferences layout (new).** Open Graphics > Upscaling. The FSR 1.0 and FSR 2 columns do not overlap, both dynamic-scaling labels read "Dynamic scaling (target FPS)" and are not cut off, the render size text is shown. Pass: nothing overlaps or clips.
2. **Static sharpness (1).** Stand still, same spot, compare FSR 1.0 (Quality) against FSR 2 (Quality): text, textures, hair. Pass: FSR 2 is at least as sharp.
3. **Camera sign (7).** Walk, run and turn the camera quickly. Pass: no shimmer, no doubled image, no jitter that fights the motion. If it fails, say which direction (turning or moving) and Claude flips `kFsr2RowsFollowNdcY`.
4. **Avatars and ghosting (2).** DONE for avatars (body, rigged mesh, hair). Still check: trails behind alpha surfaces (leaves, glass, particles) and behind a flying avatar or vehicle. Also look at hats and jewellery (rigid attachments) on a moving avatar, and animesh if you can find any.
5. **Shimmer in effects (6).** Move the camera over water, a wet or shiny floor, under shadows and in fog. Pass: no crawling or boiling.
6. **Quality modes (5).** Cycle Native AA, Quality, Balanced, Performance, Ultra Performance. Pass: each takes effect without a restart, Native AA looks anti-aliased at full size, and the render size text matches the FPS tooltip.
7. **Dynamic scaling.** Tick it with a target of about 60 FPS, walk through busy and empty places. Pass: no hitches when the scale changes (a half-second stall when avatars load was the earlier bug), no ghost trails, scale rises again when there is room.
8. **LOD bias bracket (8).** With FSR 2 on and then off, compare shadows, a mirror or reflection if you have one, the inventory thumbnails and a snapshot preview. Pass: they look the same.
9. **Live toggles (9).** Switch FSR 1.0 and FSR 2 on and off several times. Pass: each unticks the other, no chat line, no flicker, no crash.
10. **Window resize (4).** Resize the window, maximize, restore, switch to fullscreen if you use it. Pass: no crash, no stale-size image, the render size text updates.
11. **Teleport and camera cut (3).** Teleport, take a snapshot, then make a large camera jump (focus on something far away). Pass: no smear of the old scene for more than one frame.
12. **Performance (12).** Same scene, same camera spot, native (FSR 2 off) versus FSR 2 Quality, about a minute each. Write down the FPS shown in the stats bar. Pass: Quality is at least 15% faster than native (your number to confirm). Claude reads the GPU ms from the `VulkanPacing` log lines.

Then log out and close the viewer. Tell Claude what failed; Claude reads the log for item 13.

## Session B: forced failure, viewer closed (10)

1. Rename `build-vc170-64\newview\Release\app_settings\shaders\class1\deferred\fsr2MotionF.glsl` to something else.
2. Start the viewer, tick FSR 2.
3. Expect: FSR 2 unticked, FSR 1.0 ticked at the matching preset (Native AA: FSR 1.0 stays off), one nearby-chat line, no repeated shader reloads.
4. Close the viewer and rename the file back.

Also a second case worth trying: rename `fsr2AvatarVelocityV.glsl` instead. Expect FSR 2 to stay on and keep working with camera-only motion on avatars (the velocity programs are optional), and no fallback.

## Session C: VRAM stability, 40 minutes (11)

Log in, FSR 2 on, walk or stand in a busy region for 40 minutes, then switch FSR 2 off and wait a minute. Pass (Claude checks the `VRAM by owner MB` log lines): `upscaler=` stays flat while FSR 2 is on and returns to 0 after switching off; total used and budget stay flat. Session A can run inside this time if you stay logged in.

## After each session: Claude reads the logs (13)

- `Firestorm.log`: `Failed to link shader`, `FSR 2`, `VK_ERROR`, validation lines, repeated `FSR 2 upscale not recorded`, `FrameSpike` lines with `Display:Sky` above 60 ms.
- `vulkan_fatal.log`: `TEXSLOW` and anything fatal.

## What to report

Per item: pass, fail, or not tried, plus one line for anything odd. Screenshots help for blur, shimmer and layout problems.
