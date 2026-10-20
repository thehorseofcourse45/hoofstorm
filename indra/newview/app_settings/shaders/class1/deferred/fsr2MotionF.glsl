/**
 * @file class1/deferred/fsr2MotionF.glsl
 *
 * FSR 2 temporal upscaler: camera motion vectors from the scene depth.
 */

/*[EXTRA_CODE_HERE]*/

// FSR 2 (FidelityFX FSR 3.1) camera motion vectors, one per render-size pixel, into an RG16F target.
//
// Depth: the scene depth is GL style and not inverted: depth = 0.5 * ndc.z + 0.5, 0 at the near plane, 1 at the far plane
// (the Vulkan backend keeps GL's -1..1 clip depth through VK_EXT_depth_clip_control). The position is rebuilt the way
// deferredUtil.glsl getPosition() does it, ndc = (uv * 2 - 1, depth * 2 - 1), but with the unjittered inverse
// view-projection, so it lands in agent space instead of eye space. The far plane (sky, depth 1.0) is a finite point and
// reprojects like any other; nothing is divided by the rebuilt w, so a point at infinity cannot give NaN either.
//
// Units and sign: UV units (0..1 across the target), from the current pixel to where the same point was last frame:
//     motion = prevUv - curUv = (prevNdc - curNdc) * 0.5
// FSR 3.1 reads previous = uv + motion, with motionVectorScale = render size meaning UV units. Offscreen targets are not
// Y-flipped in the Vulkan backend (row 0 is ndc y = -1 and uv 0), so this UV is the memory UV the SDK samples and y needs
// no flip. motion_y_sign (kFsr2MotionYSign in llfsr2.h) scales y in case the in-world check shows otherwise.
// Camera motion only: moving objects and avatars get the camera's motion.

out vec4 frag_color;

uniform sampler2D depthMap;
uniform mat4 inv_view_proj_cur;  // inverse(unjittered projection * modelview), this frame
uniform mat4 view_proj_cur;      // unjittered projection * modelview, this frame
uniform mat4 view_proj_prev;     // unjittered projection * modelview, previous displayed frame
uniform float motion_y_sign;

in vec2 vary_fragcoord;

void main()
{
    float depth = texture(depthMap, vary_fragcoord).r;
    vec4 ndc = vec4(vary_fragcoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inv_view_proj_cur * ndc;

    // both ends go through the same projection math, so an unchanged camera gives exactly zero
    vec4 cur = view_proj_cur * world;
    vec4 prev = view_proj_prev * world;

    vec2 motion = vec2(0.0);
    if (cur.w * prev.w > 0.0)   // in front of both cameras; otherwise there is no previous position (zero)
    {
        motion = (prev.xy / prev.w - cur.xy / cur.w) * 0.5;
        motion.y *= motion_y_sign;
        motion = clamp(motion, vec2(-2.0), vec2(2.0));   // anything past a screen width is off screen; stays finite in RG16F
    }
    frag_color = vec4(motion, 0.0, 0.0);
}
