/**
 * @file class1/deferred/fsr2AvatarVelocityF.glsl
 *
 * FSR 2 temporal upscaler: avatar motion vectors, fragment stage (body and rigged mesh share it).
 */

/*[EXTRA_CODE_HERE]*/

// Writes prevUv - curUv (UV units, the same convention and Y sign as fsr2MotionF.glsl) for pixels of an avatar, over the camera
// motion that pass already wrote. The vertex stages hand over the unjittered clip position this frame and last frame; dividing
// here keeps the interpolation perspective correct.
//
// The pass has no depth attachment. A pixel is dropped when the avatar surface is behind what the scene drew there (a wall, another
// avatar), compared in linear depth with a tolerance because the scene was rendered with the jittered projection and this pass
// with the unjittered one.

out vec4 frag_color;

uniform sampler2D depthMap;      // scene depth, GL style 0..1
uniform vec2 depth_params;       // near, far of the camera
uniform float motion_y_sign;

in vec4 vary_cur_clip;
in vec4 vary_prev_clip;

float linear_depth(float d)
{
    float z = d * 2.0 - 1.0;
    return 2.0 * depth_params.x * depth_params.y / (depth_params.y + depth_params.x - z * (depth_params.y - depth_params.x));
}

void main()
{
    if (vary_prev_clip.w <= 0.0 || vary_cur_clip.w <= 0.0)
    {
        discard;    // no previous position in front of the camera: keep the camera motion
    }
    float scene = texelFetch(depthMap, ivec2(gl_FragCoord.xy), 0).r;
    if (linear_depth(gl_FragCoord.z) > linear_depth(scene) * 1.01 + 0.05)
    {
        discard;
    }
    vec2 motion = (vary_prev_clip.xy / vary_prev_clip.w - vary_cur_clip.xy / vary_cur_clip.w) * 0.5;
    motion.y *= motion_y_sign;
    frag_color = vec4(clamp(motion, vec2(-2.0), vec2(2.0)), 0.0, 0.0);
}
