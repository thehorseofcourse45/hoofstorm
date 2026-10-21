// VulkanFsrUpscaler: the viewer's only entry to AMD's FidelityFX FSR 3.1 temporal upscaler (vendored under ffx/, see ffx/README).
// It owns the SDK scratch memory, the upscaler context and the shared targets the SDK wants from its caller (dilated depth and
// motion, reconstructed previous depth). Recording goes into the caller's command buffer.
#pragma once

#include "vkcontext.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <memory>

// An image handed to the upscaler. The SDK creates its own views, so it needs the image and its description, not a view.
struct Fsr2Image
{
    VkImage image = VK_NULL_HANDLE;                    // null: not supplied (allowed for the reactive mask only)
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;                    // one mip level, one layer
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // current layout; ignored for the output, which is fully overwritten
};

// Render-size inputs. color: HDR linear. depth: D32_SFLOAT, 0 near 1 far. motion: RG16F/RG32F, current-to-previous screen
// motion in UV units (the wrapper scales by the render size). reactive: optional R8, 0..1.
struct Fsr2Inputs
{
    Fsr2Image color, depth, motion, reactive;
    float jitterX = 0, jitterY = 0;          // render pixels, as ffxFsr3UpscalerGetJitterOffset returns them
    float frameTimeMs = 16.7f;               // raised to 1 ms if lower
    float sharpness = 0;                     // 0 turns the SDK's sharpening pass off; up to 1
    bool reset = false;                      // drop history (camera cut, resize)
    bool depthInverted = false;              // not built (ffx/README): dispatch returns false
    float cameraNear = 0.1f, cameraFar = 1000.0f, fovY = 1.0f;   // metres, metres, radians; the SDK linearises depth with them
};

class VulkanFsrUpscaler
{
public:
    ~VulkanFsrUpscaler() { destroy(); }
    // False when ctx.failFsr2Create is set or the SDK reports an error; nothing is left allocated then.
    bool create(VkContext& ctx, uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH, uint32_t maxRenderW = 0, uint32_t maxRenderH = 0);
    // True when resize() only changes the dispatch size: same display size, render size within what the context was created for.
    bool resizesInPlace(uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH) const
    {
        return mContext && mDisplayW == displayW && mDisplayH == displayH && renderW <= mMaxRenderW && renderH <= mMaxRenderH;
    }
    // A smaller render size (dynamic scaling) keeps the context: recreating it waits for the device and rebuilds every SDK pipeline.
    // A larger one recreates it once, sized for the display, so later steps stay in place.
    bool resize(VkContext& ctx, uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH)
    {
        if (resizesInPlace(displayW, displayH, renderW, renderH))
        {
            ++ctx.fsr2CreateCalls;
            mRenderW = renderW;
            mRenderH = renderH;
            return true;
        }
        const bool grow = mContext && mDisplayW == displayW && mDisplayH == displayH;
        return create(ctx, displayW, displayH, renderW, renderH, grow ? displayW : renderW, grow ? displayH : renderH);
    }
    // Records the upscale into cmd. Missing color/depth/motion/output or depthInverted: false, nothing recorded. Otherwise every
    // supplied input and the output end in SHADER_READ_ONLY_OPTIMAL, the output readable by fragment and compute shaders; false
    // then means the SDK refused the dispatch (or ctx.failFsr2Dispatch is set).
    bool dispatch(VkCommandBuffer cmd, const Fsr2Inputs& in, const Fsr2Image& output);
    // Waits for the device: the SDK frees its images at once. Not while a recorded dispatch is still unsubmitted.
    void destroy();
    int64_t memoryBytes() const { return mMemBytes; }

private:
    VkContext* mCtx = nullptr;
    std::vector<uint8_t> mScratch;           // the backend's state; must outlive every SDK object
    FfxInterface mInterface{};
    std::unique_ptr<FfxFsr3UpscalerContext> mContext;    // 512 KB, too big for a stack
    bool mHasShared = false;
    uint32_t mSharedId = 0;                  // backend context that owns mShared
    FfxResourceInternal mShared[3]{};        // dilated depth, dilated motion vectors, reconstructed previous nearest depth
    uint32_t mDisplayW = 0, mDisplayH = 0, mRenderW = 0, mRenderH = 0;
    uint32_t mMaxRenderW = 0, mMaxRenderH = 0;   // what the context was created for
    int64_t mMemBytes = 0;
};
