#pragma once
// FSR 1.0 upscaling: preset render scales and the effective scene scale.

// Render scale per axis for Ultra Quality, Quality, Balanced, Performance.
inline float fsrQualityScale(unsigned quality)
{
    static const float kScale[4] = {0.77f, 0.67f, 0.59f, 0.50f};
    return kScale[quality > 3 ? 3 : quality];
}

// FSR on: the preset wins. Off: the plain render multiplier, when it is a real reduction.
inline float effectiveRenderScale(bool fsrEnabled, unsigned quality, float multiplier)
{
    if (fsrEnabled) return fsrQualityScale(quality);
    return multiplier > 0.f && multiplier < 1.f ? multiplier : 1.0f;
}
