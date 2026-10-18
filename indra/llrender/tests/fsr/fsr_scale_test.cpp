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
    printf("FSR_SCALE_CHECKS=%d %s\n", checks, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
