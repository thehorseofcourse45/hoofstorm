# OpenGL call catalog

Generated from `indra/` by `glcatalog.py` (comments stripped, so commented-out calls are not counted).

- Distinct GL functions: **172**
- Total call sites: **370** in **35** files

Counts are call sites in source, not runtime calls. Preprocessor-disabled blocks (`#if 0`, other platforms) are still counted, so treat as an upper bound.

## 1. By category

| Category | Call sites | Distinct fns | Vulkan equivalent |
|---|---:|---:|---|
| context / debug / introspection | 86 | 9 | Instance/device setup, validation layers |
| vertex buffers / VAO / draw | 62 | 25 | VkBuffer, vertex input state, vkCmdDraw* |
| queries / sync / fences | 53 | 12 | Query pools, fences, semaphores, barriers |
| textures / samplers / images | 37 | 21 | VkImage, VkSampler, staging uploads |
| shader / program objects | 35 | 24 | Pipeline + shader module creation (SPIR-V) |
| framebuffers / render targets | 33 | 27 | Render passes / dynamic rendering, attachments |
| fixed pipeline state (blend/depth/cull/stencil/raster) | 25 | 19 | Baked into pipeline objects (or dynamic state) |
| uniforms / UBOs | 21 | 21 | Descriptor sets, UBOs, push constants |
| legacy fixed-function (not in core GL) | 12 | 10 | None needed if dead; port or drop |
| UNCATEGORIZED | 6 | 4 | ? |

## 2. By module and category

| Module | context / debug / introspection | vertex buffers / VAO / draw | queries / sync / fences | textures / samplers / images | shader / program objects | framebuffers / render targets | fixed pipeline state | uniforms / UBOs | legacy fixed-function | UNCATEGORIZED | Total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| llrender | 73 | 61 | 31 | 37 | 35 | 29 | 23 | 21 | 11 | 5 | 326 |
| llwindow | 8 | 0 | 1 | 0 | 0 | 4 | 2 | 0 | 0 | 0 | 15 |
| newview | 3 | 1 | 21 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 27 |
| llappearance | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 |
| llcommon | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 |

## 3. Functions per category

### context / debug / introspection

`glGetIntegerv` ×33, `glGetError` ×27, `glGetString` ×15, `glGetFloatv` ×4, `glGetStringi` ×2, `glSwapBuffers` ×2, `glDebugMessageCallback` ×1, `glGetBooleanv` ×1, `glObjectLabel` ×1

### vertex buffers / VAO / draw

`glBindBuffer` ×13, `glVertexAttribPointer` ×13, `glGetBufferParameteriv` ×4, `glGenBuffers` ×4, `glBufferData` ×4, `glDeleteBuffers` ×3, `glVertexAttribIPointer` ×2, `glDrawArrays` ×2, `glBindBufferARB` ×1, `glDeleteBuffersARB` ×1, `glGenBuffersARB` ×1, `glIsBufferARB` ×1, `glBufferDataARB` ×1, `glBufferSubDataARB` ×1, `glGetBufferSubDataARB` ×1, `glMapBufferARB` ×1, `glUnmapBufferARB` ×1, `glGetBufferParameterivARB` ×1, `glDisableVertexAttribArray` ×1, `glEnableVertexAttribArray` ×1, `glBufferSubData` ×1, `glDrawRangeElements` ×1, `glGenVertexArrays` ×1, `glBindVertexArray` ×1, `glDrawElements` ×1

### queries / sync / fences

`glBeginQuery` ×7, `glEndQuery` ×7, `glGenQueries` ×6, `glGetQueryObjectui64v` ×6, `glGetQueryObjectuiv` ×6, `glDeleteSync` ×4, `glDeleteQueries` ×4, `glFlush` ×4, `glFenceSync` ×3, `glClientWaitSync` ×3, `glFinish` ×2, `glWaitSync` ×1

### textures / samplers / images

`glTexParameteri` ×12, `glPixelStorei` ×4, `glTexImage2D` ×2, `glGetTexImage` ×2, `glGenerateMipmapEXT` ×1, `glActiveTexture` ×1, `glBindTexture` ×1, `glTexParameterf` ×1, `glTexParameteriv` ×1, `glGenTextures` ×1, `glDeleteTextures` ×1, `glCompressedTexImage2D` ×1, `glTexSubImage2D` ×1, `glTexImage3D` ×1, `glTexSubImage3D` ×1, `glGenerateMipmap` ×1, `glCopyTexSubImage2D` ×1, `glCopyTexSubImage3D` ×1, `glCopyTexImage2D` ×1, `glGetTexLevelParameteriv` ×1, `glGetCompressedTexImage` ×1

### shader / program objects

