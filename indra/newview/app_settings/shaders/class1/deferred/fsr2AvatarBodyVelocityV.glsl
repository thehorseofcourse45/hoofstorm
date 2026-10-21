/**
 * @file class1/deferred/fsr2AvatarBodyVelocityV.glsl
 *
 * FSR 2 temporal upscaler: avatar body mesh velocity, vertex stage.
 */

// The body palette (avatarSkinV.glsl, 45 vec4) already includes the modelview of the frame it was uploaded in, so it skins into eye
// space and only the projection is applied here. The previous palette was uploaded with last frame's camera, which puts camera
// motion into the difference as well. When the mesh has no usable previous palette (prev_valid 0) the previous position is the current
// clip position carried through the camera change instead. Not built on the skinning feature because that include defines the single
// palette.

uniform mat4 proj_unjittered;
uniform mat4 prev_clip_from_cur_clip;   // previous view-projection * inverse(current view-projection)
uniform float prev_valid;               // 1 when matrixPalettePrev holds last frame's palette
uniform vec4 matrixPalette[45];
uniform vec4 matrixPalettePrev[45];

in vec3 position;
in vec4 weight;

out vec4 vary_cur_clip;
out vec4 vary_prev_clip;

vec4 palette_at(int i, bool prev)
{
    return prev ? matrixPalettePrev[i] : matrixPalette[i];
}

vec4 clip_position(bool prev)
{
    int i = min(int(floor(weight.x)), 14);
    int next = min(i + 1, 14);
    float x = fract(weight.x);
    vec4 p = vec4(position, 1.0);
    vec3 eye = vec3(dot(mix(palette_at(i, prev), palette_at(next, prev), x), p),
                    dot(mix(palette_at(i + 15, prev), palette_at(next + 15, prev), x), p),
                    dot(mix(palette_at(i + 30, prev), palette_at(next + 30, prev), x), p));
    return proj_unjittered * vec4(eye, 1.0);
}

void main()
{
    vary_cur_clip = clip_position(false);
    vary_prev_clip = prev_valid > 0.5 ? clip_position(true) : prev_clip_from_cur_clip * vary_cur_clip;
    gl_Position = vary_cur_clip;
}
