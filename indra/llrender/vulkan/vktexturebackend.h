// Vulkan implementation of LLTextureBackend (lltexturebackend.h) for the skeleton. See doc/vulkan/texture-layer-design.md.
//
// The GL model is kept: units, a texture bound to each unit, sampler state that belongs to the texture, and edit calls that act
// on the texture bound to the active unit. Underneath:
//   - a texture is a VkImage with a full mip chain, created when level 0 is defined (the chain is allocated up front, which is
//     how GL's "define the levels you want" maps to Vulkan); uploads go through a staging buffer; mip generation is a blit chain
//   - the GL sampler state (wrap, filters, anisotropy, compare) becomes a VkSampler from a cache, and base/max level plus the
//     swizzle become a VkImageView from a cache, both resolved when a draw needs the texture (VkProgram::prepareDraw)
//   - source pixels are converted to the Vulkan format on the CPU (RGB to RGBA, float to half, ...); driver-side compression
//     requests (GL_COMPRESSED_RGBA) are stored uncompressed, pre-compressed S3TC data is uploaded as BC1 / BC2 / BC3
// Skeleton limits: edits are blocking one-time submits; edits during a frame submit the pending recording first.
// Framebuffer copies record into the active frame. No true 3D textures or multisampling; cube arrays are 2D image arrays.
#pragma once

#include "lltexturebackend.h"
#include "vkcontext.h"

#include <array>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class VulkanTextureBackend final : public LLTextureBackend
{
public:
    explicit VulkanTextureBackend(VkContext* ctx);
    ~VulkanTextureBackend() override;

    // What a draw needs for the sampler that reads texture unit 'unit'. 'samplerType' is the GLSL type ("sampler2D", "samplerCube",
    // "samplerCubeArray", ...). An unbound unit, or one holding a texture of the wrong kind, gets a white fallback texture.
    void unitDescriptor(uint32_t unit, const std::string& samplerType, VkDescriptorImageInfo& info);
    // Same, with the sampler kind already decoded by the caller (0 = 2D, 1 = cube, 2 = cube array): no string work per draw.
    static int samplerKind(const std::string& samplerType) { return samplerType == "samplerCubeArray" ? 2 : samplerType == "samplerCube" ? 1 : 0; }
    void unitDescriptor(uint32_t unit, int kind, const std::string& samplerType, VkDescriptorImageInfo& info);

    // statistics for the tests
    uint32_t imagesCreated = 0;
    uint32_t samplersCreated = 0;
    uint32_t viewsCreated = 0;
    uint32_t kindMismatches = 0;    // a sampler read a unit holding the wrong kind of texture

    void initContextState() override {}
    void activateUnit(U32 unit) override { mActive = unit < kUnits ? unit : 0; }
    void bind(U32 target, U32 texture) override;
    void setWrap(U32 target, S32 wrapS, S32 wrapT, S32 wrapR) override;
    void setFilter(U32 target, S32 magFilter, S32 minFilter) override;
    void setMaxAnisotropy(U32 target, F32 maxAnisotropy) override;
    void setMagFilter(U32 target, S32 magFilter) override;
    void setCompare(U32 target, bool enabled, U32 func) override;
    void setLevelRange(U32 target, S32 baseLevel, S32 maxLevel) override;
    void setSwizzle(U32 target, const S32* rgba) override;
    void setLegacyMipmapGeneration(U32 target, bool enabled) override;
    void generate(S32 count, U32* names) override;
    void destroy(S32 count, const U32* names) override;
    void setUnpackRowLength(S32 pixels) override { mRowLength = pixels; }
    void setUnpackSwapBytes(bool swap) override { mSwapBytes = swap; }
    void image2D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, U32 format, U32 type, const void* pixels) override;
    void compressedImage2D(U32 target, S32 level, U32 internalFormat, S32 width, S32 height, S32 imageSize, const void* data) override;
    void subImage2D(U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels) override;
    void image3D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, S32 depth, U32 format, U32 type,
                 const void* pixels) override;
    void subImage3D(U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format, U32 type,
                    const void* pixels) override;
    void generateMipmaps(U32 target) override;
    void copyFromFramebuffer2D(U32 target, S32 level, S32 xo, S32 yo, S32 x, S32 y, S32 w, S32 h) override;
    void copyFromFramebuffer3D(U32 target, S32 level, S32 xo, S32 yo, S32 zo, S32 x, S32 y, S32 w, S32 h) override;
    void copyImageFromFramebuffer2D(U32 target, S32 level, S32 internal, S32 x, S32 y, S32 w, S32 h) override;
    void replaceWithMipLevel(U32 target, S32 mip, S32 internalFormat, S32 width, S32 height, U32 format, U32 type, U64 bytes) override;
    S32 getLevelParameter(U32 target, S32 level, U32 pname) override;
    void readImage(U32 target, S32 level, U32 format, U32 type, void* out) override;
    void readCompressedImage(U32 target, S32 level, void* out) override;
    bool isResident(U32) override { return true; }
    void shutdown() override {}

