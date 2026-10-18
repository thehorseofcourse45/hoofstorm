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

// Dynamic scaling: drops the render scale when the frame rate is below the target, raises it when there is headroom.
// The scale moves in 0.05 steps between the floor and 1.0, at most once per cooldown, from a smoothed frame time.
struct FsrDynamicScaler
{
    float scale = 1.0f;
    float avgDt = 0.f;
    float sinceChange = 0.f;
    int frames = 0;

    void reset(float startScale) { scale = startScale; avgDt = 0.f; sinceChange = 0.f; frames = 0; }

    // dt: last frame time in seconds. Returns true when scale changed (caller reallocates the render targets).
    bool update(float dt, float targetFps, float floorScale)
    {
        const float old = scale;
        if (floorScale < 0.5f) floorScale = 0.5f;
        if (scale < floorScale) scale = floorScale;
        if (scale > 1.0f) scale = 1.0f;
        if (dt <= 0.f || targetFps <= 0.f) return scale != old;
        avgDt = frames == 0 ? dt : avgDt + (dt - avgDt) * 0.05f;
        ++frames;
        sinceChange += dt;
        if (scale != old) { avgDt = 0.f; frames = 0; sinceChange = 0.f; return true; }
        if (frames < 30 || sinceChange < 2.0f) return false;
        const float fps = 1.f / avgDt;
        float next = scale;
        if (fps < targetFps * 0.93f)
        {
            next = scale - 0.05f;
            if (next < floorScale) next = floorScale;
        }
        else if (scale < 1.0f)
        {
            const float up = scale + 0.05f > 1.0f ? 1.0f : scale + 0.05f;
            // only step up if the frame rate predicted at the larger scale still meets the target (avoids flip-flopping)
            if (fps * (scale / up) * (scale / up) > targetFps * 1.03f) next = up;
        }
        if (next == scale) return false;
        scale = next; avgDt = 0.f; frames = 0; sinceChange = 0.f;
        return true;
    }
};
