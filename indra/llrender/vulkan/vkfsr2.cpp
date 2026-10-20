#include "vkfsr2.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <algorithm>
#include <cwchar>

namespace
{
// The backend reports fp16Supported from the extension list, not from what the device enabled (ffx/README). One VkDevice per
// process (the backend keeps it in a static), so the choice is a static too.
bool sFp16 = false;
FfxErrorCode (*sSdkCaps)(FfxInterface*, FfxDeviceCapabilities*) = nullptr;
FfxErrorCode capsFromContext(FfxInterface* backend, FfxDeviceCapabilities* caps)
{
    const FfxErrorCode r = sSdkCaps(backend, caps);
    caps->fp16Supported = sFp16;
    return r;
}

void sdkMessage(FfxMsgType type, const wchar_t* text)
{
    fwprintf(stderr, L"FidelityFX %ls: %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? L"error" : L"warning", text);
}

bool isDepth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM; }

FfxResource resourceOf(const Fsr2Image& i, const wchar_t* name, FfxResourceStates state, bool storage)
{
    if (!i.image)
        return ffxGetResourceVK(nullptr, {}, name, state);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i.format;
    ci.extent = {i.width, i.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    return ffxGetResourceVK((void*) i.image, ffxGetImageResourceDescriptionVK(i.image, ci), name, state);
}
}

bool VulkanFsrUpscaler::create(VkContext& ctx, uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH)
{
    destroy();
    if (ctx.failFsr2Create)
        return false;
    mCtx = &ctx;
    mDisplayW = displayW; mDisplayH = displayH; mRenderW = renderW; mRenderH = renderH;

    VkDeviceContext device{ctx.device, ctx.phys, vkGetDeviceProcAddr};
    const size_t contexts = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1;      // + the context owning the shared targets
    mScratch.assign(ffxGetScratchMemorySizeVK(ctx.phys, contexts), 0);
    if (ffxGetInterfaceVK(&mInterface, ffxGetDeviceVK(&device), mScratch.data(), mScratch.size(), contexts) != FFX_OK)
    {
        destroy();
        return false;
    }
    sSdkCaps = mInterface.fpGetDeviceCapabilities;
    sFp16 = ctx.fp16Math;       // shaderFloat16, shaderInt16 and the 16-bit storage features, all enabled
    mInterface.fpGetDeviceCapabilities = capsFromContext;

    FfxFsr3UpscalerContextDescription desc{};
    desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize = {renderW, renderH};
    desc.maxUpscaleSize = {displayW, displayH};
    desc.backendInterface = mInterface;
    desc.fpMessage = sdkMessage;
    mContext = std::make_unique<FfxFsr3UpscalerContext>();     // a failed create is destroyed too (ffx/README)
    bool ok = ffxFsr3UpscalerContextCreate(mContext.get(), &desc) == FFX_OK
              && mInterface.fpCreateBackendContext(&mInterface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &mSharedId) == FFX_OK;
    mHasShared = ok;
    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    ok = ok && ffxFsr3UpscalerGetSharedResourceDescriptions(mContext.get(), &shared) == FFX_OK;
    const FfxCreateResourceDescription* descs[3] = {&shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth};
    for (int i = 0; i < 3 && ok; ++i)
        ok = mInterface.fpCreateResource(&mInterface, descs[i], mSharedId, &mShared[i]) == FFX_OK;
    FfxEffectMemoryUsage upscaler{}, targets{};
    ok = ok && ffxFsr3UpscalerContextGetGpuMemoryUsage(mContext.get(), &upscaler) == FFX_OK
         && mInterface.fpGetEffectGpuMemoryUsage(&mInterface, mSharedId, &targets) == FFX_OK;
    if (!ok)
    {
        destroy();
        return false;
    }
    mMemBytes = int64_t(upscaler.totalUsageInBytes + targets.totalUsageInBytes);
    ctx.upscalerMemBytes += mMemBytes;
    return true;
}

bool VulkanFsrUpscaler::dispatch(VkCommandBuffer cmd, const Fsr2Inputs& in, const Fsr2Image& output)
{
    if (!mHasShared || in.depthInverted || !in.color.image || !in.depth.image || !in.motion.image || !output.image)
        return false;

    // The SDK's own barriers assume its inputs are already compute-readable (src = SHADER_READ), so whatever wrote them last (a
    // render pass, a copy) is made visible here. The output is fully overwritten: its old contents are discarded.
    VkImageMemoryBarrier barriers[5];
    uint32_t count = 0;
    auto add = [&](const Fsr2Image& i, VkImageLayout from, VkImageLayout to, VkAccessFlags dst) {
        VkImageMemoryBarrier& b = barriers[count++];
        b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = i.image;
        b.subresourceRange = {VkImageAspectFlags(isDepth(i.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    };
    for (const Fsr2Image* i : {&in.color, &in.depth, &in.motion, &in.reactive})
        if (i->image)
            add(*i, i->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
    add(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, count, barriers);

    FfxFsr3UpscalerDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = resourceOf(in.color, L"FSR2_Color", FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.depth = resourceOf(in.depth, L"FSR2_Depth", FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.motionVectors = resourceOf(in.motion, L"FSR2_Motion", FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.reactive = resourceOf(in.reactive, L"FSR2_Reactive", FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.dilatedDepth = mInterface.fpGetResource(&mInterface, mShared[0]);
    d.dilatedMotionVectors = mInterface.fpGetResource(&mInterface, mShared[1]);
    d.reconstructedPrevNearestDepth = mInterface.fpGetResource(&mInterface, mShared[2]);
    d.output = resourceOf(output, L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
    d.jitterOffset = {in.jitterX, in.jitterY};
    d.motionVectorScale = {float(mRenderW), float(mRenderH)};
    d.renderSize = {mRenderW, mRenderH};
    d.upscaleSize = {mDisplayW, mDisplayH};
    d.enableSharpening = in.sharpness > 0;
    d.sharpness = std::min(in.sharpness, 1.0f);
    d.frameTimeDelta = std::max(in.frameTimeMs, 1.0f);
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.cameraNear;
    d.cameraFar = in.cameraFar;
    d.cameraFovAngleVertical = in.fovY;
    const bool ok = ffxFsr3UpscalerContextDispatch(mContext.get(), &d) == FFX_OK;

    // the SDK leaves the output in GENERAL (its UNORDERED_ACCESS); the passes after it sample it
    count = 0;
    add(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, barriers);
    return ok;
}

void VulkanFsrUpscaler::destroy()
{
    if (!mCtx)
        return;
    vkDeviceWaitIdle(mCtx->device);     // the SDK destroys images, views and pipelines immediately
    if (mContext)
        ffxFsr3UpscalerContextDestroy(mContext.get());
    for (FfxResourceInternal& r : mShared)
        if (r.internalIndex)
            mInterface.fpDestroyResource(&mInterface, r, mSharedId);
    if (mHasShared)
        mInterface.fpDestroyBackendContext(&mInterface, mSharedId);
    mCtx->upscalerMemBytes -= mMemBytes;
    mMemBytes = 0;
    mHasShared = false;
    for (FfxResourceInternal& r : mShared)
        r = {};
    mContext.reset();
    mScratch.clear();
    mCtx = nullptr;
}
