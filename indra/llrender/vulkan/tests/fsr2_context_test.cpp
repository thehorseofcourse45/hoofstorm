// Creates and destroys the vendored FidelityFX FSR 3.1 upscaler context (render 640x360, display 960x540) on a validated headless
// device, in the FP16 variant when the device supports it and in the FP32 fallback, and checks for zero validation messages.
#include "vkcontext.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <cstring>

static bool g_fp16;
static FfxErrorCode (*g_sdkCaps)(FfxInterface*, FfxDeviceCapabilities*);
static FfxErrorCode capsFromContext(FfxInterface* i, FfxDeviceCapabilities* caps)
{
    const FfxErrorCode r = g_sdkCaps(i, caps);
    caps->fp16Supported = g_fp16;       // the SDK only checks the extension list; what counts is what the device enabled
    return r;
}
static unsigned g_sdkMessages;
static void sdkMessage(FfxMsgType type, const wchar_t* text)
{
    if (type == FFX_MESSAGE_TYPE_ERROR) { ++g_sdkMessages; fwprintf(stderr, L"FFX error: %ls\n", text); }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);    // a crash inside the SDK must not swallow the progress lines
    VkContext ctx;
    unsigned checks = 0;
    auto check = [&](bool ok, const char* label) { if (!ok) throw std::runtime_error(label); ++checks; printf("PASS %s\n", label); };
    try
    {
        check(ctx.init(GetModuleHandle(nullptr), nullptr, true) && ctx.validation, "validated GPU initialized");
        check(ffxFsr3UpscalerGetEffectVersion() == FFX_SDK_MAKE_VERSION(3, 1, 4), "upscaler component is FSR 3.1.4");
        check(ffxFsr3UpscalerGetJitterPhaseCount(640, 960) > 0, "jitter sequence available");

        VkDeviceContext deviceContext{ctx.device, ctx.phys, vkGetDeviceProcAddr};
        const size_t scratchBytes = ffxGetScratchMemorySizeVK(ctx.phys, FFX_FSR3UPSCALER_CONTEXT_COUNT);
        std::vector<uint8_t> scratch(scratchBytes);
        for (int fp16 = ctx.fp16Math ? 1 : 0; fp16 >= 0; --fp16)
        {
            g_fp16 = fp16 != 0;
            FfxFsr3UpscalerContextDescription desc{};
            desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
            desc.maxRenderSize = {640, 360};
            desc.maxUpscaleSize = {960, 540};
            desc.fpMessage = sdkMessage;
            memset(scratch.data(), 0, scratch.size());
            check(ffxGetInterfaceVK(&desc.backendInterface, ffxGetDeviceVK(&deviceContext), scratch.data(), scratch.size(),
                                    FFX_FSR3UPSCALER_CONTEXT_COUNT) == FFX_OK, "VK backend interface");
            g_sdkCaps = desc.backendInterface.fpGetDeviceCapabilities;
            desc.backendInterface.fpGetDeviceCapabilities = capsFromContext;

            FfxFsr3UpscalerContext upscaler;
            check(ffxFsr3UpscalerContextCreate(&upscaler, &desc) == FFX_OK, g_fp16 ? "upscaler context created (FP16)" : "upscaler context created (FP32)");
            FfxEffectMemoryUsage usage{};
            check(ffxFsr3UpscalerContextGetGpuMemoryUsage(&upscaler, &usage) == FFX_OK && usage.totalUsageInBytes > 0, "upscaler reports its VRAM");
            printf("upscaler VRAM %.1f MB\n", usage.totalUsageInBytes / 1048576.0);
            check(ffxFsr3UpscalerContextDestroy(&upscaler) == FFX_OK, "upscaler context destroyed");
            VK_CHECK(vkDeviceWaitIdle(ctx.device));
        }
        {
            // a permutation the viewer does not build must fail the create cleanly, not crash
            g_fp16 = false;
            FfxFsr3UpscalerContextDescription desc{};
            desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
            desc.maxRenderSize = {640, 360};
            desc.maxUpscaleSize = {960, 540};
            memset(scratch.data(), 0, scratch.size());
            check(ffxGetInterfaceVK(&desc.backendInterface, ffxGetDeviceVK(&deviceContext), scratch.data(), scratch.size(),
                                    FFX_FSR3UPSCALER_CONTEXT_COUNT) == FFX_OK, "VK backend interface (unsupported permutation)");
            g_sdkCaps = desc.backendInterface.fpGetDeviceCapabilities;
            desc.backendInterface.fpGetDeviceCapabilities = capsFromContext;
            FfxFsr3UpscalerContext upscaler;
            check(ffxFsr3UpscalerContextCreate(&upscaler, &desc) != FFX_OK, "inverted-depth permutation is refused");
            ffxFsr3UpscalerContextDestroy(&upscaler);
            VK_CHECK(vkDeviceWaitIdle(ctx.device));
        }
        check(g_sdkMessages == 0, "no SDK error messages");
        ctx.shutdown();
        check(ctx.validationErrors == 0 && ctx.validationWarnings == 0, "zero validation messages including teardown");
        printf("FSR2_CONTEXT PASS (%u checks)\n", checks);
        return 0;
    }
    catch (const std::exception& e)
    {
        printf("FAIL: %s (validation %u errors, %u warnings)\n", e.what(), ctx.validationErrors, ctx.validationWarnings);
        return 1;
    }
}
