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

// Column-major projection offset (a translation in NDC) applying a sub-pixel jitter given in render pixels.
inline void fsr2JitterProjection(float jx, float jy, float renderW, float renderH, float out[16])
{
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    out[12] = 2.0f * jx / renderW;
    out[13] = -2.0f * jy / renderH;
}
