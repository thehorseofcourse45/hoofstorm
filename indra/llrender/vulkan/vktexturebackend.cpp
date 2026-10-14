#include "vktexturebackend.h"
#include "glconsts.h"
#include "vkframebufferbackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
uint16_t floatToHalf(float value)
{
    uint32_t x;
    memcpy(&x, &value, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const uint32_t mant = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF)
    {
        return (uint16_t) (sign | 0x7C00 | (mant ? 0x200 : 0));            // inf, nan
    }
    const int32_t exp = (int32_t) ((x >> 23) & 0xFF) - 127 + 15;
    if (exp >= 31)
    {
        return (uint16_t) (sign | 0x7C00);
    }
    if (exp <= 0)
    {
        if (exp < -10)
        {
            return (uint16_t) sign;
        }
        const uint32_t m = mant | 0x800000;
        const int shift = 14 - exp;
        uint32_t half = m >> shift;
        if ((m >> (shift - 1)) & 1)
        {
            ++half;
        }
        return (uint16_t) (sign | half);
    }
    uint32_t h = sign | ((uint32_t) exp << 10) | (mant >> 13);
    if (mant & 0x1000)
    {
        ++h;
    }
    return (uint16_t) h;
}

float halfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t) (h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t x;
    if (exp == 0)
    {
        if (mant == 0)
        {
            x = sign;
        }
        else
        {
            exp = 1;
            while (!(mant & 0x400))
            {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FF;
            x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    }
    else if (exp == 31)
    {
        x = sign | 0x7F800000 | (mant << 13);
    }
    else
    {
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &x, 4);
    return f;
}

bool isCubeFace(U32 t)
{
    return t >= glc::TEXTURE_CUBE_MAP_POSITIVE_X && t <= glc::TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

uint32_t levelsFor(uint32_t w, uint32_t h)
{
    uint32_t levels = 1;
    for (uint32_t m = std::max(w, h); m > 1; m >>= 1)
    {
        ++levels;
    }
    return levels;
}

// number of components of a GL pixel format, and whether red and blue are swapped
int formatComps(U32 f, bool& bgr)
{
    bgr = false;
    switch (f)
    {
    case glc::RED:
    case glc::ALPHA:
    case glc::LUMINANCE:
    case glc::DEPTH_COMPONENT: return 1;
    case glc::RG:
    case glc::LUMINANCE_ALPHA: return 2;
    case glc::BGR: bgr = true; return 3;
    case glc::RGB: return 3;
    case glc::BGRA: bgr = true; return 4;
    case glc::RGBA: return 4;
    }
    return 0;
}

int typeBytes(U32 t)
{
    switch (t)
    {
    case glc::UNSIGNED_BYTE:
    case glc::BYTE: return 1;
    case glc::UNSIGNED_SHORT:
    case glc::SHORT:
    case glc::HALF_FLOAT: return 2;
    case glc::UNSIGNED_INT:
    case glc::INT:
    case glc::FLOAT: return 4;
    }
    return 0;
}

[[noreturn]] void fatal(const char* what, uint32_t value)
{
    fprintf(stderr, "FATAL: %s 0x%x is not supported by the Vulkan texture backend\n", what, value);
    abort();
}

float readComponent(const uint8_t* p, U32 type, bool swap)
{
    uint8_t b[4];
    const int n = typeBytes(type);
    for (int i = 0; i < n; ++i)
    {
        b[i] = swap ? p[n - 1 - i] : p[i];
    }
    switch (type)
    {
    case glc::UNSIGNED_BYTE: return b[0] / 255.0f;
    case glc::BYTE: return std::max((int8_t) b[0] / 127.0f, -1.0f);
    case glc::UNSIGNED_SHORT: { uint16_t v; memcpy(&v, b, 2); return v / 65535.0f; }
    case glc::HALF_FLOAT: { uint16_t v; memcpy(&v, b, 2); return halfToFloat(v); }
    case glc::FLOAT: { float v; memcpy(&v, b, 4); return v; }
    }
    fatal("pixel type", type);
}

uint8_t quantize(float v)
{
    return (uint8_t) (std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f);
}

VkSamplerAddressMode addressMode(S32 wrap)
{
    switch ((uint32_t) wrap)
    {
    case glc::CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case glc::MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case glc::CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkComponentSwizzle swizzleOf(S32 gl)
{
    switch ((uint32_t) gl)
    {
    case glc::RED: return VK_COMPONENT_SWIZZLE_R;
    case glc::GREEN: return VK_COMPONENT_SWIZZLE_G;
    case glc::BLUE: return VK_COMPONENT_SWIZZLE_B;
    case glc::ALPHA: return VK_COMPONENT_SWIZZLE_A;
    case glc::ONE: return VK_COMPONENT_SWIZZLE_ONE;
    }
    return VK_COMPONENT_SWIZZLE_ZERO;
}
} // namespace

// ----------------------------------------------------------------------------------------------------------------

VulkanTextureBackend::VulkanTextureBackend(VkContext* ctx) : mCtx(ctx)
{
    ctx->textures = this;
}

VulkanTextureBackend::~VulkanTextureBackend()
{
    vkDeviceWaitIdle(mCtx->device);
    for (auto& kv : mTextures)
    {
        destroyImage(kv.second);
    }
    for (auto& kv : mFallbacks)
    {
        destroyImage(kv.second);
    }
    for (auto& kv : mSamplers)
    {
        vkDestroySampler(mCtx->device, kv.second, nullptr);
    }
    if (mCtx->textures == this)
    {
        mCtx->textures = nullptr;
    }
}

void VulkanTextureBackend::unsupported(const char* what)
{
    fprintf(stderr, "FATAL: LLTextureBackend::%s is not implemented by the Vulkan skeleton yet (needs the framebuffer layer)\n", what);
    abort();
}

VulkanTextureBackend::FormatInfo VulkanTextureBackend::formatOf(U32 f)
{
    FormatInfo i;
    switch (f)
    {
    case glc::RGBA16:
        i = { VK_FORMAT_R16G16B16A16_UNORM, Kind::RGBA16, 8, false };
        break;
    case glc::RGB10_A2:
        i = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, Kind::RGB10A2, 4, false };
        break;
    case glc::RGBA8: case glc::RGBA: case glc::COMPRESSED_RGBA:
    case glc::RGB8: case glc::RGB: case glc::COMPRESSED_RGB:
        i = { VK_FORMAT_R8G8B8A8_UNORM, Kind::RGBA8, 4, false };
        break;
    case glc::SRGB8_ALPHA8: case glc::SRGB_ALPHA: case glc::COMPRESSED_SRGB_ALPHA:
    case glc::SRGB8: case glc::SRGB: case glc::COMPRESSED_SRGB:
        i = { VK_FORMAT_R8G8B8A8_SRGB, Kind::RGBA8, 4, false };
        break;
    case glc::R8: case glc::RED: case glc::COMPRESSED_RED: case glc::ALPHA8: case glc::LUMINANCE8: case glc::COMPRESSED_ALPHA:
    case glc::COMPRESSED_LUMINANCE:
        i = { VK_FORMAT_R8_UNORM, Kind::R8, 1, false };
        break;
    case glc::RG8: case glc::RG: case glc::COMPRESSED_RG: case glc::LUMINANCE8_ALPHA8: case glc::COMPRESSED_LUMINANCE_ALPHA:
        i = { VK_FORMAT_R8G8_UNORM, Kind::RG8, 2, false };
        break;
    case glc::RGBA16F: case glc::RGB16F:
        i = { VK_FORMAT_R16G16B16A16_SFLOAT, Kind::RGBA16F, 8, false };
        break;
    case glc::RG16F: i = { VK_FORMAT_R16G16_SFLOAT, Kind::RG16F, 4, false }; break;
    case glc::R16F: i = { VK_FORMAT_R16_SFLOAT, Kind::R16F, 2, false }; break;
    case glc::RGBA32F: case glc::RGB32F:
        i = { VK_FORMAT_R32G32B32A32_SFLOAT, Kind::RGBA32F, 16, false };
        break;
    case glc::RG32F: i = { VK_FORMAT_R32G32_SFLOAT, Kind::RG32F, 8, false }; break;
    case glc::R32F: i = { VK_FORMAT_R32_SFLOAT, Kind::R32F, 4, false }; break;
    case glc::R11F_G11F_B10F: i = { VK_FORMAT_B10G11R11_UFLOAT_PACK32, Kind::B10G11R11, 4, false }; break;
    case glc::DEPTH_COMPONENT16: case glc::DEPTH_COMPONENT24: case glc::DEPTH_COMPONENT32F:
        i = { VK_FORMAT_D32_SFLOAT, Kind::D32, 4, false };
        break;
    case glc::COMPRESSED_RGB_S3TC_DXT1: case glc::COMPRESSED_RGBA_S3TC_DXT1:
        i = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, Kind::BC, 8, true };
        break;
    case glc::COMPRESSED_RGBA_S3TC_DXT3: i = { VK_FORMAT_BC2_UNORM_BLOCK, Kind::BC, 16, true }; break;
    case glc::COMPRESSED_RGBA_S3TC_DXT5: i = { VK_FORMAT_BC3_UNORM_BLOCK, Kind::BC, 16, true }; break;
    default:
        fatal("internal format", f);
    }
    return i;
}

// ----------------------------------------------------------------------------------------------------------------
// one-time command buffers

void VulkanTextureBackend::submitNow(const std::function<void(VkCommandBuffer)>& record, bool waitForReadback)
{
    if (mCtx->recording && !waitForReadback)
    {
        mCtx->framebuffers->endRendering();
        record(mCtx->slot().cmd);
        return;
    }
    if (!waitForReadback)
    {
        record(mCtx->batchCommands());   // no frame is recording: ride along with the next submission instead of waiting
        ++mCtx->batchedUploads;
        return;
    }
    const auto reason=VkContext::Stall::Readback;
    mCtx->flushCommands(reason);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = mCtx->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(mCtx->device, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    record(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkCommandBuffer buffers[2];
    si.commandBufferCount = 0;
    if (VkCommandBuffer pending = mCtx->takeBatch()) buffers[si.commandBufferCount++] = pending;   // earlier work first
    buffers[si.commandBufferCount++] = cmd;
    si.pCommandBuffers = buffers;
    const auto start=std::chrono::steady_clock::now();
    VK_CHECK(vkQueueSubmit(mCtx->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(mCtx->queue));
    mCtx->completedFrame = mCtx->submittedFrame;
    ++mCtx->blockingSubmissions;
    mCtx->recordStall(reason,start);
    vkFreeCommandBuffers(mCtx->device, mCtx->pool, 1, &cmd);
}

static VkImageAspectFlags aspectOf(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

// The pipeline stages that touch an image while it is in a layout. A barrier only has to order the stages of the layout it leaves
// against the stages of the layout it enters; ALL_COMMANDS on both sides would also drain unrelated work.
static VkPipelineStageFlags stagesOf(VkImageLayout layout)
{
    if (!g_narrowBarriers) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | g_shaderStages;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return g_shaderStages;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;     // UNDEFINED and anything unusual
    }
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags srcAccess, VkAccessFlags dstAccess, uint32_t baseLevel, uint32_t levelCount, uint32_t layers)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, baseLevel, levelCount, 0, layers};
    ++g_barrierCount;
    vkCmdPipelineBarrier(cmd, stagesOf(from), stagesOf(to), 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ----------------------------------------------------------------------------------------------------------------
// images

void VulkanTextureBackend::destroyImage(Tex& t)
{
    if (!t.image)
    {
        return;
    }
    const auto device = mCtx->device;
    const auto image = t.image;
    const auto memory = t.memory;
    auto views = std::move(t.views);
    t.views.clear();
    t.cView = VK_NULL_HANDLE;
    mCtx->deferRelease([device, image, memory, views] {
        for (const auto& view : views) vkDestroyImageView(device, view.second, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
    });
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t.image = VK_NULL_HANDLE;
    t.memory = VK_NULL_HANDLE;
}

void VulkanTextureBackend::ensureImage(Tex& t, uint32_t w, uint32_t h, uint32_t layers, U32 glInternal, bool cube, bool cubeArray,
                                       bool rect)
{
    if (t.image && t.width == w && t.height == h && t.layers == layers && t.glInternal == glInternal && t.cube == cube &&
        t.cubeArray == cubeArray)
    {
        return;
    }
    destroyImage(t);

    const FormatInfo fi = formatOf(glInternal);
    t.info = fi;
    t.glInternal = glInternal;
    t.width = w;
    t.height = h;
    t.layers = layers;
    t.cube = cube;
    t.cubeArray = cubeArray;
    t.levels = rect ? 1 : levelsFor(w, h);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = (cube || cubeArray) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fi.format;
    ici.extent = {w, h, 1};
    ici.mipLevels = t.levels;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(mCtx->phys, fi.format, &props);
    const auto feature = fi.kind == Kind::D32 ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    t.attachmentCapable = !fi.compressed && (props.optimalTilingFeatures & feature);
    if (t.attachmentCapable)
        ici.usage |= fi.kind == Kind::D32 ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(mCtx->device, &ici, nullptr, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(mCtx->device, t.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(mCtx->device, &mai, nullptr, &t.memory));
    VK_CHECK(vkBindImageMemory(mCtx->device, t.image, t.memory, 0));
    ++imagesCreated;

    // GL starts a level out as zeros, Vulkan as garbage: clear the image, then leave it ready for sampling
    const VkImageAspectFlags aspect = aspectOf(fi.format);
    submitNow([&](VkCommandBuffer cmd) {
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                t.levels, layers);
        if (!fi.compressed)
        {
            VkImageSubresourceRange range{aspect, 0, t.levels, 0, layers};
            if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
            {
                VkClearDepthStencilValue v{1.0f, 0};
                vkCmdClearDepthStencilImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
            }
            else
            {
                VkClearColorValue v{};
                vkCmdClearColorImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
            }
        }
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, t.levels, layers);
    });
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanTextureBackend::upload(Tex& t, uint32_t level, uint32_t layer, uint32_t layerCount, int32_t x, int32_t y, uint32_t w,
                                  uint32_t h, const std::vector<uint8_t>& texels)
{
    if (!t.image || level >= t.levels || texels.empty())
    {
        return;
    }
    VkBuffer staging = mCtx->ringBuffer;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingOffset = 0;
    const bool useRing = mCtx->recording && texels.size() <= mCtx->ringSize;
    const bool useBatch = !mCtx->recording;
    if (useBatch)
    {
        memcpy(mCtx->batchStage(texels.size(), staging, stagingOffset), texels.data(), texels.size());
    }
    else if (useRing)
    {
        // 16-byte alignment covers all supported texel and compressed-block copy offsets.
        stagingOffset = mCtx->uploadRingAlloc(texels.data(), (uint32_t)texels.size(), VkContext::Stall::Upload, 16);
        ++mCtx->textureRingUploads;
    }
    else
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = texels.size();
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(mCtx->device, &bci, nullptr, &staging));
        ++mCtx->textureUploadAllocations;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(mCtx->device, staging, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(mCtx->device, &mai, nullptr, &stagingMemory));
        VK_CHECK(vkBindBufferMemory(mCtx->device, staging, stagingMemory, 0));
        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(mCtx->device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
        memcpy(mapped, texels.data(), texels.size());
        vkUnmapMemory(mCtx->device, stagingMemory);
    }

    const VkImageAspectFlags aspect = aspectOf(t.info.format);
    submitNow([&](VkCommandBuffer cmd) {
        transition(t, cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, t.levels, t.layers);
        VkBufferImageCopy region{};
        region.bufferOffset = stagingOffset;
        region.imageSubresource = {aspect, level, layer, layerCount};
        region.imageOffset = {x, y, 0};
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, t.levels, t.layers);
    });
    if (useBatch)
    {
        if (mCtx->batch.bytes > VkContext::kBatchLimit) mCtx->flushBatchBlocking();   // no frame may be coming (minimized)
    }
    else if (!useRing)
    {
        const auto device = mCtx->device;
        mCtx->deferRelease([device, staging, stagingMemory] {
            vkDestroyBuffer(device, staging, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
        });
    }
}

// ----------------------------------------------------------------------------------------------------------------
// pixel conversion

std::vector<uint8_t> VulkanTextureBackend::convertForUpload(const Tex& t, U32 format, U32 type, uint32_t w, uint32_t h, uint32_t count,
                                                            const void* src)
{
    bool bgr = false;
    const int comps = formatComps(format, bgr);
    const int tb = typeBytes(type);
    if (!comps || !tb)
    {
        fatal("pixel format/type", format);
    }
    const uint32_t texel = t.info.texelBytes;
    std::vector<uint8_t> out((size_t) w * h * count * texel);
    const uint32_t rowPixels = mRowLength > 0 ? (uint32_t) mRowLength : w;
    const size_t srcRowBytes = (size_t) rowPixels * comps * tb;
    const size_t srcLayerBytes = srcRowBytes * h;
    const uint8_t* base = (const uint8_t*) src;

    // fast path: RGBA bytes into an RGBA8 image
    if (t.info.kind == Kind::RGBA8 && comps == 4 && type == glc::UNSIGNED_BYTE && !bgr && !mSwapBytes)
    {
        for (uint32_t l = 0; l < count; ++l)
        {
            for (uint32_t y = 0; y < h; ++y)
            {
                memcpy(out.data() + ((size_t) l * h + y) * w * 4, base + l * srcLayerBytes + y * srcRowBytes, (size_t) w * 4);
            }
        }
        return out;
    }

    // fast path: 8-bit components into RGBA8/RG8/R8 (missing components 0, alpha 255, as the generic path yields)
    if (type == glc::UNSIGNED_BYTE && !mSwapBytes && (t.info.kind == Kind::RGBA8 || t.info.kind == Kind::RG8 || t.info.kind == Kind::R8))
    {
        const int outComps = t.info.kind == Kind::RGBA8 ? 4 : t.info.kind == Kind::RG8 ? 2 : 1;
        for (uint32_t l = 0; l < count; ++l)
        {
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t* p = base + l * srcLayerBytes + y * srcRowBytes;
                uint8_t* d = out.data() + ((size_t) l * h + y) * w * texel;
                for (uint32_t x = 0; x < w; ++x, p += comps, d += texel)
                {
                    uint8_t c[4] = {0, 0, 0, 255};
                    for (int i = 0; i < comps; ++i) c[i] = p[i];
                    if (bgr) std::swap(c[0], c[2]);
                    for (int i = 0; i < outComps; ++i) d[i] = c[i];
                }
            }
        }
        return out;
    }

    for (uint32_t l = 0; l < count; ++l)
    {
        for (uint32_t y = 0; y < h; ++y)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = base + l * srcLayerBytes + y * srcRowBytes + (size_t) x * comps * tb;
                float c[4] = {0.f, 0.f, 0.f, 1.f};
                for (int i = 0; i < comps; ++i)
                {
                    c[i] = readComponent(p + i * tb, type, mSwapBytes);
                }
                if (bgr)
                {
                    std::swap(c[0], c[2]);
                }
                uint8_t* d = out.data() + (((size_t) l * h + y) * w + x) * texel;
                switch (t.info.kind)
                {
                case Kind::RGBA16:
                    for (int k=0;k<4;++k)
                    {
                        const uint16_t v=uint16_t(std::clamp(c[k],0.f,1.f)*65535.f+0.5f);
                        memcpy(d+k*2,&v,2);
                    }
                    break;
                case Kind::RGB10A2:
                {
                    auto unorm=[](float v,uint32_t maximum) { return uint32_t(std::clamp(v,0.f,1.f)*maximum+0.5f); };
                    const uint32_t packed=unorm(c[0],1023) | (unorm(c[1],1023)<<10) | (unorm(c[2],1023)<<20) | (unorm(c[3],3)<<30);
                    memcpy(d,&packed,4);break;
                }
                case Kind::RGBA8: d[0] = quantize(c[0]); d[1] = quantize(c[1]); d[2] = quantize(c[2]); d[3] = quantize(c[3]); break;
                case Kind::RG8: d[0] = quantize(c[0]); d[1] = quantize(c[1]); break;
                case Kind::R8: d[0] = quantize(c[0]); break;
                case Kind::RGBA16F:
                    for (int i = 0; i < 4; ++i) { uint16_t hf = floatToHalf(c[i]); memcpy(d + i * 2, &hf, 2); }
                    break;
                case Kind::RG16F:
                    for (int i = 0; i < 2; ++i) { uint16_t hf = floatToHalf(c[i]); memcpy(d + i * 2, &hf, 2); }
                    break;
                case Kind::R16F: { uint16_t hf = floatToHalf(c[0]); memcpy(d, &hf, 2); break; }
                case Kind::RGBA32F: memcpy(d, c, 16); break;
                case Kind::RG32F: memcpy(d, c, 8); break;
                case Kind::R32F: memcpy(d, c, 4); break;
                default: fatal("upload to internal format", t.glInternal);
                }
            }
        }
    }
    return out;
}

void VulkanTextureBackend::convertForReadback(const Tex& t, const uint8_t* texels, U32 format, U32 type, uint32_t w, uint32_t h,
                                              uint32_t count, void* out)
{
    bool bgr = false;
    const int comps = formatComps(format, bgr);
    const int tb = typeBytes(type);
    if (!comps || (type != glc::UNSIGNED_BYTE && type != glc::FLOAT && type != glc::UNSIGNED_INT))
    {
        fatal("readback format/type", format);
    }
    const uint32_t texel = t.info.texelBytes;
    for (size_t i = 0; i < (size_t) w * h * count; ++i)
    {
        const uint8_t* s = texels + i * texel;
        float c[4] = {0.f, 0.f, 0.f, 1.f};
        switch (t.info.kind)
        {
        case Kind::RGBA16:
            for (int k=0;k<4;++k) { uint16_t v;memcpy(&v,s+k*2,2);c[k]=float(v)/65535.f; }
            break;
        case Kind::RGB10A2:
        {
            uint32_t packed;memcpy(&packed,s,4);
            for (int k=0;k<3;++k) c[k]=float((packed>>(k*10))&1023)/1023.f;
            c[3]=float(packed>>30)/3.f;break;
        }
        case Kind::RGBA8: for (int k = 0; k < 4; ++k) c[k] = s[k] / 255.0f; break;
        case Kind::RG8: c[0] = s[0] / 255.0f; c[1] = s[1] / 255.0f; break;
        case Kind::R8: c[0] = s[0] / 255.0f; break;
        case Kind::RGBA16F: for (int k = 0; k < 4; ++k) { uint16_t hf; memcpy(&hf, s + k * 2, 2); c[k] = halfToFloat(hf); } break;
        case Kind::RG16F: for (int k = 0; k < 2; ++k) { uint16_t hf; memcpy(&hf, s + k * 2, 2); c[k] = halfToFloat(hf); } break;
        case Kind::R16F: { uint16_t hf; memcpy(&hf, s, 2); c[0] = halfToFloat(hf); break; }
        case Kind::RGBA32F: memcpy(c, s, 16); break;
        case Kind::RG32F: memcpy(c, s, 8); break;
        case Kind::R32F: case Kind::D32: memcpy(c, s, 4); break;
        case Kind::B10G11R11:
        {
            uint32_t packed; memcpy(&packed, s, 4);
            auto unpack = [](uint32_t bits, int mantissa) {
                uint32_t exp = bits >> mantissa, fraction = bits & ((1u << mantissa) - 1);
                if (exp == 31) return fraction ? NAN : INFINITY;
                return std::ldexp((float) (exp ? (1u << mantissa) + fraction : fraction),
                                  (exp ? (int) exp : 1) - 15 - mantissa);
            };
            c[0] = unpack(packed & 0x7FF, 6);
            c[1] = unpack((packed >> 11) & 0x7FF, 6);
            c[2] = unpack(packed >> 22, 5);
            break;
        }
        default: fatal("readback of internal format", t.glInternal);
        }
        if (bgr)
        {
            std::swap(c[0], c[2]);
        }
        uint8_t* d = (uint8_t*) out + i * comps * tb;
        for (int k = 0; k < comps; ++k)
        {
            if (type == glc::UNSIGNED_BYTE)
            {
                d[k] = quantize(c[k]);
            }
            else if (type == glc::UNSIGNED_INT)
            {
                uint32_t value = (uint32_t) (std::clamp((double) c[k], 0.0, 1.0) * 4294967295.0 + 0.5);
                memcpy(d + k * 4, &value, 4);
            }
            else
            {
                memcpy(d + k * 4, &c[k], 4);
            }
        }
    }
}

// ----------------------------------------------------------------------------------------------------------------
// names and binding

void VulkanTextureBackend::generate(S32 count, U32* names)
{
    for (S32 i = 0; i < count; ++i)
    {
        names[i] = mNext++;
        mTextures[names[i]];
    }
}

void VulkanTextureBackend::destroy(S32 count, const U32* names)
{
    for (S32 i = 0; names && i < count; ++i)
    {
        auto it = mTextures.find(names[i]);
        if (it == mTextures.end())
        {
            continue;
        }
        destroyImage(it->second);
        mTextures.erase(it);
        for (auto& u : mUnitTex)
        {
            if (u == names[i])
            {
                u = 0;
            }
        }
    }
}

void VulkanTextureBackend::bind(U32 target, U32 texture)
{
    mUnitTex[mActive] = texture;
    if (texture)
    {
        Tex& t = mTextures[texture];       // a name the viewer made up (LLTexUnit::bindManual) still gets an object
        if (!t.glTarget)
        {
            t.glTarget = isCubeFace(target) ? glc::TEXTURE_CUBE_MAP : target;
        }
    }
}

VulkanTextureBackend::Tex* VulkanTextureBackend::bound(U32 target)
{
    (void) target;
    auto it = mTextures.find(mUnitTex[mActive]);
    return it == mTextures.end() ? nullptr : &it->second;
}

void VulkanTextureBackend::setWrap(U32 target, S32 wrapS, S32 wrapT, S32 wrapR)
{
    if (Tex* t = bound(target))
    {
        t->ss.wrapS = wrapS;
        t->ss.wrapT = wrapT;
        if (wrapR >= 0)
        {
            t->ss.wrapR = wrapR;
        }
    }
}

void VulkanTextureBackend::setFilter(U32 target, S32 magFilter, S32 minFilter)
{
    if (Tex* t = bound(target))
    {
        t->ss.mag = magFilter;
        t->ss.min = minFilter;
    }
}

void VulkanTextureBackend::setMagFilter(U32 target, S32 magFilter)
{
    if (Tex* t = bound(target))
    {
        t->ss.mag = magFilter;
    }
}

void VulkanTextureBackend::setMaxAnisotropy(U32 target, F32 maxAnisotropy)
{
    if (Tex* t = bound(target))
    {
        t->ss.anisotropy = maxAnisotropy;
    }
}

void VulkanTextureBackend::setCompare(U32 target, bool enabled, U32 func)
{
    if (Tex* t = bound(target))
    {
        t->ss.compare = enabled;
        if (enabled)
        {
            t->ss.compareFunc = func;
        }
    }
}

void VulkanTextureBackend::setLevelRange(U32 target, S32 baseLevel, S32 maxLevel)
{
    if (Tex* t = bound(target))
    {
        t->baseLevel = baseLevel;
        t->maxLevel = maxLevel;
    }
}

void VulkanTextureBackend::setSwizzle(U32 target, const S32* rgba)
{
    if (Tex* t = bound(target))
    {
        memcpy(t->swizzle, rgba, sizeof(t->swizzle));
    }
}

void VulkanTextureBackend::setLegacyMipmapGeneration(U32 target, bool enabled)
{
    if (Tex* t = bound(target))
    {
        t->autoMips = enabled;
    }
}

// ----------------------------------------------------------------------------------------------------------------
// storage and upload

void VulkanTextureBackend::image2D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, U32 format, U32 type,
                                   const void* pixels)
{
    Tex* tp = bound(target);
    if (!tp)
    {
        return;
    }
    Tex& t = *tp;
    const bool face = isCubeFace(target);
    const U32 object = face ? glc::TEXTURE_CUBE_MAP : target;
    const bool cube = object == glc::TEXTURE_CUBE_MAP;
    const bool rect = object == glc::TEXTURE_RECTANGLE;
    if (object == glc::TEXTURE_3D || object == glc::TEXTURE_CUBE_MAP_ARRAY)
    {
        unsupported("image2D on a 3D or array target");
    }
    t.glTarget = object;

    if (level == 0 || !t.image)
    {
        ensureImage(t, (uint32_t) width << level, (uint32_t) height << level, cube ? 6 : 1, internalFormat, cube, false, rect);
    }
    if (!pixels)
    {
        return;
    }
    const uint32_t lw = std::max(1u, t.width >> level), lh = std::max(1u, t.height >> level);
    if ((uint32_t) width > lw || (uint32_t) height > lh)
    {
        fprintf(stderr, "warning: level %d of %ux%u texture defined as %dx%d, upload skipped\n", level, t.width, t.height, width, height);
        return;
    }
    const uint32_t layer = face ? (uint32_t) (target - glc::TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    upload(t, (uint32_t) level, layer, 1, 0, 0, (uint32_t) width, (uint32_t) height,
           convertForUpload(t, format, type, (uint32_t) width, (uint32_t) height, 1, pixels));
    if (level == 0 && t.autoMips)
    {
        generateMipmaps(target);
    }
}

void VulkanTextureBackend::compressedImage2D(U32 target, S32 level, U32 internalFormat, S32 width, S32 height, S32 imageSize,
                                             const void* data)
{
    Tex* tp = bound(target);
    if (!tp)
    {
        return;
    }
    Tex& t = *tp;
    const bool face = isCubeFace(target);
    const U32 object = face ? glc::TEXTURE_CUBE_MAP : target;
    t.glTarget = object;
    if (level == 0 || !t.image)
    {
        ensureImage(t, (uint32_t) width << level, (uint32_t) height << level, object == glc::TEXTURE_CUBE_MAP ? 6 : 1, internalFormat,
                    object == glc::TEXTURE_CUBE_MAP, false, false);
    }
    if (!data)
    {
        return;
    }
    const uint32_t layer = face ? (uint32_t) (target - glc::TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    upload(t, (uint32_t) level, layer, 1, 0, 0, (uint32_t) width, (uint32_t) height,
           std::vector<uint8_t>((const uint8_t*) data, (const uint8_t*) data + imageSize));
}

void VulkanTextureBackend::subImage2D(U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels)
{
    Tex* tp = bound(target);
    if (!tp || !tp->image)
    {
        return;
    }
    const uint32_t layer = isCubeFace(target) ? (uint32_t) (target - glc::TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    upload(*tp, (uint32_t) level, layer, 1, x, y, (uint32_t) width, (uint32_t) height,
           convertForUpload(*tp, format, type, (uint32_t) width, (uint32_t) height, 1, pixels));
}

void VulkanTextureBackend::image3D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, S32 depth, U32 format, U32 type,
                                   const void* pixels)
{
    Tex* tp = bound(target);
    if (!tp)
    {
        return;
    }
    if (target != glc::TEXTURE_CUBE_MAP_ARRAY)
    {
        unsupported("image3D on a target other than GL_TEXTURE_CUBE_MAP_ARRAY");
    }
    Tex& t = *tp;
    t.glTarget = target;
    if (level == 0 || !t.image)
    {
        ensureImage(t, (uint32_t) width << level, (uint32_t) height << level, (uint32_t) depth, internalFormat, false, true, false);
    }
    if (pixels)
    {
        upload(t, (uint32_t) level, 0, (uint32_t) depth, 0, 0, (uint32_t) width, (uint32_t) height,
               convertForUpload(t, format, type, (uint32_t) width, (uint32_t) height, (uint32_t) depth, pixels));
    }
}

void VulkanTextureBackend::subImage3D(U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format, U32 type,
                                      const void* pixels)
{
    Tex* tp = bound(target);
    if (!tp || !tp->image)
    {
        return;
    }
    upload(*tp, (uint32_t) level, (uint32_t) z, (uint32_t) depth, x, y, (uint32_t) width, (uint32_t) height,
           convertForUpload(*tp, format, type, (uint32_t) width, (uint32_t) height, (uint32_t) depth, pixels));
}

void VulkanTextureBackend::generateMipmaps(U32 target)
{
    Tex* tp = bound(target);
    if (!tp || !tp->image || tp->levels < 2)
    {
        return;
    }
    Tex& t = *tp;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(mCtx->phys, t.info.format, &props);
    if (t.info.compressed || !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
    {
        fprintf(stderr, "warning: format 0x%x cannot generate mipmaps with a blit chain\n", t.glInternal);
        return;
    }
    const VkImageAspectFlags aspect = aspectOf(t.info.format);
    submitNow([&](VkCommandBuffer cmd) {
        transition(t, cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // level 0 is the source; every other level is rewritten, so its old contents are not needed
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, 0, 1, t.layers);
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 1,
                t.levels - 1, t.layers);
        for (uint32_t l = 1; l < t.levels; ++l)
        {
            VkImageBlit blit{};
            blit.srcSubresource = {aspect, l - 1, 0, t.layers};
            blit.srcOffsets[1] = {(int32_t) std::max(1u, t.width >> (l - 1)), (int32_t) std::max(1u, t.height >> (l - 1)), 1};
            blit.dstSubresource = {aspect, l, 0, t.layers};
            blit.dstOffsets[1] = {(int32_t) std::max(1u, t.width >> l), (int32_t) std::max(1u, t.height >> l), 1};
            vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
            barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, l, 1, t.layers);
        }
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, 0, t.levels, t.layers);
    });
}

void VulkanTextureBackend::replaceWithMipLevel(U32 target, S32 mip, S32 internalFormat, S32 width, S32 height, U32 format, U32 type,
                                               U64 bytes)
{
    (void) format;
    (void) type;
    (void) bytes;
    Tex* tp = bound(target);
    if (!tp || !tp->image)
    {
        return;
    }
    Tex& t = *tp;
    // keep the old image, make a new one of the new size, copy the old level `mip` into its level 0
    Tex old = t;
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t.image = VK_NULL_HANDLE;
    t.memory = VK_NULL_HANDLE;
    t.views.clear();
    t.cView = VK_NULL_HANDLE;
    ensureImage(t, (uint32_t) width, (uint32_t) height, old.layers, internalFormat, old.cube, old.cubeArray, false);
    const VkImageAspectFlags aspect = aspectOf(t.info.format);
    submitNow([&](VkCommandBuffer cmd) {
        transition(t, cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(old, cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, t.levels, t.layers);
        VkImageCopy copy{};
        copy.srcSubresource = {aspect, (uint32_t) mip, 0, old.layers};
        copy.dstSubresource = {aspect, 0, 0, t.layers};
        copy.extent = {(uint32_t) width, (uint32_t) height, 1};
        vkCmdCopyImage(cmd, old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, t.levels, t.layers);
    });
    destroyImage(old); // earlier draws and this copy retain the old image/views until the frame fence completes
}

// ----------------------------------------------------------------------------------------------------------------
// queries and readback

S32 VulkanTextureBackend::getLevelParameter(U32 target, S32 level, U32 pname)
{
    Tex* t = bound(target);
    if (!t || !t->image || (uint32_t) level >= t->levels)
    {
        return 0;
    }
    const uint32_t w = std::max(1u, t->width >> level), h = std::max(1u, t->height >> level);
    switch (pname)
    {
    case glc::TEXTURE_WIDTH: return (S32) w;
    case glc::TEXTURE_HEIGHT: return (S32) h;
    case glc::TEXTURE_COMPRESSED: return t->info.compressed ? 1 : 0;
    case glc::TEXTURE_COMPRESSED_IMAGE_SIZE: return t->info.compressed ? (S32) (((w + 3) / 4) * ((h + 3) / 4) * t->info.texelBytes) : 0;
    }
    return 0;
}

void VulkanTextureBackend::readImage(U32 target, S32 level, U32 format, U32 type, void* out)
{
    Tex* tp = bound(target);
    if (!tp || !tp->image || (uint32_t) level >= tp->levels)
    {
        return;
    }
    Tex& t = *tp;
    const uint32_t w = std::max(1u, t.width >> level), h = std::max(1u, t.height >> level);
    const uint32_t layer = isCubeFace(target) ? (uint32_t) (target - glc::TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    const uint32_t count = (target == glc::TEXTURE_CUBE_MAP_ARRAY) ? t.layers : 1;
    std::vector<uint8_t> raw((size_t) w * h * count * t.info.texelBytes);

    VkBuffer staging;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = raw.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(mCtx->device, &bci, nullptr, &staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mCtx->device, staging, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(mCtx->device, &mai, nullptr, &stagingMemory));
    VK_CHECK(vkBindBufferMemory(mCtx->device, staging, stagingMemory, 0));

    const VkImageAspectFlags aspect = aspectOf(t.info.format);
    submitNow([&](VkCommandBuffer cmd) {
        transition(t, cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, 0, t.levels, t.layers);
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, (uint32_t) level, layer, count};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
        barrier(cmd, t.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, 0, t.levels, t.layers);
    }, true);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(mCtx->device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
    memcpy(raw.data(), mapped, raw.size());
    vkUnmapMemory(mCtx->device, stagingMemory);
    vkDestroyBuffer(mCtx->device, staging, nullptr);
    vkFreeMemory(mCtx->device, stagingMemory, nullptr);

    convertForReadback(t, raw.data(), format, type, w, h, count, out);
}

void VulkanTextureBackend::readCompressedImage(U32 target, S32 level, void* out)
{
    Tex* tp = bound(target);
    if (!tp || !tp->image || !tp->info.compressed)
    {
        return;
    }
    Tex& t = *tp;
    const uint32_t w = std::max(1u, t.width >> level), h = std::max(1u, t.height >> level);
    const size_t bytes = (size_t) ((w + 3) / 4) * ((h + 3) / 4) * t.info.texelBytes;
    VkBuffer staging;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(mCtx->device, &bci, nullptr, &staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mCtx->device, staging, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(mCtx->device, &mai, nullptr, &stagingMemory));
    VK_CHECK(vkBindBufferMemory(mCtx->device, staging, stagingMemory, 0));
    submitNow([&](VkCommandBuffer cmd) {
        transition(t, cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0, t.levels, t.layers);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t) level, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
        barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, 0, t.levels, t.layers);
    }, true);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(mCtx->device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
    memcpy(out, mapped, bytes);
    vkUnmapMemory(mCtx->device, stagingMemory);
    vkDestroyBuffer(mCtx->device, staging, nullptr);
    vkFreeMemory(mCtx->device, stagingMemory, nullptr);
}

// ----------------------------------------------------------------------------------------------------------------
// what a draw needs: image view and sampler

VkImageView VulkanTextureBackend::viewOf(Tex& t)
{
    const uint32_t base = (uint32_t) std::min(std::max(t.baseLevel, 0), (S32) t.levels - 1);
    const uint32_t last = (uint32_t) std::min(std::max(t.maxLevel, (S32) base), (S32) t.levels - 1);
    const uint32_t values[] = {base, last, uint32_t(t.swizzle[0]), uint32_t(t.swizzle[1]), uint32_t(t.swizzle[2]), uint32_t(t.swizzle[3])};
    const std::string key(reinterpret_cast<const char*>(values), sizeof(values));
    auto it = t.views.find(key);
    if (it != t.views.end())
    {
        return it->second;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = t.cubeArray ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : t.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.info.format;
    if (aspectOf(t.info.format) == VK_IMAGE_ASPECT_COLOR_BIT)
    {
        vi.components = {swizzleOf(t.swizzle[0]), swizzleOf(t.swizzle[1]), swizzleOf(t.swizzle[2]), swizzleOf(t.swizzle[3])};
    }
    vi.subresourceRange = {aspectOf(t.info.format), base, last - base + 1, 0, t.layers};
    VkImageView view;
    VK_CHECK(vkCreateImageView(mCtx->device, &vi, nullptr, &view));
    t.views[key] = view;
    ++viewsCreated;
    return view;
}

VkSampler VulkanTextureBackend::samplerOf(const Tex& t)
{
    const SamplerState& s = t.ss;
    uint32_t anisotropyBits;
    memcpy(&anisotropyBits, &s.anisotropy, sizeof(anisotropyBits));
    const uint32_t values[] = {uint32_t(s.wrapS), uint32_t(s.wrapT), uint32_t(s.wrapR), uint32_t(s.mag),
        uint32_t(s.min), anisotropyBits, uint32_t(s.compare), uint32_t(s.compareFunc)};
    const std::string key(reinterpret_cast<const char*>(values), sizeof(values));
    auto it = mSamplers.find(key);
    if (it != mSamplers.end())
    {
        return it->second;
    }
    const bool minLinear = s.min == (S32) glc::LINEAR || s.min == (S32) glc::LINEAR_MIPMAP_NEAREST || s.min == (S32) glc::LINEAR_MIPMAP_LINEAR;
    const bool usesMips = (uint32_t) s.min >= glc::NEAREST_MIPMAP_NEAREST;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = s.mag == (S32) glc::NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = minLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = (s.min == (S32) glc::LINEAR_MIPMAP_LINEAR || s.min == (S32) glc::NEAREST_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                                          : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = addressMode(s.wrapS);
    sci.addressModeV = addressMode(s.wrapT);
    sci.addressModeW = addressMode(s.wrapR);
    sci.anisotropyEnable = (mCtx->samplerAnisotropy && s.anisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = std::min(s.anisotropy, mCtx->maxAnisotropy);
    sci.compareEnable = s.compare ? VK_TRUE : VK_FALSE;
    sci.compareOp = (VkCompareOp) (s.compareFunc - glc::NEVER);
    sci.minLod = 0.0f;
    sci.maxLod = usesMips ? VK_LOD_CLAMP_NONE : 0.25f;      // a min filter without mips never leaves level 0
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler;
    VK_CHECK(vkCreateSampler(mCtx->device, &sci, nullptr, &sampler));
    mSamplers[key] = sampler;
    ++samplersCreated;
    return sampler;
}

VulkanTextureBackend::Tex& VulkanTextureBackend::fallback(const std::string& samplerType)
{
    if (samplerType.find("Shadow") != std::string::npos)
    {
        fprintf(stderr, "FATAL: a shadow sampler reads a unit with no depth texture bound (the skeleton has no depth fallback)\n");
        abort();
    }
    const bool arr = samplerType == "samplerCubeArray";
    const bool cube = samplerType == "samplerCube" || arr;
    auto it = mFallbacks.find(samplerType);
    if (it == mFallbacks.end())
    {
        Tex& t = mFallbacks[samplerType];
        ensureImage(t, 1, 1, cube ? 6 : 1, glc::RGBA8, cube && !arr, arr, false);
        const std::vector<uint8_t> white((size_t) t.layers * 4, 255);
        upload(t, 0, 0, t.layers, 0, 0, 1, 1, white);
        return t;
    }
    return it->second;
}

void VulkanTextureBackend::unitDescriptor(uint32_t unit, const std::string& samplerType, VkDescriptorImageInfo& info)
{
    unitDescriptor(unit, samplerKind(samplerType), samplerType, info);
}

void VulkanTextureBackend::unitDescriptor(uint32_t unit, int kind, const std::string& samplerType, VkDescriptorImageInfo& info)
{
    Tex* t = nullptr;
    if (unit < kUnits)
    {
        auto it = mTextures.find(mUnitTex[unit]);
        if (it != mTextures.end() && it->second.image)
        {
            t = &it->second;
        }
    }
    const bool wantCube = kind == 1, wantArray = kind == 2;
    const bool ok = t && ((wantArray && t->cubeArray) || (wantCube && t->cube) || (!wantCube && !wantArray && !t->cube && !t->cubeArray));
    if (t && !ok)
    {
        ++kindMismatches;
    }
    bool sampleOk = ok;
    if (mCtx->recording && ok && !mCtx->framebuffers->prepareSample(mUnitTex[unit]))
    {
        sampleOk = false;
        if (!mCtx->feedbackSubstitutions++)
            fprintf(stderr, "WARNING: a draw samples its own render target; using a fallback texture (counted in VulkanPacing)\n");
    }
    if (!sampleOk)
    {
        t = &fallback(samplerType);
    }
    if (!t->cSampler || !(t->cSs == t->ss))
    {
        t->cSampler = samplerOf(*t);
        t->cSs = t->ss;
    }
    if (!t->cView || t->cImage != t->image || t->cLevels != t->levels || t->cBase != t->baseLevel || t->cMax != t->maxLevel ||
        memcmp(t->cSwizzle, t->swizzle, sizeof(t->swizzle)) != 0)
    {
        t->cView = viewOf(*t);
        t->cImage = t->image;
        t->cLevels = t->levels;
        t->cBase = t->baseLevel;
        t->cMax = t->maxLevel;
        memcpy(t->cSwizzle, t->swizzle, sizeof(t->swizzle));
    }
    info.sampler = t->cSampler;
    info.imageView = t->cView;
    info.imageLayout = t->layout;
}

void VulkanTextureBackend::transition(Tex& t, VkCommandBuffer cmd, VkImageLayout layout)
{
    if (t.layout == layout && (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL)) return;
    barrier(cmd, t.image, aspectOf(t.info.format), t.layout, layout,
            t.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT :
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0, t.levels, t.layers);
    t.layout = layout;
}

void VulkanTextureBackend::copyFromFramebuffer2D(U32 target, S32 level, S32 xo, S32 yo, S32 x, S32 y, S32 w, S32 h)
{
    mCtx->framebuffers->copyToTexture(target, level, xo, yo, 0, x, y, w, h);
}
void VulkanTextureBackend::copyFromFramebuffer3D(U32 target, S32 level, S32 xo, S32 yo, S32 zo, S32 x, S32 y, S32 w, S32 h)
{
    mCtx->framebuffers->copyToTexture(target, level, xo, yo, zo, x, y, w, h);
}
void VulkanTextureBackend::copyImageFromFramebuffer2D(U32 target, S32 level, S32 internal, S32 x, S32 y, S32 w, S32 h)
{
    image2D(target, level, internal, w, h, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
    copyFromFramebuffer2D(target, level, 0, 0, x, y, w, h);
}
