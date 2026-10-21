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
// Camera motion, plus one rigid movement per moving avatar: the pipeline sends the screen boxes of the visible avatars that moved
// since the last frame (avatar_box, see collectFsr2AvatarBoxes), three vec4 each: (min xy, max xy), (min z, max z) in the
// unjittered NDC used here, and (movement since last frame in agent space). A pixel inside a box's rectangle whose depth is
// inside its z range is taken to be on the avatar, and last frame it was that movement away. Limb animation is not covered.

#define MAX_AVATARS 32           // kFsr2ReactiveAvatarsMax

out vec4 frag_color;

uniform sampler2D depthMap;
uniform mat4 inv_view_proj_cur;  // inverse(unjittered projection * modelview), this frame
uniform mat4 view_proj_cur;      // unjittered projection * modelview, this frame
uniform mat4 view_proj_prev;     // unjittered projection * modelview, previous displayed frame
uniform float motion_y_sign;
uniform int avatar_count;        // boxes in avatar_box, 0..MAX_AVATARS
uniform vec4 avatar_box[96];     // 3 * MAX_AVATARS; a literal, the Vulkan backend reads the array size as plain digits

in vec2 vary_fragcoord;

void main()
{
    float depth = texture(depthMap, vary_fragcoord).r;
    vec4 ndc = vec4(vary_fragcoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inv_view_proj_cur * ndc;
    vec4 prev_world = world;
    for (int i = 0; i < avatar_count && i < MAX_AVATARS; ++i)
    {
        vec4 rect = avatar_box[i * 3];
        vec4 zr = avatar_box[i * 3 + 1];
        if (all(greaterThanEqual(ndc.xy, rect.xy)) && all(lessThanEqual(ndc.xy, rect.zw)) && ndc.z >= zr.x && ndc.z <= zr.y)
        {
            prev_world = vec4(world.xyz / world.w - avatar_box[i * 3 + 2].xyz, 1.0);
            break;
        }
    }

    // both ends go through the same projection math, so an unchanged camera gives exactly zero
    vec4 cur = view_proj_cur * world;
    vec4 prev = view_proj_prev * prev_world;

    vec2 motion = vec2(0.0);
    if (cur.w * prev.w > 0.0)   // in front of both cameras; otherwise there is no previous position (zero)
    {
        motion = (prev.xy / prev.w - cur.xy / cur.w) * 0.5;
        motion.y *= motion_y_sign;
        motion = clamp(motion, vec2(-2.0), vec2(2.0));   // anything past a screen width is off screen; stays finite in RG16F
    }
    frag_color = vec4(motion, 0.0, 0.0);
}
