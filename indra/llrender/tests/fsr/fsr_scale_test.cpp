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
    check(fsrQualityScale(0) == 0.77f, "ultra quality 0.77");
    check(fsrQualityScale(1) == 0.67f, "quality 0.67");
    check(fsrQualityScale(2) == 0.59f, "balanced 0.59");
    check(fsrQualityScale(3) == 0.50f, "performance 0.50");
    check(fsrQualityScale(99) == 0.50f, "out of range clamps to performance");
    check(effectiveRenderScale(false, 1, 1.0f) == 1.0f, "off, multiplier 1");
    check(effectiveRenderScale(false, 1, 0.8f) == 0.8f, "off keeps the render multiplier");
    check(effectiveRenderScale(true, 1, 0.8f) == 0.67f, "on overrides the multiplier");
    check(effectiveRenderScale(false, 1, 0.0f) == 1.0f, "off, invalid multiplier is 1");
    printf("FSR_SCALE_CHECKS=%d %s\n", checks, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
