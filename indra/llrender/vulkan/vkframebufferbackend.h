// Vulkan framebuffer implementation for the standalone renderer.
#pragma once
#include "llframebufferbackend.h"
#include "llrenderbackend.h"
#include "vkcontext.h"
#include <array>
#include <atomic>
#include <map>
#include <memory>

class VulkanRenderStateBackend;
class VulkanFsrUpscaler;
class VulkanFramebufferBackend final : public LLFramebufferBackend
{
public:
    VulkanFramebufferBackend(VkContext* ctx, VulkanRenderStateBackend* state);
    ~VulkanFramebufferBackend() override;
    U32 createFramebuffer() override;
    void destroyFramebuffer(U32 framebuffer) override;
    void bind(U32 framebuffer) override;
    void bindRead(U32 framebuffer) override;
    U32 getBound() const override { return mDraw; }
    void attach(U32 framebuffer, U32 attachment, U32 target, U32 texture, S32 level) override;
    U32 checkStatus() override;
    void setDrawBuffers(U32 count) override;
    void setDrawBufferNone() override { setDrawBuffers(0); }
    void setDrawBufferBack() override;
    void setReadBuffer(U32 buffer) override;
    void setClearColor(F32 r, F32 g, F32 b, F32 a) override { mClear = {{r, g, b, a}}; }
    void setClearDepth(F64 depth) override;
    void clear(U32 mask) override;
    void readPixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* data) override;

    void beginFrame(const float clear[4]);
    void beginRendering(U32 clearMask = 0);
    void endRendering();
    bool isRendering() const { return mRendering; }
    bool drawable();     // false when an attached texture has no image (out of memory): draws and clears are skipped
    VkExtent2D extent();
    std::array<VkFormat, 4> colorFormats();
    VkFormat depthFormat();
    U32 colorCount() const;
    bool prepareSample(U32 texture);   // false: feedback tolerated, the caller must sample a fallback instead
    void prepareDraw() { mSampledDepth = false; }
    // Begins the frame if none is recording (after a restore from minimized the first call may be a clear, copy or read, not a
    // draw), keeping the current framebuffer bindings. False while minimized: there is no frame.
    bool ensureFrame();
    // Copies into the active texture; used by LLTextureBackend's framebuffer-copy methods.
    void copyToTexture(U32 target, S32 level, S32 xoffset, S32 yoffset, S32 layer,
                       S32 x, S32 y, S32 width, S32 height);

    // FSR 2 temporal upscale, recorded into the current frame (dispatchRenderBackendFsr2 in llrenderbackend.h). Ends dynamic
    // rendering; the next draw reopens it. Afterwards the inputs and the output are SHADER_READ_ONLY_OPTIMAL. False, with nothing
    // recorded, when no frame is recording, sizes are zero or render > display, an image is unsuitable (more than one mip or layer,
    // depth not D32/D16, a stencil format, output without storage usage or sRGB, an image smaller than its size, one image twice),
    // the upscaler cannot be created, or after three failures in a row (fsr2Failed). Also false when the SDK refuses the dispatch;
    // the layouts have changed then. Every false except no frame and a skipped size counts toward the three. The upscaler is
    // created on first use and recreated when a size changes; that waits for the device, so it runs on the queue-owning thread.
    bool dispatchFsr2(const RenderBackendFsr2Params& params);
    // Frees the upscaler and its history, sets upscalerMemBytes to 0 and clears the failure state.
    void releaseFsr2();
    bool fsr2Failed() const { return mFsr2Failed; }
    bool fsr2LastOk() const { return mFsr2LastOk; }     // the last dispatchFsr2 call returned true
    // The last recorded dispatch reset the history: the caller asked for it, or the swapchain was recreated since the one before.
    bool fsr2LastReset() const { return mFsr2LastReset; }

private:
    struct Attachment { U32 texture = 0, target = 0; S32 level = 0; };
    struct Framebuffer
    {
        std::array<Attachment, 4> color{};
        Attachment depth;
        U32 drawCount = 1, readBuffer = 0x8CE0;
    };
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        U32 texture = 0, level = 0, layer = 0;
    };
    bool hasStorage(const Attachment& a) const;
    Image imageOf(const Attachment& a);
    Image colorImage(U32 framebuffer, U32 attachment);
    Image depthImage(U32 framebuffer);
    Image readImage(bool depth);
    void transition(const Image& image, VkImageLayout layout);
    Framebuffer& object(U32 framebuffer);
    [[noreturn]] static void fail(const char* message);
    VkContext* mCtx;
    VulkanRenderStateBackend* mState;
    std::map<U32, Framebuffer> mObjects;
    U32 mNext = 1, mDraw = 0, mRead = 0;
    U32 mDefaultDrawCount = 1, mDefaultRead = 0x0405;
    bool mRendering = false, mDefaultClear = false, mSampledDepth = false;
    VkImageLayout mRenderingDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::array<float, 4> mClear{}, mFrameClear{};
    float mClearDepth = 1.0f;

    bool fsr2Fail(const char* why);     // counts a failure; always false
    void retireFsr2();                  // submits first when the current recording still holds a dispatch, then frees
    std::unique_ptr<VulkanFsrUpscaler> mFsr2;
    uint32_t mFsr2Sizes[4]{};           // display w/h, render w/h of mFsr2
    uint64_t mFsr2Serial = ~0ull;       // VkContext::cmdSerial of the recording that holds the last dispatch
    int mFsr2Failures = 0;              // in a row
    std::atomic<bool> mFsr2Failed{false};     // read from the viewer thread while the render thread runs
    std::atomic<bool> mFsr2LastOk{false};     // same
    bool mFsr2LastReset = false;
    uint32_t mFsr2SwapchainGeneration = 0;      // VkContext::swapchainGeneration at the last recorded dispatch
};