`glGetProgramiv` ×6, `glGetShaderiv` ×3, `glDeleteShader` ×3, `glGetProgramInfoLog` ×2, `glIsShader` ×2, `glGetShaderInfoLog` ×1, `glCreateShader` ×1, `glShaderSource` ×1, `glCompileShader` ×1, `glCreateProgram` ×1, `glAttachShader` ×1, `glGetAttachedShaders` ×1, `glDetachShader` ×1, `glDeleteProgram` ×1, `glBindAttribLocation` ×1, `glLinkProgram` ×1, `glValidateProgram` ×1, `glIsProgram` ×1, `glUseProgram` ×1, `glGetAttribLocation` ×1, `glGetActiveUniform` ×1, `glProgramParameteri` ×1, `glProgramBinary` ×1, `glGetProgramBinary` ×1

### framebuffers / render targets

`glClearColor` ×3, `glClear` ×3, `glBindFramebuffer` ×2, `glDrawBuffer` ×2, `glGenFramebuffers` ×1, `glDeleteFramebuffers` ×1, `glFramebufferTexture2D` ×1, `glCheckFramebufferStatus` ×1, `glDrawBuffers` ×1, `glReadBuffer` ×1, `glReadPixels` ×1, `glIsRenderbufferEXT` ×1, `glBindRenderbufferEXT` ×1, `glDeleteRenderbuffersEXT` ×1, `glGenRenderbuffersEXT` ×1, `glRenderbufferStorageEXT` ×1, `glGetRenderbufferParameterivEXT` ×1, `glIsFramebufferEXT` ×1, `glBindFramebufferEXT` ×1, `glDeleteFramebuffersEXT` ×1, `glGenFramebuffersEXT` ×1, `glCheckFramebufferStatusEXT` ×1, `glFramebufferTexture1DEXT` ×1, `glFramebufferTexture2DEXT` ×1, `glFramebufferTexture3DEXT` ×1, `glFramebufferRenderbufferEXT` ×1, `glGetFramebufferAttachmentParameterivEXT` ×1

### fixed pipeline state (blend/depth/cull/stencil/raster)

`glEnable` ×4, `glDisable` ×3, `glIsEnabled` ×2, `glBlendFuncSeparateEXT` ×1, `glDepthFunc` ×1, `glDepthMask` ×1, `glBlendFunc` ×1, `glBlendFuncSeparate` ×1, `glColorMask` ×1, `glLineWidth` ×1, `glViewport` ×1, `glScissor` ×1, `glCullFace` ×1, `glPolygonMode` ×1, `glPolygonOffset` ×1, `glPointSize` ×1, `glStencilFunc` ×1, `glStencilOp` ×1, `glStencilMask` ×1

### uniforms / UBOs

`glBindBufferBase` ×1, `glGetUniformLocation` ×1, `glGetUniformBlockIndex` ×1, `glUniformBlockBinding` ×1, `glUniform1i` ×1, `glUniform2i` ×1, `glUniform1f` ×1, `glUniform2f` ×1, `glUniform3f` ×1, `glUniform4f` ×1, `glUniform1iv` ×1, `glUniform4iv` ×1, `glUniform1fv` ×1, `glUniform2fv` ×1, `glUniform3fv` ×1, `glUniform4fv` ×1, `glUniform4uiv` ×1, `glUniformMatrix2fv` ×1, `glUniformMatrix3fv` ×1, `glUniformMatrix3x4fv` ×1, `glUniformMatrix4fv` ×1

### legacy fixed-function (not in core GL)

`glMaterialfv` ×2, `glMateriali` ×2, `glHint` ×1, `glClientActiveTexture` ×1, `glPushAttrib` ×1, `glPushClientAttrib` ×1, `glPopClientAttrib` ×1, `glPopAttrib` ×1, `glAreTexturesResident` ×1, `glVertexPointer` ×1

### UNCATEGORIZED

`glTypeOf` ×3, `glClearDepth` ×1, `glGetBufferPointervARB` ×1, `glTF` ×1

## 4. Files that call GL directly, outside `llrender/` (the migration list)

These bypass the abstraction layer and must be routed through it before a second backend is possible.

| File | GL calls | Top categories |
|---|---:|---|
| `llwindow/llwindowsdl.cpp` | 7 | context / debug / introspection 6, fixed pipeline state 1 |
| `newview/llvoavatar.cpp` | 5 | queries / sync / fences 5 |
| `newview/llvieweroctree.cpp` | 5 | queries / sync / fences 5 |
| `newview/llscenemonitor.cpp` | 4 | queries / sync / fences 4 |
| `newview/llreflectionmap.cpp` | 4 | queries / sync / fences 4 |
| `llwindow/llwindowsdl2.cpp` | 3 | framebuffers / render targets 2, fixed pipeline state 1 |
| `newview/llspatialpartition.cpp` | 2 | legacy fixed-function 1, vertex buffers / VAO / draw 1 |
| `newview/llreflectionmapmanager.cpp` | 2 | queries / sync / fences 2 |
| `newview/fsmaniprotatejoint.cpp` | 2 | context / debug / introspection 2 |
| `llwindow/llwindowwin32.cpp` | 2 | framebuffers / render targets 2 |
| `newview/pipeline.cpp` | 1 | queries / sync / fences 1 |
| `newview/llsnapshotlivepreview.cpp` | 1 | context / debug / introspection 1 |
| `newview/llfilepicker.cpp` | 1 | UNCATEGORIZED 1 |
| `llwindow/llwindowmesaheadless.cpp` | 1 | queries / sync / fences 1 |
| `llwindow/llwindowmacosx-objc.mm` | 1 | context / debug / introspection 1 |
| `llwindow/llwindowmacosx-objc.h` | 1 | context / debug / introspection 1 |
| `llcommon/llprofiler.h` | 1 | context / debug / introspection 1 |
| `llappearance/lltexlayer.cpp` | 1 | context / debug / introspection 1 |