private:
    friend class VulkanFramebufferBackend;
    static constexpr U32 kUnits = VkContext::kTextureUnits;

    struct SamplerState
    {
        S32 wrapS = 0x2901, wrapT = 0x2901, wrapR = 0x2901;     // GL_REPEAT
        S32 mag = 0x2601, min = 0x2702;                         // GL_LINEAR, GL_NEAREST_MIPMAP_LINEAR
        F32 anisotropy = 1.f;
        bool compare = false;
        U32 compareFunc = 0x0203;                               // GL_LEQUAL
        bool operator==(const SamplerState& o) const
        {
            return wrapS == o.wrapS && wrapT == o.wrapT && wrapR == o.wrapR && mag == o.mag && min == o.min && anisotropy == o.anisotropy &&
                   compare == o.compare && compareFunc == o.compareFunc;
        }
    };

    // how a GL internal format is stored
    enum class Kind { RGBA16, RGB10A2, RGBA8, RG8, R8, RGBA16F, RG16F, R16F, RGBA32F, RG32F, R32F, B10G11R11, D32, BC };
    struct FormatInfo
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        Kind kind = Kind::RGBA8;
        uint32_t texelBytes = 4;        // bytes per texel, or per 4x4 block for Kind::BC
        bool compressed = false;
    };

    struct Tex
    {
        U32 glTarget = 0;               // object target: GL_TEXTURE_2D, _CUBE_MAP, _CUBE_MAP_ARRAY ...
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool attachmentCapable = false;
        FormatInfo info;
        U32 glInternal = 0;
        uint32_t width = 0, height = 0, layers = 1, levels = 1;
        bool cube = false, cubeArray = false;
        SamplerState ss;
        S32 swizzle[4] = {0x1903, 0x1904, 0x1905, 0x1906};      // GL_RED, GREEN, BLUE, ALPHA
        S32 baseLevel = 0, maxLevel = 1000;
        bool autoMips = false;
        std::map<std::string, VkImageView> views;
        // Last handles handed to a draw, with what they were made from, so a repeat draw skips the string-keyed lookups.
        // cView must be reset wherever 'views' is cleared or moved out.
        SamplerState cSs;
        VkSampler cSampler = VK_NULL_HANDLE;
        VkImageView cView = VK_NULL_HANDLE;
        VkImage cImage = VK_NULL_HANDLE;
        uint32_t cLevels = 0;
        S32 cBase = 0, cMax = 0, cSwizzle[4] = {};
    };

    [[noreturn]] void unsupported(const char* what);
    Tex* bound(U32 target);                                     // texture bound to the active unit (target is only checked loosely)
    FormatInfo formatOf(U32 glInternal);
    void ensureImage(Tex& t, uint32_t w, uint32_t h, uint32_t layers, U32 glInternal, bool cube, bool cubeArray, bool rect);
    void destroyImage(Tex& t);
    void transition(Tex& t, VkCommandBuffer cmd, VkImageLayout layout);
    void submitNow(const std::function<void(VkCommandBuffer)>& record, bool waitForReadback = false);
    void upload(Tex& t, uint32_t level, uint32_t layer, uint32_t layerCount, int32_t x, int32_t y, uint32_t w, uint32_t h,
                const std::vector<uint8_t>& texels);
    std::vector<uint8_t> convertForUpload(const Tex& t, U32 format, U32 type, uint32_t w, uint32_t h, uint32_t count, const void* src);
    void convertForReadback(const Tex& t, const uint8_t* texels, U32 format, U32 type, uint32_t w, uint32_t h, uint32_t count, void* out);
    void image2DFace(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, S32 layers, U32 format, U32 type,
                     const void* pixels, bool cubeArray3D);
    VkImageView viewOf(Tex& t);
    VkSampler samplerOf(const Tex& t);
    Tex& fallback(const std::string& samplerType);

    VkContext* mCtx;
    std::unordered_map<U32, Tex> mTextures;       // node based: references stay valid across inserts
    U32 mNext = 1;
    std::array<U32, kUnits> mUnitTex{};
    U32 mActive = 0;
    S32 mRowLength = 0;
    bool mSwapBytes = false;
    std::map<std::string, VkSampler> mSamplers;
    std::map<std::string, Tex> mFallbacks;
};
