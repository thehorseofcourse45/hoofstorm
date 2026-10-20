/**
 * @file class1/deferred/fsr2ReactiveF.glsl
 *
 * FSR 2 temporal upscaler: reactive mask from the deferred G-buffer.
 */

/*[EXTRA_CODE_HERE]*/

// FSR 2 (FidelityFX FSR 3.1) reactive mask, one value per render-size pixel, into an R8 target. A value tells the upscaler how
// little to trust its history there: 0 = normal accumulation, up to 0.9 (never 1, which would drop the history entirely).
//
// What the G-buffer (mRT->deferredScreen, see addDeferredAttachments and gbufferUtil.glsl) holds, and why the mask is 0 in v1:
//   frag_data[0] diffuse.a   legacy emissive / shadow-kill (materialF), cloud alpha, 1.0 for the WL sky; 0 for everything else
//   frag_data[1] spec.a      legacy glossiness / vertex alpha as specular, PBR terrain base color alpha, 0 for most others
//   frag_data[2] normal.w    the lighting model, one of four codes (llshadermgr.cpp): GBUFFER_FLAG_SKIP_ATMOS 0.0 (WL sky,
//                            clouds, sun, moon, stars, highlight), HAS_ATMOS 0.34 (all legacy geometry), HAS_PBR 0.67,
//                            HAS_HDRI 1.0 (HDRI sky). With RenderHDREnabled off the normal target is RGB10_A2, whose 2-bit alpha
//                            holds exactly these four codes and no spare value.
//   no stencil (the deferred depth is depth-only)
// - Avatars are not identifiable: avatarF.glsl writes diffuse.a 0, spec 0, env 0, HAS_ATMOS, the same as alpha-masked prims
//   (diffuseAlphaMask*F), trees and terrain; rigged mesh avatars and attachments use the same material/PBR shaders as static
//   objects.
// - Alpha-blended surfaces, particles, water and glow are not in the G-buffer at all: they are drawn in the forward passes after
//   deferred lighting (materialF DIFFUSE_ALPHA_MODE_BLEND writes frag_color, not frag_data), and glow goes to the screen's alpha.
// - Alpha-masked (cutout) surfaces are in the G-buffer but carry no flag of their own (see avatars).
// So the pipeline passes reactive_flag < 0 and the mask is exactly 0 everywhere. The branch below is ready for a G-buffer code
// (normal.w compared with the same 0.1 tolerance as GET_GBUFFER_FLAG); one of the existing codes can be switched on through
// reactive_flag (for example SKIP_ATMOS for animated clouds) if the in-world check shows ghosting there. Anything else needs
// a forward-pass write, which the plan adds only after that check.

out vec4 frag_color;

uniform sampler2D normalMap;     // G-buffer frag_data[2]; .w = GBUFFER_FLAG_* code
uniform float reactive_flag;     // normal.w code to mark as reactive; < 0 marks nothing (v1, kFsr2ReactiveFlagNone)
uniform float reactive_value;    // mask value for marked pixels; clamped to 0.9

in vec2 vary_fragcoord;

void main()
{
    float mask = 0.0;
    if (reactive_flag >= 0.0)
    {
        float flag = texture(normalMap, vary_fragcoord).w;
        if (abs(flag - reactive_flag) < 0.1)
        {
            mask = clamp(reactive_value, 0.0, 0.9);
        }
    }
    frag_color = vec4(mask, 0.0, 0.0, 0.0);
}
