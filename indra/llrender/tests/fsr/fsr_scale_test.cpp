// Pure checks for the FSR preset table and the effective render scale.
#include "llfsr.h"
#include <cstdio>

int main()
{
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failed; printf("FAIL %s\n", label); }
    };
    check(fsrQualityScale(0, 0.9f) == 0.77f, "ultra quality 0.77");
    check(fsrQualityScale(1, 0.9f) == 0.67f, "quality 0.67");
    check(fsrQualityScale(2, 0.9f) == 0.59f, "balanced 0.59");
    check(fsrQualityScale(3, 0.9f) == 0.50f, "performance 0.50");
    check(fsrQualityScale(4, 0.9f) == 0.9f, "custom uses the slider value");
    check(fsrQualityScale(4, 0.3f) == 0.5f, "custom clamps up to 0.5");
    check(fsrQualityScale(4, 2.0f) == 1.0f, "custom clamps down to 1.0");
    check(fsrQualityScale(99, 0.9f) == 0.50f, "out of range clamps to performance");
    check(effectiveRenderScale(false, 1, 0.9f, 1.0f) == 1.0f, "off, multiplier 1");
    check(effectiveRenderScale(false, 1, 0.9f, 0.8f) == 0.8f, "off keeps the render multiplier");
    check(effectiveRenderScale(true, 1, 0.9f, 0.8f) == 0.67f, "on overrides the multiplier");
    check(effectiveRenderScale(true, 4, 0.75f, 1.0f) == 0.75f, "on, custom");
    check(effectiveRenderScale(false, 1, 0.9f, 0.0f) == 1.0f, "off, invalid multiplier is 1");
    check(fsrSharpnessStops(1.0f) == 0.0f, "sharpness 1 is the sharpest (0 stops)");
    check(fsrSharpnessStops(0.0f) == 2.0f, "sharpness 0 is off (2 stops)");
    check(fsrSharpnessStops(0.9f) > 0.19f && fsrSharpnessStops(0.9f) < 0.21f, "default sharpness is 0.2 stops");
    {
        FsrDynamicScaler d; d.reset(1.0f);
        auto run = [&](float fps, int n) { bool ch = false; for (int i = 0; i < n; ++i) ch |= d.update(1.f / fps, 60.f, 0.67f); return ch; };
        check(!run(60.f, 200), "dynamic: at target at full scale does nothing");
        check(run(40.f, 400) && d.scale < 1.0f, "dynamic: slow frames lower the scale");
        for (int i = 0; i < 40; ++i) run(30.f, 400);
        check(d.scale == 0.67f, "dynamic: never below the floor");
        for (int i = 0; i < 40; ++i) run(200.f, 400);
        check(d.scale == 1.0f, "dynamic: lots of headroom climbs back to 1.0");
        FsrDynamicScaler e; e.reset(0.9f);
        check(!e.update(0.001f, 60.f, 0.5f) && e.scale == 0.9f, "dynamic: first frame no change");
        check(e.update(0.001f, 60.f, 0.95f) && e.scale == 0.95f, "dynamic: floor raise clamps the scale up");
        FsrDynamicScaler f; f.reset(0.8f);
        for (int i = 0; i < 400; ++i) f.update(1.f / 62.f, 60.f, 0.5f);
        check(f.scale == 0.8f, "dynamic: marginal headroom does not step up (no flip-flop)");
    }
    printf("FSR_SCALE_CHECKS=%d %s\n", checks, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
