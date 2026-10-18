#include "vkframebufferbackend.h"
#include "vkquerybackend.h"
#include "vkbackends.h"
#include "vktexturebackend.h"
#include "glconsts.h"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace
{
constexpr U32 COLOR0 = 0x8CE0, DEPTH = 0x8D00, COMPLETE = 0x8CD5;
constexpr U32 COLOR_BIT = 0x4000, DEPTH_BIT = 0x0100, BACK = 0x0405;
}

void VulkanFramebufferBackend::fail(const char* message)
{
    fprintf(stderr, "FATAL: Vulkan framebuffer: %s\n", message);
    abort();
}

VulkanFramebufferBackend::VulkanFramebufferBackend(VkContext* ctx, VulkanRenderStateBackend* state) : mCtx(ctx), mState(state)
{
    ctx->framebuffers = this;
}
VulkanFramebufferBackend::~VulkanFramebufferBackend() { mCtx->framebuffers = nullptr; }
U32 VulkanFramebufferBackend::createFramebuffer() { mObjects[mNext] = {}; return mNext++; }
VulkanFramebufferBackend::Framebuffer& VulkanFramebufferBackend::object(U32 framebuffer)
{
    auto it = mObjects.find(framebuffer);
    if (it == mObjects.end()) fail("unknown framebuffer");
    return it->second;
}
void VulkanFramebufferBackend::destroyFramebuffer(U32 framebuffer)
{
    if (!framebuffer) return;
    if (framebuffer == mDraw) bind(0);
    if (framebuffer == mRead) mRead = 0;
    mObjects.erase(framebuffer);
}
void VulkanFramebufferBackend::bind(U32 framebuffer)
{
    if (framebuffer) object(framebuffer);
    endRendering();
    mDraw = mRead = framebuffer;
}
void VulkanFramebufferBackend::bindRead(U32 framebuffer)
{
    if (framebuffer) object(framebuffer);
    mRead = framebuffer;
}
void VulkanFramebufferBackend::attach(U32 framebuffer, U32 attachment, U32 target, U32 texture, S32 level)
{
    if (!framebuffer || framebuffer != mDraw || level < 0) fail("invalid attachment binding or level");
    endRendering();
    Attachment a{texture, target, level};
    if (texture && hasStorage(a)) imageOf(a);     // a render target that ran out of memory attaches anyway; drawing to it is skipped
    if (attachment == DEPTH) object(framebuffer).depth = a;
    else if (attachment >= COLOR0 && attachment < COLOR0 + 4) object(framebuffer).color[attachment - COLOR0] = a;
    else fail("unsupported attachment");
}

