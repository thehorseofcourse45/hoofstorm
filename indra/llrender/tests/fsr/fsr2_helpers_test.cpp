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

    printf("FSR2_HELPERS %s checks=%d\n", failed ? "FAIL" : "PASS", checks);
    return failed ? 1 : 0;
}
