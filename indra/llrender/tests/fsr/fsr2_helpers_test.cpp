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

    printf("FSR2_HELPERS %s checks=%d\n", failed ? "FAIL" : "PASS", checks);
    return failed ? 1 : 0;
}