bool VulkanFramebufferBackend::hasStorage(const Attachment& a) const
{
    if (!a.texture) return true;
    auto it = mCtx->textures->mTextures.find(a.texture);
    return it != mCtx->textures->mTextures.end() && it->second.image;
}
bool VulkanFramebufferBackend::drawable()
{
    if (!mDraw) return true;
    const auto& f = object(mDraw);
    for (U32 i = 0; i < f.drawCount; ++i) if (!hasStorage(f.color[i])) return false;
    return hasStorage(f.depth);
}
VulkanFramebufferBackend::Image VulkanFramebufferBackend::imageOf(const Attachment& a)
{
    if (!a.texture) return {};
    auto it = mCtx->textures->mTextures.find(a.texture);
    if (it == mCtx->textures->mTextures.end() || !it->second.image) fail("attachment texture has no storage");
    auto& t = it->second;
    if ((U32) a.level >= t.levels || !t.attachmentCapable) fail("texture level or format is not renderable");
    U32 layer = 0;
    if (a.target >= glc::TEXTURE_CUBE_MAP_POSITIVE_X && a.target <= glc::TEXTURE_CUBE_MAP_NEGATIVE_Z)
        layer = a.target - glc::TEXTURE_CUBE_MAP_POSITIVE_X;
    else if (a.target != glc::TEXTURE_2D && a.target != glc::TEXTURE_RECTANGLE) fail("unsupported attachment target");
    if (layer >= t.layers) fail("attachment layer outside texture");
    const bool first = a.level == 0 && layer == 0;      // nearly every attachment; skips the string-keyed lookup
    const std::string key{'A', (char) a.level, (char) layer};   // short enough for no heap allocation; viewOf keys are longer
    VkImageView found = first ? t.aView : VK_NULL_HANDLE;
    if (!found)
    {
        auto view = t.views.find(key);
        if (view != t.views.end()) found = view->second;
    }
    if (!found)
    {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = t.info.format;
        vi.subresourceRange = {VkImageAspectFlags(t.info.kind == VulkanTextureBackend::Kind::D32 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                               (U32) a.level, 1, layer, 1};
        VK_CHECK(vkCreateImageView(mCtx->device, &vi, nullptr, &found));
        t.views.emplace(key, found);
    }
    if (first) t.aView = found;
    return {t.image, found, t.info.format,
            {std::max(1u, t.width >> a.level), std::max(1u, t.height >> a.level)},
            VkImageAspectFlags(t.info.kind == VulkanTextureBackend::Kind::D32 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
            a.texture, (U32) a.level, layer};
}
VulkanFramebufferBackend::Image VulkanFramebufferBackend::colorImage(U32 framebuffer, U32 attachment)
{
    if (!framebuffer)
    {
        if (attachment != BACK && attachment != COLOR0) fail("default framebuffer has only a back buffer");
        return {mCtx->images[mCtx->imageIndex], mCtx->views[mCtx->imageIndex], mCtx->swapFormat, mCtx->extent, VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT)};
    }
    if (attachment < COLOR0 || attachment >= COLOR0 + 4) fail("invalid color read buffer");
    return imageOf(object(framebuffer).color[attachment - COLOR0]);
}
VulkanFramebufferBackend::Image VulkanFramebufferBackend::depthImage(U32 framebuffer)
{
    if (!framebuffer) return {mCtx->depthImage, mCtx->depthView, mCtx->depthFormat, mCtx->extent, VK_IMAGE_ASPECT_DEPTH_BIT};
    return imageOf(object(framebuffer).depth);
}
VulkanFramebufferBackend::Image VulkanFramebufferBackend::readImage(bool depth)
{
    Image image = depth ? depthImage(mRead) : colorImage(mRead, mRead ? object(mRead).readBuffer : mDefaultRead);
    if (!image.image) fail("read buffer has no attachment");
    return image;
}
U32 VulkanFramebufferBackend::checkStatus()
{
    if (!mDraw || !drawable()) return COMPLETE;
    const auto& f = object(mDraw);
    VkExtent2D size{};
    bool any = false;
    for (U32 i = 0; i <= 4; ++i)
    {
        Image image = imageOf(i == 4 ? f.depth : f.color[i]);
        if (!image.image) continue;
        if ((i == 4) != (image.aspect == VK_IMAGE_ASPECT_DEPTH_BIT)) return 0x8CD6;
        if (any && (size.width != image.extent.width || size.height != image.extent.height)) return 0x8CD6;
        size = image.extent;
        any = true;
    }
    if (!any) return 0x8CD7;
    for (U32 i = 0; i < f.drawCount; ++i) if (!f.color[i].texture) return 0x8CDB;
    return COMPLETE;
}
void VulkanFramebufferBackend::setDrawBuffers(U32 count)
{
    if (count > 4 || (!mDraw && count > 1)) fail("invalid draw buffer count");
    endRendering();
    if (mDraw) object(mDraw).drawCount = count;
    else mDefaultDrawCount = count;
}
void VulkanFramebufferBackend::setDrawBufferBack()
{
    if (mDraw) fail("back buffer belongs to the window framebuffer");
    setDrawBuffers(1);
}
void VulkanFramebufferBackend::setReadBuffer(U32 buffer)
{
    if (buffer != 0)
    {
        if (!mRead && buffer != BACK) fail("invalid window read buffer");
        if (mRead && (buffer < COLOR0 || buffer >= COLOR0 + 4)) fail("invalid framebuffer read buffer");
    }
    if (mRead) object(mRead).readBuffer = buffer;
    else mDefaultRead = buffer;
}
void VulkanFramebufferBackend::setClearDepth(F64 depth) { mClearDepth = (float) std::clamp(depth, 0.0, 1.0); }
U32 VulkanFramebufferBackend::colorCount() const { return mDraw ? mObjects.at(mDraw).drawCount : mDefaultDrawCount; }
std::array<VkFormat, 4> VulkanFramebufferBackend::colorFormats()
{
    std::array<VkFormat, 4> formats{};
    for (U32 i = 0; i < colorCount(); ++i) formats[i] = colorImage(mDraw, COLOR0 + i).format;
    return formats;
}
VkFormat VulkanFramebufferBackend::depthFormat() { return depthImage(mDraw).format; }
VkExtent2D VulkanFramebufferBackend::extent()
{
    if (!mDraw) return mCtx->extent;
    auto& f = object(mDraw);
    for (const auto& a : f.color) if (a.texture) return imageOf(a).extent;
    if (f.depth.texture) return imageOf(f.depth).extent;
    fail("framebuffer has no attachments");
}
void VulkanFramebufferBackend::transition(const Image& image, VkImageLayout layout)
{
    if (!image.image) return;
    if (image.texture)
        mCtx->textures->transition(mCtx->textures->mTextures.at(image.texture), mCtx->slot().cmd, layout);
    else mCtx->transitionWindow(image.aspect == VK_IMAGE_ASPECT_DEPTH_BIT, layout);
}
void VulkanFramebufferBackend::beginFrame(const float clear[4])
{
    mRendering = false;
    mDraw = mRead = 0;
    mDefaultDrawCount = 1;
    mDefaultRead = BACK;
    mDefaultClear = true;
    std::copy(clear, clear + 4, mFrameClear.begin());
}
void VulkanFramebufferBackend::endRendering()
{
    if (mRendering)
    {
        if (mCtx->queries) { mCtx->queries->endRendering(); mCtx->queries->passEnd(); }
        vkCmdEndRendering(mCtx->slot().cmd); mRendering = false;
    }
}
void VulkanFramebufferBackend::beginRendering(U32 clearMask)
{
    const VkImageLayout depthLayout = mSampledDepth && !(clearMask & DEPTH_BIT) ?
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    if (mRendering && depthLayout != mRenderingDepthLayout) endRendering();
    if (mRendering) return;
    if (!mCtx->recording) fail("draw outside frame");
    if (const U32 status = checkStatus(); status != COMPLETE)
    {
        fprintf(stderr, "Vulkan framebuffer %u incomplete, status 0x%x\n", mDraw, status);
        fail("incomplete framebuffer");
    }
    const U32 requestedClear = clearMask;
    const bool initialWindowClear = !mDraw && mDefaultClear;
    if (initialWindowClear) { clearMask |= COLOR_BIT | DEPTH_BIT; mDefaultClear = false; }
    VkRenderingAttachmentInfo colors[4]{};
    mCtx->textures->beginBarrierBatch();
    for (U32 i = 0; i < colorCount(); ++i)
    {
        Image image = colorImage(mDraw, COLOR0 + i);
        transition(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = image.view;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = clearMask & COLOR_BIT ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const auto& colorClear = initialWindowClear && !(requestedClear & COLOR_BIT) ? mFrameClear : mClear;
        std::copy(colorClear.begin(), colorClear.end(), colors[i].clearValue.color.float32);
    }
    Image image = depthImage(mDraw);
    transition(image, depthLayout);
    mCtx->textures->flushBarriers(mCtx->slot().cmd);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = image.view;
    depth.imageLayout = depthLayout;
    depth.loadOp = clearMask & DEPTH_BIT ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = mSampledDepth ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = initialWindowClear && !(requestedClear & DEPTH_BIT) ? 1.0f : mClearDepth;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, extent()};
    info.layerCount = 1;
    info.colorAttachmentCount = colorCount();
    info.pColorAttachments = colors;
    info.pDepthAttachment = image.image ? &depth : nullptr;
    ++mCtx->statRenderPasses;
    ++mCtx->statPassDraws[{mDraw, info.renderArea.extent.width, info.renderArea.extent.height, colorCount()}].second;
    vkCmdBeginRendering(mCtx->slot().cmd, &info);
    if (mCtx->queries) { mCtx->queries->passBegin({mDraw, info.renderArea.extent.width, info.renderArea.extent.height, colorCount()}); mCtx->queries->beginRendering(); }
    mRendering = true;
    mRenderingDepthLayout = depthLayout;
}
bool VulkanFramebufferBackend::ensureFrame()
{
    if (mCtx->recording) return true;
    const U32 draw = mDraw, read = mRead;      // beginFrame resets the bindings to the window
    const float black[4] = {0, 0, 0, 1};
    if (!mCtx->beginFrame(black)) return false;
    mDraw = draw; mRead = read;
    return true;
}

void VulkanFramebufferBackend::clear(U32 mask)
{
    prepareDraw();
    if (mask & ~(COLOR_BIT | DEPTH_BIT)) fail("stencil clears need a stencil attachment");
    if (!mask) return;
    if (!ensureFrame() || !drawable()) return;     // minimized, or a render target without memory: nothing to clear
    const auto& s = mState->s;
    assert(!(mask & COLOR_BIT) || (s.colorMask[0] && s.colorMask[1] && s.colorMask[2] && s.colorMask[3]));
    assert(!(mask & DEPTH_BIT) || s.depthWrite);
    if (!mRendering && !s.scissorTest) { beginRendering(mask); return; }
    beginRendering();
    std::vector<VkClearAttachment> attachments;
    if (mask & COLOR_BIT) for (U32 i = 0; i < colorCount(); ++i)
    {
        VkClearAttachment a{};
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.colorAttachment = i;
        std::copy(mClear.begin(), mClear.end(), a.clearValue.color.float32);
        attachments.push_back(a);
    }
    if ((mask & DEPTH_BIT) && depthImage(mDraw).image)
    {
        VkClearAttachment a{};
        a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        a.clearValue.depthStencil.depth = mClearDepth;
        attachments.push_back(a);
    }
    auto size = extent();
    int x0 = 0, y0 = 0, x1 = (int) size.width, y1 = (int) size.height;
    if (s.scissorTest)
    {
        x0 = std::clamp(s.scissor[0], 0, x1);
        y0 = std::clamp(s.scissor[1], 0, y1);
        x1 = std::clamp(s.scissor[0] + std::max(0, s.scissor[2]), x0, x1);
        y1 = std::clamp(s.scissor[1] + std::max(0, s.scissor[3]), y0, y1);
    }
    if (x1 > x0 && y1 > y0 && !attachments.empty())
    {
        VkClearRect rect{{{x0, mDraw ? y0 : (int) size.height - y1}, {(U32) (x1 - x0), (U32) (y1 - y0)}}, 0, 1};
        vkCmdClearAttachments(mCtx->slot().cmd, (U32) attachments.size(), attachments.data(), 1, &rect);
    }
}
bool VulkanFramebufferBackend::prepareSample(U32 texture)
{
    auto& t = mCtx->textures->mTextures.at(texture);
    if (mDraw)
    {
        const auto& f = object(mDraw);
        for (const auto& a : f.color) if (a.texture == texture)
        {
            if (mCtx->tolerateFeedback) return false;
            fail("sampling a current color attachment creates feedback");
        }
        if (f.depth.texture == texture)
        {
            if (mState->s.depthTest && mState->s.depthWrite)
            {
                if (mCtx->tolerateFeedback) return false;
                fail("sampling a writable current depth attachment creates feedback");
            }
            mSampledDepth = true;
            if (t.layout != VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL) endRendering();
            mCtx->textures->transition(t, mCtx->slot().cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
            return true;
        }
    }
    if (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return true;
    endRendering();
    mCtx->textures->transition(t, mCtx->slot().cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

void VulkanFramebufferBackend::readPixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* data)
{
    if (!data || width < 0 || height < 0) fail("invalid readPixels arguments");
    if (!width || !height) return;
    if (!ensureFrame())     // minimized: no image to read
    {
        memset(data, 0, VulkanTextureBackend::sourceBytes(format, type, width, height, 1, 0));
        return;
    }
    Image image = readImage(format == glc::DEPTH_COMPONENT);
    if (x < 0 || y < 0 || x > (S32) image.extent.width - width || y > (S32) image.extent.height - height)
        fail("readPixels rectangle outside attachment");
    endRendering();
    transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VulkanTextureBackend::Tex converted;
    if (image.texture) converted.info = mCtx->textures->mTextures.at(image.texture).info;
    else converted.info = {image.format, image.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? VulkanTextureBackend::Kind::D32 : VulkanTextureBackend::Kind::RGBA8, 4, false};
    const size_t rowBytes = (size_t) width * converted.info.texelBytes;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = rowBytes * height;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(mCtx->device, &bi, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mCtx->device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(mCtx->device, &ai, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(mCtx->device, buffer, memory, 0));
    VkBufferImageCopy copy{};
    copy.imageSubresource = {image.aspect, image.level, image.layer, 1};
    copy.imageOffset = {x, mRead ? y : (S32) image.extent.height - y - height, 0};
    copy.imageExtent = {(U32) width, (U32) height, 1};
    vkCmdCopyImageToBuffer(mCtx->slot().cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    mCtx->flushCommands(VkContext::Stall::Readback);
    void* mapped;
    VK_CHECK(vkMapMemory(mCtx->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    std::vector<uint8_t> raw(rowBytes * height);
    for (S32 row = 0; row < height; ++row)
        memcpy(raw.data() + row * rowBytes, (uint8_t*) mapped + (mRead ? row : height - row - 1) * rowBytes, rowBytes);
    if (!image.texture && image.aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
        (image.format == VK_FORMAT_B8G8R8A8_UNORM || image.format == VK_FORMAT_B8G8R8A8_SRGB))
        for (size_t i = 0; i < raw.size(); i += 4) std::swap(raw[i], raw[i + 2]);
    mCtx->textures->convertForReadback(converted, raw.data(), format, type, width, height, 1, data);
    vkUnmapMemory(mCtx->device, memory);
    vkDestroyBuffer(mCtx->device, buffer, nullptr);
    vkFreeMemory(mCtx->device, memory, nullptr);
}

void VulkanFramebufferBackend::copyToTexture(U32 target, S32 level, S32 xoffset, S32 yoffset, S32 layer,
                                             S32 x, S32 y, S32 width, S32 height)
{
    if (!ensureFrame()) return;     // minimized: no image to copy from
    auto* t = mCtx->textures->bound(target);
    if (t && !t->image) return;     // texture storage not allocated (out of memory): skip, as uploads do
    if (!t || level < 0 || (U32) level >= t->levels || layer < 0 || (U32) layer >= t->layers)
        fail("invalid copy destination");
    if (target >= glc::TEXTURE_CUBE_MAP_POSITIVE_X && target <= glc::TEXTURE_CUBE_MAP_NEGATIVE_Z)
        layer = target - glc::TEXTURE_CUBE_MAP_POSITIVE_X;
    Image src = readImage(t->info.kind == VulkanTextureBackend::Kind::D32);
    U32 w = std::max(1u, t->width >> level), h = std::max(1u, t->height >> level);
    if (x < 0 || y < 0 || xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
        x > (S32) src.extent.width - width || y > (S32) src.extent.height - height ||
        xoffset > (S32) w - width || yoffset > (S32) h - height) fail("copy rectangle outside image");
    if (!width || !height) return;
    if (src.image == t->image || t->info.compressed) fail("overlapping or compressed framebuffer copy");
    endRendering();
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    mCtx->textures->transition(*t, mCtx->slot().cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {src.aspect, src.level, src.layer, 1};
    blit.dstSubresource = {src.aspect, (U32) level, (U32) layer, 1};
    blit.srcOffsets[0] = {x, mRead ? y : (S32) src.extent.height - y, 0};
    blit.srcOffsets[1] = {x + width, mRead ? y + height : (S32) src.extent.height - y - height, 1};
    blit.dstOffsets[0] = {xoffset, yoffset, 0};
    blit.dstOffsets[1] = {xoffset + width, yoffset + height, 1};
    VkFormatProperties sourceProps, destProps;
    vkGetPhysicalDeviceFormatProperties(mCtx->phys, src.format, &sourceProps);
    vkGetPhysicalDeviceFormatProperties(mCtx->phys, t->info.format, &destProps);
    if (!(sourceProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(destProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) fail("format does not support framebuffer blits");
    vkCmdBlitImage(mCtx->slot().cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
    mCtx->textures->transition(*t, mCtx->slot().cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
