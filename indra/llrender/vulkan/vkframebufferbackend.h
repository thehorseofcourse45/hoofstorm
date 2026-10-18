// Vulkan framebuffer implementation for the standalone renderer.
#pragma once
#include "llframebufferbackend.h"
#include "vkcontext.h"
#include <array>
#include <map>

class VulkanRenderStateBackend;
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
};
