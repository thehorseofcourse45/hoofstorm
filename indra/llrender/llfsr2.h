#pragma once
// FSR 2/3.1 temporal upscaling: quality divisors, render scale, jitter sequence length, mip bias and the jitter projection offset.
#include "llfsr.h"
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

// Texture LOD bias for one pass: fsr2MipBias only for the jittered main-scene geometry (G-buffer, lighting, forward alpha and
// water) of a frame the upscaler handles; 0 for everything else (shadow maps, probe and mirror cube snapshots, impostors, HUD,
// UI, previews, snapshots and plain-path frames), and for zero sizes.
inline float fsr2SceneLodBias(bool jitteredMainScene, bool upscalerTargets, float renderW, float displayW)
{
    if (!jitteredMainScene || !upscalerTargets || renderW <= 0.0f || displayW <= 0.0f) return 0.0f;
    return fsr2MipBias(renderW, displayW);
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

// The one Y-direction decision for FSR 2: +1 when render-target memory rows grow with NDC y (row 0 is NDC y = -1), which is
// the case for the Vulkan backend's offscreen targets (positive-height viewport); -1 for a Y-flipped target. Both signs below
// follow from it. Flip this if the in-world check shows vertical reprojection or vertical jitter going the wrong way.
constexpr float kFsr2RowsFollowNdcY = 1.0f;

// Y sign of the motion vectors (fsr2MotionF.glsl `motion_y_sign`). They are UV units, current pixel to previous position,
// and FSR 3.1 reads previous = uv + motion in the image's memory UV (row 0 = v 0). With rows following NDC y the texture UV
// the shader works in already is that memory UV: +1.
constexpr float kFsr2MotionYSign = kFsr2RowsFollowNdcY;

// Y sign of the jitter handed to the SDK. fsr2JitterProjection uses AMD's Y = -2 jy / H, which moves the content by -jy
// memory rows when rows follow NDC y, while FSR 3.1 expects the content moved by +jy rows (its DX convention, row 0 at the
// top): the SDK gets -jy. X needs no flip (columns grow with NDC x in both conventions).
constexpr float kFsr2JitterYSign = -kFsr2RowsFollowNdcY;

// The jitter to report in the dispatch for a projection jittered by (jx, jy) render pixels with fsr2JitterProjection.
inline void fsr2DispatchJitter(float jx, float jy, float& outX, float& outY)
{
    outX = jx;
    outY = kFsr2JitterYSign * jy;
}

// The upscaler is created and dispatched for these sizes: none zero (minimized), the render size not above the display size
// on either axis, and below it unless the mode is Native AA (quality 0), which runs at the display size.
inline bool fsr2UseUpscaler(unsigned quality, unsigned renderW, unsigned renderH, unsigned displayW, unsigned displayH)
{
    if (!renderW || !renderH || !displayW || !displayH) return false;
    if (renderW > displayW || renderH > displayH) return false;
    return quality == 0 || renderW < displayW || renderH < displayH;
}

// Fallback when the temporal upscaler cannot run (LLPipeline::applyFsr2Fallback): the FSR 1.0 preset (0 Ultra Quality ..
// 3 Performance, llfsr.h) whose render scale is closest to this mode's, so the scene keeps its render size as far as FSR 1.0
// allows (Ultra Performance's 1/3 becomes Performance's 1/2, the lowest FSR 1.0 preset). -1 for Native AA: it renders at full
// size, where FSR 1.0 has nothing to upscale, so it stays off.
inline int fsr2FallbackFsrQuality(unsigned fsr2Quality)
{
    const float scale = fsr2RenderScale(fsr2Quality);
    if (scale >= 1.0f) return -1;
    int best = 0;
    for (int q = 1; q < 4; ++q)
        if (std::fabs(fsrQualityScale(q, 1.0f) - scale) < std::fabs(fsrQualityScale(best, 1.0f) - scale)) best = q;
    return best;
}

// A frame reads the upscaled output (mFSR2Map) only when its dispatch was accepted (with the render thread on: queued), the
// failure rule has not tripped, and the last dispatch the backend finished did not fail. With the render thread on that last
// signal is usually the previous frame's: a failure on the render thread shows one frame late, and that frame reads the output
// of the last accepted dispatch (or the cleared target). Otherwise the frame takes the plain path (bilinear stretch).
inline bool fsr2ReadUpscaledOutput(bool dispatchAccepted, bool backendFailed, bool lastDispatchFailed)
{
    return dispatchAccepted && !backendFailed && !lastDispatchFailed;
}

// FXAA (1) or SMAA (2) after the post chain: the temporal upscaler anti-aliases the frames it upscaled, every other frame
// (FSR 2 off, or a plain-path frame while its targets exist) runs the chosen one.
inline unsigned fsr2PostAntiAlias(unsigned fsaaType, bool upscaled)
{
    return upscaled ? 0u : fsaaType;
}

// Camera near and far (metres) and vertical field of view (radians) from a column-major GL perspective projection
// (depth -1..1), as the SDK wants them to linearise depth. A jitter translation in front of it does not change them.
inline void fsr2CameraFromProjection(const float m[16], float& outNear, float& outFar, float& outFovY)
{
    // m[10] = (f + n) / (n - f), m[14] = 2 f n / (n - f), m[5] = 1 / tan(fovY / 2)
    outNear = m[14] / (m[10] - 1.0f);
    outFar = m[14] / (m[10] + 1.0f);
    outFovY = 2.0f * std::atan(1.0f / m[5]);
}

// Reactive mask (fsr2ReactiveF.glsl). kFsr2ReactiveMax is the value for marked pixels (the shader also clamps to it);
// kFsr2ReactiveFlag is the G-buffer normal.w code to mark, negative for none. The G-buffer has no code for avatars or
// alpha-blended surfaces, so v1 marks nothing (see the shader); an existing GBUFFER_FLAG_* value can be set here if the
// in-world check shows ghosting on that class.
constexpr float kFsr2ReactiveMax = 0.9f;
constexpr float kFsr2ReactiveFlagNone = -1.0f;
constexpr float kFsr2ReactiveFlag = kFsr2ReactiveFlagNone;

// Column-major projection offset (a translation in NDC) applying a sub-pixel jitter given in render pixels.
inline void fsr2JitterProjection(float jx, float jy, float renderW, float renderH, float out[16])
{
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    out[12] = 2.0f * jx / renderW;
    out[13] = -2.0f * jy / renderH;
}
