#pragma once
// FSR 1.0 upscaling: preset render scales, the effective scene scale and the sharpness mapping.

// Render scale per axis for Ultra Quality, Quality, Balanced, Performance; 4 is Custom (the slider value, 0.5 to 1.0).
inline float fsrQualityScale(unsigned quality, float customScale)
{
    static const float kScale[4] = {0.77f, 0.67f, 0.59f, 0.50f};
    if (quality == 4) return customScale < 0.5f ? 0.5f : (customScale > 1.0f ? 1.0f : customScale);
    return kScale[quality > 3 ? 3 : quality];
}

// FSR on: the preset (or custom value) wins. Off: the plain render multiplier, when it is a real reduction.
inline float effectiveRenderScale(bool fsrEnabled, unsigned quality, float customScale, float multiplier)
{
    if (fsrEnabled) return fsrQualityScale(quality, customScale);
    return multiplier > 0.f && multiplier < 1.f ? multiplier : 1.0f;
}

// The user-facing sharpness (0 off .. 1 sharpest) as AMD's RCAS attenuation in stops (2 off .. 0 sharpest).
inline float fsrSharpnessStops(float sharpness)
{
    const float s = sharpness < 0.f ? 0.f : (sharpness > 1.f ? 1.f : sharpness);
    return (1.f - s) * 2.f;
}
