# Shader probe: how much of the viewer's real GLSL works as Vulkan SPIR-V?

Developer tools for the Vulkan shader layer work. Nothing here is part of the viewer build.

## 1. Dump the viewer's real shader sources

The viewer has a developer hook (off unless the variables are set) in `llrender/llshadermgr.cpp`:

| Variable | Effect |
|---|---|
| `FS_SHADER_DUMP_DIR=<existing dir>` | Write the final source of every shader object the viewer compiles (after includes, `#define`s and generated code) and a `manifest.txt` that lists, per program, which objects were attached. Also bypasses the shader program binary cache so that everything is compiled from source. |
| `FS_SHADER_DUMP_EXIT=1` | Quit as soon as every shader is loaded (end of `LLViewerShaderMgr::setShaders`). The quit flag is set during startup, so the main loop never runs `idle_startup()`: **the viewer never shows the login screen and cannot log in.** |

```
set FS_SHADER_DUMP_DIR=C:\phoenix-firestorm\shaderdump
set FS_SHADER_DUMP_EXIT=1
build-vc170-64\newview\Release\firestorm-bin.exe
```

- **Always set `FS_SHADER_DUMP_EXIT` too.** Without it the viewer continues to the login screen, where saved credentials may be used.
- Use a **short** directory. Each file name is about 60 characters; a long directory pushes paths over Windows' 260-character limit and the viewer cannot write the files (silently, apart from one warning in the log).
- The viewer exits after about 8 seconds with exit code `-1073741819` (access violation): quitting this early is not a clean shutdown. It is harmless for this purpose.

## 2. Probe

```
set VULKAN_SDK=C:\VulkanSDK\1.4.357.0

rem each shader object on its own (control: plain GLSL validation, then Vulkan before/after mechanical translation)
python shader_vk_probe.py  C:\phoenix-firestorm\shaderdump  C:\phoenix-firestorm\shaderdump_out

rem programs as GL links them: merge each stage's objects, translate, compile, then link the stages of each program together
set PREPROCESS=1
python shader_vk_link_probe.py  C:\phoenix-firestorm\shaderdump  C:\phoenix-firestorm\shaderdump_link

rem program composition statistics from the manifest
python manifest_stats.py  C:\phoenix-firestorm\shaderdump
```

The mechanical translation (`translate()` in `shader_vk_probe.py`): `#version 450`; loose (non-opaque) uniforms become uniform blocks;
legacy no-op `#extension` lines are removed; bindings and locations come from `-fauto-bind-uniforms -fauto-map-locations` (per stage) and
`--auto-map-bindings --auto-map-locations` (when stages are linked). Results and what they mean: `doc/vulkan/shader-layer-design.md`
(project notes).