## 5. Files inside `llrender/` (the layer to put the backend interface under)

| File | GL calls | Top categories |
|---|---:|---|
| `llrender/llshaderbackend_gl.cpp` | 61 | shader / program objects 33, uniforms / UBOs 20, context / debug / introspection 8 |
| `llrender/llgl.cpp` | 60 | context / debug / introspection 49, queries / sync / fences 5, vertex buffers / VAO / draw 4 |
| `llrender/lltexturebackend_gl.cpp` | 44 | textures / samplers / images 36, vertex buffers / VAO / draw 7, legacy fixed-function 1 |
| `llrender/llvertexbuffer_gl.cpp` | 35 | vertex buffers / VAO / draw 35 |
| `llrender/llglheaders.h` | 29 | framebuffers / render targets 16, vertex buffers / VAO / draw 10, fixed pipeline state 1 |
| `llrender/llrenderstatebackend_gl.cpp` | 21 | fixed pipeline state 19, context / debug / introspection 2 |
| `llrender/llglslshader.cpp` | 18 | queries / sync / fences 15, shader / program objects 2, context / debug / introspection 1 |
| `llrender/llframebufferbackend_gl.cpp` | 14 | framebuffers / render targets 13, UNCATEGORIZED 1 |
| `llrender/llimagegl.cpp` | 13 | queries / sync / fences 10, context / debug / introspection 3 |
| `llrender/llrenderbackend_vulkan.cpp` | 7 | vertex buffers / VAO / draw 5, queries / sync / fences 1, uniforms / UBOs 1 |
| `llrender/llrender.cpp` | 7 | context / debug / introspection 5, fixed pipeline state 2 |
| `llrender/llpostprocess.cpp` | 6 | legacy fixed-function 4, context / debug / introspection 2 |
| `llrender/llglstates.h` | 4 | legacy fixed-function 4 |
| `llrender/vulkan/vkshaderbackend.cpp` | 3 | UNCATEGORIZED 3 |
| `llrender/llrendertarget.cpp` | 2 | context / debug / introspection 2 |
| `llrender/llshadermgr.cpp` | 1 | context / debug / introspection 1 |
| `llrender/llcubemap.cpp` | 1 | fixed pipeline state 1 |

## 6. newview files that use the wrapper layer but make no direct GL calls

**153** newview files already render only through `gGL` / `LLVertexBuffer` / `LLGLSLShader` / `LLRenderTarget` etc. These need no migration, only the backend underneath them to change. Top 15 by wrapper use:

- `newview/llmaniptranslate.cpp` (236 wrapper references)
- `newview/llviewershadermgr.cpp` (210 wrapper references)
- `newview/llviewerwindow.cpp` (174 wrapper references)
- `newview/llviewerdisplay.cpp` (170 wrapper references)
- `newview/llglsandbox.cpp` (162 wrapper references)
- `newview/llmodelpreview.cpp` (152 wrapper references)
- `newview/fsmaniptranslatejoint.cpp` (134 wrapper references)
- `newview/llviewershadermgr.h` (130 wrapper references)
- `newview/lldrawpoolterrain.cpp` (115 wrapper references)
- `newview/llnetmap.cpp` (98 wrapper references)
- `newview/llworldmapview.cpp` (96 wrapper references)
- `newview/llmanipscale.cpp` (91 wrapper references)
- `newview/llfloaterimagepreview.cpp` (88 wrapper references)
- `newview/lldrawpoolbump.cpp` (86 wrapper references)
- `newview/llfasttimerview.cpp` (84 wrapper references)

## 6b. Dead code to delete instead of port

`newview/lldrawpoolterrain.cpp` `renderFull4TU()`, `renderFull2TU()` and `renderSimple()` (from line 659) are declared in `lldrawpoolterrain.h` and have no callers anywhere in `indra/`. They hold **0** GL call sites, mostly fixed-function (`glTexGen*`, `glMatrixMode`, ...). Delete them first; this removes most of the legacy-fixed-function category from the migration list.


## 7. Uncategorized (extend CATS)

`glTypeOf` ×3, `glClearDepth` ×1, `glGetBufferPointervARB` ×1, `glTF` ×1
