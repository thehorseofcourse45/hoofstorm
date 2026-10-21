# Avatar velocity spike (Task 1)

Findings, with evidence.

(a) Rigged mesh is not drawn per avatar. It lives in the normal pools as `*_RIGGED` pass types and is drawn per pass type for the whole view:
`LLPipeline::renderObjects(type, texture, batch_texture, rigged=true)` -> `mSimplePool->pushRiggedBatches(type + 1, ...)` (pipeline.cpp:8162);
PBR: `renderGLTFObjects(type, texture, rigged)` -> `pushRiggedGLTFBatches` (pipeline.cpp:8181). Calling these with rigged=true draws only rigged
geometry (avatar attachments and animesh), not static objects. The main view's render maps are still intact in renderFinalize.

(b) The shadow pass is the template: it binds a program with `program.bind(rigged)` (llglslshader.cpp:1103 binds `mRiggedVariant`) and calls the
helpers above (pipeline.cpp:11250-11370). Every pool switches to its rigged variant itself, so there is no single global override; instead the
velocity pass binds `gDeferredFSR2AvatarVelocityProgram.bind(true)` and calls the helpers for the pass types in the shadow list.
Palette upload for rigged goes through three `LLRenderPass::uploadMatrixPalette` overloads (lldrawpool.cpp:651-756); one helper there can also
upload the previous palette while a static flag `LLRenderPass::sFsr2VelocityPass` is set.

Body mesh: `LLDrawPoolAvatar::renderShadow` -> `avatarp->renderSkinned()` with `gDeferredAvatarShadowProgram` (lldrawpoolavatar.cpp:287-417). Its
palette is built per joint mesh in `LLViewerJointMesh::uploadJointMatrices` (llviewerjointmesh.cpp:168-185, 45 vec4); the previous one has to be kept
per joint mesh (not in `MatrixPaletteCache`).

(c) Rigid (non-rigged) attachments take their model matrix from the draw info / bridge render matrix; nothing keeps the previous one.

Decision: strategy S for body and rigged mesh (opaque, alpha-masked, PBR); strategy R for rigid attachments: they keep camera motion plus the
existing per-avatar box translation, so `collectFsr2AvatarBoxes` stays after Task 5 and the velocity pass overdraws the box result.
