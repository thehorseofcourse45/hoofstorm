// Pure checks for the FSR 2/3.1 temporal upscaler helpers.
#include "llfsr2.h"
#include <cstdio>
#include <cmath>

int main()
{
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failed; printf("FAIL %s\n", label); }
    };
    auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

    const float kDiv[5] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    const int kPhases[5] = {8, 18, 23, 32, 72};
    for (unsigned q = 0; q < 5; ++q)
    {
        check(near(fsr2QualityDivisor(q), kDiv[q], 1e-5f), "quality divisor table");
        check(near(fsr2RenderScale(q), 1.0f / kDiv[q], 1e-5f), "render scale is 1/divisor");
        check(fsr2JitterPhaseCount(kDiv[q]) == kPhases[q], "jitter phase count table");
    }
    check(near(fsr2QualityDivisor(9), 3.0f, 1e-5f), "quality 9 clamps to ultra performance");
    check(near(fsr2RenderScale(1), 0.6667f, 1e-4f), "quality render scale 0.6667");

    check(near(fsr2MipBias(640.f, 960.f), -1.585f, 0.01f), "mip bias 640/960");
    check(near(fsr2MipBias(480.f, 960.f), -2.0f, 0.01f), "mip bias 480/960");

    float m[16];
    fsr2JitterProjection(0.5f, -0.5f, 640.f, 360.f, m);
    check(near(m[12], 2.f * 0.5f / 640.f, 1e-7f), "jitter translation X");
    check(near(m[13], -2.f * -0.5f / 360.f, 1e-7f), "jitter translation Y");
    bool ident = true;
    for (int i = 0; i < 16; ++i)
    {
        if (i == 12 || i == 13) continue;
        ident = ident && m[i] == ((i % 5 == 0) ? 1.0f : 0.0f);
    }
    check(ident, "jitter matrix is identity elsewhere");

    check(near(effectiveRenderScaleFsr2(true, 1, 0.8f), 1.0f / 1.5f, 1e-5f), "fsr2 on, quality is 1/1.5");
    check(near(effectiveRenderScaleFsr2(true, 3, 0.8f), 0.5f, 1e-5f), "fsr2 on, performance is 1/2");
    check(near(effectiveRenderScaleFsr2(true, 4, 1.0f), 1.0f / 3.0f, 1e-5f), "fsr2 on, ultra performance is 1/3");
    check(effectiveRenderScaleFsr2(true, 0, 0.8f) == 1.0f, "fsr2 on, native AA is 1.0");
    check(effectiveRenderScaleFsr2(false, 1, 0.8f) == 0.8f, "fsr2 off returns the multiplier");
    check(effectiveRenderScaleFsr2(false, 3, 1.0f) == 1.0f, "fsr2 off, multiplier 1");

    check(fsr2ShouldUpscale(0, 1.0f), "native AA upscales at scale 1.0");
    check(fsr2ShouldUpscale(1, fsr2RenderScale(1)), "quality upscales below 1");
    check(fsr2ShouldUpscale(4, 0.5f), "ultra performance upscales below 1");
    check(!fsr2ShouldUpscale(1, 1.0f), "quality at scale 1.0 does not upscale");
    check(!fsr2ShouldUpscale(3, 1.2f), "performance above scale 1 does not upscale");

    // phase counter: one step per displayed frame, wraps at the count, never negative
    check(fsr2NextPhase(0, 8) == 1, "next phase 0 -> 1");
    check(fsr2NextPhase(7, 8) == 0, "next phase wraps at the count");
    check(fsr2NextPhase(71, 72) == 0, "next phase wraps at 72");
    check(fsr2NextPhase(30, 18) == 13, "phase past a smaller count wraps into range");
    check(fsr2NextPhase(-3, 8) >= 0 && fsr2NextPhase(-3, 8) < 8, "negative phase gives a phase in range");
    check(fsr2NextPhase(2147483647, 32) >= 0 && fsr2NextPhase(2147483647, 32) < 32, "largest int phase does not overflow");
    check(fsr2NextPhase(5, 0) == 0, "zero count gives phase 0");

    // jitter translation applied to an identity projection shifts a point by exactly one jitter in NDC
    {
        const float jx = 0.3125f, jy = -0.1875f, w = 640.f, h = 360.f;
        float t[16];
        fsr2JitterProjection(jx, jy, w, h, t);
        auto mul = [&](const float p[4], float out[4]) {
            for (int r = 0; r < 4; ++r)
                out[r] = t[0 * 4 + r] * p[0] + t[1 * 4 + r] * p[1] + t[2 * 4 + r] * p[2] + t[3 * 4 + r] * p[3];
        };
        const float p1[4] = {0.25f, -0.5f, 0.3f, 1.0f};
        const float p2[4] = {-0.8f, 0.6f, 0.7f, 2.0f};    // a perspective point: the shift scales with w, so NDC moves the same
        float c1[4], c2[4];
        mul(p1, c1);
        mul(p2, c2);
        check(near(c1[0] / c1[3] - p1[0] / p1[3], 2.f * jx / w, 1e-6f) && near(c1[1] / c1[3] - p1[1] / p1[3], -2.f * jy / h, 1e-6f),
              "jittered identity point moves one jitter in NDC");
        check(near(c2[0] / c2[3] - p2[0] / p2[3], 2.f * jx / w, 1e-6f) && near(c2[1] / c2[3] - p2[1] / p2[3], -2.f * jy / h, 1e-6f),
              "jittered point with w=2 moves one jitter in NDC");
        check(c1[2] == p1[2] && c1[3] == p1[3] && c2[2] == p2[2] && c2[3] == p2[3], "jitter leaves depth and w alone");
    }

    // jitter offsets: AMD's Halton(2,3) at index phase+1, centred, in render pixels
    {
        float x = 0.f, y = 0.f;
        fsr2JitterOffset(0, 8, x, y);
        check(near(x, 0.0f, 1e-6f) && near(y, 1.f / 3.f - 0.5f, 1e-6f), "jitter phase 0 is (0, -1/6)");
        fsr2JitterOffset(1, 8, x, y);
        check(near(x, -0.25f, 1e-6f) && near(y, 2.f / 3.f - 0.5f, 1e-6f), "jitter phase 1 is (-1/4, 1/6)");
        fsr2JitterOffset(2, 8, x, y);
        check(near(x, 0.25f, 1e-6f) && near(y, 1.f / 9.f - 0.5f, 1e-6f), "jitter phase 2 is (1/4, -7/18)");
        bool nonzero = true, inRange = true, repeats = true;
        for (unsigned q = 0; q < 5; ++q)
        {
            const int count = kPhases[q];
            for (int i = 0; i < count; ++i)
            {
                float ax, ay, bx, by;
                fsr2JitterOffset(i, count, ax, ay);
                fsr2JitterOffset(i + count, count, bx, by);
                nonzero = nonzero && !(ax == 0.f && ay == 0.f);
                inRange = inRange && ax >= -0.5f && ax < 0.5f && ay >= -0.5f && ay < 0.5f;
                repeats = repeats && ax == bx && ay == by;
            }
        }
        check(nonzero, "jitter is never (0, 0)");
        check(inRange, "jitter stays within half a pixel");
        check(repeats, "jitter repeats after the phase count");
    }

    // camera history: a frame without a usable previous frame resets (mPrevViewProj = current, zero motion)
    check(!fsr2HistoryCut(true, 1280, 720, 1280, 720, false, 0.5f), "same size, small move keeps history");
    check(fsr2HistoryCut(false, 1280, 720, 1280, 720, false, 0.f), "no history is a cut");
    check(fsr2HistoryCut(true, 1129, 635, 1280, 720, false, 0.f), "new render width is a cut");
    check(fsr2HistoryCut(true, 1280, 719, 1280, 720, false, 0.f), "new render height is a cut");
    check(fsr2HistoryCut(true, 1280, 720, 1280, 720, true, 0.f), "region shift is a cut");
    check(fsr2HistoryCut(true, 1280, 720, 1280, 720, false, 40.f), "teleport-sized jump is a cut");
    check(!fsr2HistoryCut(true, 1280, 720, 1280, 720, false, kFsr2CameraCutMeters), "jump at the limit keeps history");

    // which sizes create and dispatch the upscaler
    check(fsr2UseUpscaler(1, 853, 480, 1280, 720), "quality below display size upscales");
    check(fsr2UseUpscaler(0, 1280, 720, 1280, 720), "native AA at display size runs");
    check(!fsr2UseUpscaler(1, 1280, 720, 1280, 720), "a preset at display size takes the plain path");
    check(!fsr2UseUpscaler(1, 0, 480, 1280, 720) && !fsr2UseUpscaler(0, 1280, 720, 0, 0), "a zero size (minimized) is skipped");
    check(!fsr2UseUpscaler(0, 1300, 720, 1280, 720) && !fsr2UseUpscaler(1, 640, 800, 1280, 720), "render above display on an axis is skipped");

    // LOD bias only inside the jittered main scene of an upscaled frame
    check(near(fsr2SceneLodBias(true, true, 640.f, 960.f), fsr2MipBias(640.f, 960.f), 1e-6f), "scene bias inside the bracket");
    check(fsr2SceneLodBias(false, true, 640.f, 960.f) == 0.f, "no bias outside the jittered main scene (shadows, probes, UI)");
    check(fsr2SceneLodBias(true, false, 640.f, 960.f) == 0.f, "no bias without the upscaler targets (plain path)");
    check(fsr2SceneLodBias(true, true, 0.f, 960.f) == 0.f && fsr2SceneLodBias(true, true, 640.f, 0.f) == 0.f, "no bias for a zero size");

    // one Y decision: the jitter Y sign is the opposite of the motion Y sign, X is never flipped
    {
        float jx = 0, jy = 0;
        fsr2DispatchJitter(0.25f, -0.125f, jx, jy);
        check(kFsr2JitterYSign == -kFsr2MotionYSign && kFsr2MotionYSign == kFsr2RowsFollowNdcY, "jitter and motion Y signs follow one constant");
        check(jx == 0.25f && jy == -0.125f * kFsr2JitterYSign, "dispatch jitter keeps X and applies the Y sign");
    }

    // camera near, far and vertical fov from a GL perspective, also through a jitter translation
    {
        const float n = 0.5f, f = 512.f, fovy = 1.0f, aspect = 1.6f;
        float p[16] = {};
        const float t = 1.f / std::tan(fovy * 0.5f);
        p[0] = t / aspect; p[5] = t; p[10] = (f + n) / (n - f); p[11] = -1.f; p[14] = 2.f * f * n / (n - f);
        float cn, cf, cfov;
        fsr2CameraFromProjection(p, cn, cf, cfov);
        check(near(cn, n, 1e-3f) && std::fabs(cf - f) / f < 1e-3f && near(cfov, fovy, 1e-5f), "camera from projection");
        float j[16], pj[16];
        fsr2JitterProjection(0.3f, -0.4f, 640.f, 360.f, j);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += j[k * 4 + r] * p[c * 4 + k];
                pj[c * 4 + r] = s;
            }
        float jn, jf, jfov;
        fsr2CameraFromProjection(pj, jn, jf, jfov);
        check(jn == cn && jf == cf && jfov == cfov, "jitter does not change the camera values");
    }

    // fallback to FSR 1.0: the preset whose scale is closest to the temporal mode's; Native AA renders full size (FSR 1.0 stays off)
    {
        check(fsr2FallbackFsrQuality(1) == 1 && fsr2FallbackFsrQuality(2) == 2 && fsr2FallbackFsrQuality(3) == 3,
              "Quality, Balanced, Performance fall back to FSR 1.0 Quality, Balanced, Performance");
        check(fsr2FallbackFsrQuality(4) == 3, "Ultra Performance (1/3) falls back to FSR 1.0 Performance, its lowest scale");
        check(fsr2FallbackFsrQuality(0) < 0, "Native AA falls back to full size without FSR 1.0");
        check(fsr2FallbackFsrQuality(9) == 3, "an out-of-range quality clamps like the divisor table");
        bool closest = true;
        for (unsigned q = 1; q <= 4; ++q)
        {
            const int fb = fsr2FallbackFsrQuality(q);
            for (unsigned other = 0; other < 4; ++other)
                closest = closest && std::fabs(fsrQualityScale(fb, 1.f) - fsr2RenderScale(q)) <= std::fabs(fsrQualityScale(other, 1.f) - fsr2RenderScale(q));
        }
        check(closest, "every fallback preset is the closest FSR 1.0 scale");
    }

    // plain-path decision: the upscaled output is read only after an accepted dispatch with no failure signal
    {
        check(fsr2ReadUpscaledOutput(true, false, false), "accepted dispatch, no failure: the frame reads the upscaled output");
        check(!fsr2ReadUpscaledOutput(false, false, false), "refused dispatch: plain path");
        check(!fsr2ReadUpscaledOutput(true, true, false), "failure rule tripped: plain path");
        check(!fsr2ReadUpscaledOutput(true, false, true), "last dispatch failed (render thread, one frame late): plain path");
    }

    // FXAA/SMAA after the post chain: skipped only on frames the upscaler anti-aliased
    {
        check(fsr2PostAntiAlias(1, true) == 0 && fsr2PostAntiAlias(2, true) == 0, "upscaled frame: no FXAA or SMAA on top");
        check(fsr2PostAntiAlias(1, false) == 1 && fsr2PostAntiAlias(2, false) == 2 && fsr2PostAntiAlias(0, false) == 0,
              "plain-path frame (or FSR 2 off): the chosen FXAA/SMAA runs");
    }

    printf("FSR2_HELPERS %s checks=%d\n", failed ? "FAIL" : "PASS", checks);
    return failed ? 1 : 0;
}
