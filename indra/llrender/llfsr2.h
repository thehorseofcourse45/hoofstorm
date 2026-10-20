#pragma once
// FSR 2/3.1 temporal upscaling: quality divisors, render scale, jitter sequence length, mip bias and the jitter projection offset.
#include <cmath>

// Per-axis downscale divisor for Native AA, Quality, Balanced, Performance, Ultra Performance. Out of range clamps to Ultra Performance.
inline float fsr2QualityDivisor(unsigned quality)
{
    static const float kDivisor[5] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return kDivisor[quality > 4 ? 4 : quality];
}

inline float fsr2RenderScale(unsigned quality)
{
    return 1.0f / fsr2QualityDivisor(quality);
}

// FSR 2 on: the preset scale wins (Native AA is 1.0). Off: the scale from the other paths, passed through.
inline float effectiveRenderScaleFsr2(bool fsr2Enabled, unsigned quality, float multiplier)
{
    return fsr2Enabled ? fsr2RenderScale(quality) : multiplier;
}

// The upscaler runs when the scene is rendered below display size, or in Native AA (quality 0), which is its anti-aliasing at scale 1.0.
inline bool fsr2ShouldUpscale(unsigned quality, float renderScale)
{
    return quality == 0 || renderScale < 1.0f;
}

// Number of Halton jitter phases before the sequence repeats: AMD's 8 * (display / render)^2, rounded up.
// The preset divisors use AMD's fixed tabulated counts (native 1.0 gives ceil(8) = 8).
inline int fsr2JitterPhaseCount(float divisor)
{
    if (std::fabs(divisor - 1.5f) < 1e-3f) return 18;
    if (std::fabs(divisor - 1.7f) < 1e-3f) return 23;
    if (std::fabs(divisor - 2.0f) < 1e-3f) return 32;
    if (std::fabs(divisor - 3.0f) < 1e-3f) return 72;
    return (int)std::ceil(8.0f * divisor * divisor - 1e-4f);
}

// Texture LOD bias for the lower-resolution render so detail matches the display resolution: log2(render / display) - 1.
inline float fsr2MipBias(float renderW, float displayW)
{
    return std::log2(renderW / displayW) - 1.0f;
}

// The jitter phase after `phase`: wraps at `count`, never negative (a phase left over from a longer sequence wraps into range).
inline int fsr2NextPhase(int phase, int count)
{
    if (count <= 0) return 0;
    int p = phase % count;
    if (p < 0) p += count;
    return (p + 1 == count) ? 0 : p + 1;
}

// Radical inverse of index in base: the Halton sequence, same arithmetic as the FidelityFX SDK's.
inline float fsr2Halton(int index, int base)
{
    float f = 1.0f, result = 0.0f;
    for (int i = index; i > 0;)
    {
        f /= (float)base;
        result = result + f * (float)(i % base);
        i = (int)std::floor((float)i / (float)base);
    }
    return result;
}

// Sub-pixel jitter in render pixels for a phase, as ffxFsr3UpscalerGetJitterOffset: Halton(2, 3) at index phase + 1, centred.
// Never (0, 0): base 2 gives 0.5 only at index 1, where base 3 gives 1/3.
inline void fsr2JitterOffset(int phase, int count, float& outX, float& outY)
{
    const int index = (count > 0 ? ((phase % count) + count) % count : 0) + 1;
    outX = fsr2Halton(index, 2) - 0.5f;
    outY = fsr2Halton(index, 3) - 0.5f;
}

// Camera moves larger than this in one displayed frame (teleports) are treated as cuts.
constexpr float kFsr2CameraCutMeters = 32.0f;

// The previous frame cannot be reprojected: no history, a new render size, a region shift (agent coordinates moved)
// or a teleport-sized camera jump. The caller then sets the previous view-projection to the current one.
inline bool fsr2HistoryCut(bool hadHistory, unsigned renderW, unsigned renderH, unsigned lastW, unsigned lastH,
                           bool regionShift, float cameraJumpMeters)
{
    return !hadHistory || renderW != lastW || renderH != lastH || regionShift || cameraJumpMeters > kFsr2CameraCutMeters;
}

// Y sign of the motion vectors (fsr2MotionF.glsl `motion_y_sign`). They are UV units, current pixel to previous position,
// and FSR 3.1 reads previous = uv + motion in the image's memory UV (row 0 = v 0). Vulkan offscreen targets are not Y-flipped
// (row 0 is ndc y = -1), so the texture UV the shader works in already is that memory UV: +1. This is the one place to flip
// if the in-world check shows vertical reprojection going the wrong way.
constexpr float kFsr2MotionYSign = 1.0f;

// Column-major projection offset (a translation in NDC) applying a sub-pixel jitter given in render pixels.
inline void fsr2JitterProjection(float jx, float jy, float renderW, float renderH, float out[16])
{
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    out[12] = 2.0f * jx / renderW;
    out[13] = -2.0f * jy / renderH;
}
