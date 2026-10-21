/**
 * @file class1/deferred/fsr2AvatarVelocityV.glsl
 *
 * FSR 2 temporal upscaler: rigged mesh velocity, vertex stage.
 */

// Rigged mesh palettes are in agent space (objectSkinV.glsl), so clip = unjittered view-projection * palette * position, once with
// this frame's palette and last frame's camera for the previous position. Not built on the object skinning feature because that
// include defines the single palette this needs two of.

uniform mat4 view_proj_cur;     // unjittered projection * modelview, this frame
uniform mat4 view_proj_prev;    // unjittered projection * modelview, previous displayed frame
uniform mat3x4 matrixPalette[MAX_JOINTS_PER_MESH_OBJECT];
uniform mat3x4 matrixPalettePrev[MAX_JOINTS_PER_MESH_OBJECT];

in vec3 position;
in vec4 weight4;

out vec4 vary_cur_clip;
out vec4 vary_prev_clip;

mat3x4 palette_at(int i, bool prev)
{
    return prev ? matrixPalettePrev[i] : matrixPalette[i];
}

vec3 skin_position(vec3 p, bool prev)
{
    vec4 w = fract(weight4);
    vec4 index = floor(weight4);
    index = clamp(index, vec4(0.0), vec4(MAX_JOINTS_PER_MESH_OBJECT - 1));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    // same blend as objectSkinV.glsl: rotation from the upper 3x3, translation from the w row of each column
    mat3x4 a = palette_at(int(index.x), prev);
    mat3x4 b = palette_at(int(index.y), prev);
    mat3x4 c = palette_at(int(index.z), prev);
    mat3x4 d = palette_at(int(index.w), prev);
    mat3 m = mat3(a) * w.x + mat3(b) * w.y + mat3(c) * w.z + mat3(d) * w.w;
    vec3 t = vec3(a[0].w, a[1].w, a[2].w) * w.x + vec3(b[0].w, b[1].w, b[2].w) * w.y +
             vec3(c[0].w, c[1].w, c[2].w) * w.z + vec3(d[0].w, d[1].w, d[2].w) * w.w;
    return m * p + t;
}

void main()
{
    vary_cur_clip = view_proj_cur * vec4(skin_position(position, false), 1.0);
    vary_prev_clip = view_proj_prev * vec4(skin_position(position, true), 1.0);
    gl_Position = vary_cur_clip;
}
