// Proxy backends for the threaded renderer (doc/vulkan/render-thread-design.md).
// The viewer talks to these on its main thread. Calls that return nothing become records for rt::Thread, which runs them on the
// render thread against the real Vulkan backends. Calls that return something are answered from a mirror of the proxied state,
// or wait for the render thread to go idle and then use the real backend directly (it stays idle until the next post).
// Included by llrenderbackend_vulkan.cpp after the Vulkan backend headers.
#pragma once

#include "vulkan/vkrenderthread.h"
#include "llrenderstatebackend.h"
#include "llshaderbackend.h"
#include "lltexturebackend.h"
#include "llframebufferbackend.h"
#include "llvertexbufferbackend.h"

#include <array>
#include <atomic>
#include <string>
#include <unordered_map>

namespace rt
{

// Names are handed out by the proxy so the main thread never waits for the render thread. Both sides count from the same start,
// and the render thread checks that every name it is asked to create matches.
[[noreturn]] inline void nameMismatch(const char* what, unsigned long long expected, unsigned long long got)
{
    fprintf(stderr, "FATAL: threaded renderer: %s name mismatch (proxy %llu, backend %llu)\n", what, expected, got);
    abort();
}

// State written by the render thread and read by the main thread, or shared between records.
struct Shared
{
    bool frameFailed = false;               // render thread only: the frame could not start, so draws are dropped until the next one
    std::atomic<U64> vramBytes{0};
};

// ------------------------------------------------------------------------------------------------------------------------

class StateProxy final : public LLRenderStateBackend
{
public:
    StateProxy(Thread& thread, VulkanRenderStateBackend* real) : T(thread), R(real)
    {
        R->queryDepthState(mDepthTest, mDepthFunc, mDepthMask);
    }
    void setCapability(U32 cap, bool enabled) override
    {
        mCaps[cap] = enabled;
        T.post([r = R, cap, enabled] { r->setCapability(cap, enabled); });
    }
    bool isCapabilityEnabled(U32 cap) const override
    {
        auto it = mCaps.find(cap);
        if (it != mCaps.end()) return it->second;
        T.sync();
        const bool value = R->isCapabilityEnabled(cap);
        mCaps[cap] = value;
        return value;
    }
    void setDepthTest(bool enabled) override
    {
        mDepthTest = enabled;
        T.post([r = R, enabled] { r->setDepthTest(enabled); });
    }
    void setDepthFunc(U32 func) override
    {
        mDepthFunc = func;
        T.post([r = R, func] { r->setDepthFunc(func); });
    }
    void setDepthMask(bool write) override
    {
        mDepthMask = write;
        T.post([r = R, write] { r->setDepthMask(write); });
    }
    void queryDepthState(bool& testEnabled, U32& func, bool& writeEnabled) const override
    {
        testEnabled = mDepthTest;
        func = mDepthFunc;
        writeEnabled = mDepthMask;
    }
    void setBlendFunc(U32 cs, U32 cd, U32 as, U32 ad) override { T.post([r = R, cs, cd, as, ad] { r->setBlendFunc(cs, cd, as, ad); }); }
    void setColorMask(bool rr, bool g, bool b, bool a) override { T.post([r = R, rr, g, b, a] { r->setColorMask(rr, g, b, a); }); }
    void setLineWidth(F32 width) override { T.post([r = R, width] { r->setLineWidth(width); }); }
    void setViewport(S32 x, S32 y, S32 w, S32 h) override { T.post([r = R, x, y, w, h] { r->setViewport(x, y, w, h); }); }
    void setScissor(S32 x, S32 y, S32 w, S32 h) override { T.post([r = R, x, y, w, h] { r->setScissor(x, y, w, h); }); }
    void setCullFace(U32 face) override { T.post([r = R, face] { r->setCullFace(face); }); }
    void setPolygonMode(U32 face, U32 mode) override { T.post([r = R, face, mode] { r->setPolygonMode(face, mode); }); }
    void setPolygonOffset(F32 factor, F32 units) override { T.post([r = R, factor, units] { r->setPolygonOffset(factor, units); }); }
    void setPointSize(F32 size) override { T.post([r = R, size] { r->setPointSize(size); }); }
    void setStencilFunc(U32 func, S32 ref, U32 mask) override { T.post([r = R, func, ref, mask] { r->setStencilFunc(func, ref, mask); }); }
    void setStencilOp(U32 sf, U32 df, U32 dp) override { T.post([r = R, sf, df, dp] { r->setStencilOp(sf, df, dp); }); }
    void setStencilMask(U32 mask) override { T.post([r = R, mask] { r->setStencilMask(mask); }); }

private:
    Thread& T;
    VulkanRenderStateBackend* R;
    mutable std::unordered_map<U32, bool> mCaps;
    bool mDepthTest = false, mDepthMask = false;
    U32 mDepthFunc = 0;
};

// ------------------------------------------------------------------------------------------------------------------------

class FramebufferProxy final : public LLFramebufferBackend
{
public:
    FramebufferProxy(Thread& thread, VulkanFramebufferBackend* real) : T(thread), R(real) {}
    U32 createFramebuffer() override
    {
        const U32 id = mNext++;
        T.post([r = R, id] {
            const U32 got = r->createFramebuffer();
            if (got != id) nameMismatch("framebuffer", id, got);
        });
        return id;
    }
    void destroyFramebuffer(U32 framebuffer) override
    {
        if (framebuffer && framebuffer == mBound) mBound = 0;
        T.post([r = R, framebuffer] { r->destroyFramebuffer(framebuffer); });
    }
    void bind(U32 framebuffer) override
    {
        mBound = framebuffer;
        T.post([r = R, framebuffer] { r->bind(framebuffer); });
    }
    void bindRead(U32 framebuffer) override { T.post([r = R, framebuffer] { r->bindRead(framebuffer); }); }
    U32 getBound() const override { return mBound; }
    void attach(U32 framebuffer, U32 attachment, U32 target, U32 texture, S32 level) override
    {
        T.post([r = R, framebuffer, attachment, target, texture, level] { r->attach(framebuffer, attachment, target, texture, level); });
    }
    U32 checkStatus() override
    {
        T.sync();
        return R->checkStatus();
    }
    void setDrawBuffers(U32 count) override { T.post([r = R, count] { r->setDrawBuffers(count); }); }
    void setDrawBufferNone() override { T.post([r = R] { r->setDrawBufferNone(); }); }
    void setDrawBufferBack() override { T.post([r = R] { r->setDrawBufferBack(); }); }
    void setReadBuffer(U32 buffer) override { T.post([r = R, buffer] { r->setReadBuffer(buffer); }); }
    void setClearColor(F32 rr, F32 g, F32 b, F32 a) override { T.post([r = R, rr, g, b, a] { r->setClearColor(rr, g, b, a); }); }
    void setClearDepth(F64 depth) override { T.post([r = R, depth] { r->setClearDepth(depth); }); }
    void clear(U32 mask) override { T.post([r = R, mask] { r->clear(mask); }); }
    void readPixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* data) override
    {
        T.sync();
        R->readPixels(x, y, width, height, format, type, data);
    }

private:
    Thread& T;
    VulkanFramebufferBackend* R;
    U32 mNext = 1, mBound = 0;
};

// ------------------------------------------------------------------------------------------------------------------------

class TextureProxy final : public LLTextureBackend
{
public:
    TextureProxy(Thread& thread, VulkanTextureBackend* real) : T(thread), R(real) {}
    void initContextState() override { T.post([r = R] { r->initContextState(); }); }
    void activateUnit(U32 unit) override { T.post([r = R, unit] { r->activateUnit(unit); }); }
    void bind(U32 target, U32 texture) override { T.post([r = R, target, texture] { r->bind(target, texture); }); }
    void setWrap(U32 target, S32 s, S32 t, S32 rr) override { T.post([r = R, target, s, t, rr] { r->setWrap(target, s, t, rr); }); }
    void setFilter(U32 target, S32 mag, S32 min) override { T.post([r = R, target, mag, min] { r->setFilter(target, mag, min); }); }
    void setMaxAnisotropy(U32 target, F32 a) override { T.post([r = R, target, a] { r->setMaxAnisotropy(target, a); }); }
    void setLevelRange(U32 target, S32 base, S32 max) override { T.post([r = R, target, base, max] { r->setLevelRange(target, base, max); }); }
    void setMagFilter(U32 target, S32 mag) override { T.post([r = R, target, mag] { r->setMagFilter(target, mag); }); }
    void setCompare(U32 target, bool enabled, U32 func) override { T.post([r = R, target, enabled, func] { r->setCompare(target, enabled, func); }); }
    void setSwizzle(U32 target, const S32* rgba) override
    {
        T.postData(rgba, 4 * sizeof(S32), [r = R, target](const void* p) { r->setSwizzle(target, static_cast<const S32*>(p)); });
    }
    void setLegacyMipmapGeneration(U32 target, bool enabled) override { T.post([r = R, target, enabled] { r->setLegacyMipmapGeneration(target, enabled); }); }

    void generate(S32 count, U32* names) override
    {
        for (S32 i = 0; i < count; ++i)
        {
            const U32 id = mNext++;
            names[i] = id;
            T.post([r = R, id] {
                U32 got = 0;
                r->generate(1, &got);
                if (got != id) nameMismatch("texture", id, got);
            });
        }
    }
    void destroy(S32 count, const U32* names) override
    {
        if (count <= 0 || !names) return;
        T.postData(names, size_t(count) * sizeof(U32), [r = R, count](const void* p) { r->destroy(count, static_cast<const U32*>(p)); });
    }

    void setUnpackRowLength(S32 pixels) override
    {
        mRowLength = pixels;
        T.post([r = R, pixels] { r->setUnpackRowLength(pixels); });
    }
    void setUnpackSwapBytes(bool swap) override { T.post([r = R, swap] { r->setUnpackSwapBytes(swap); }); }

    void image2D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, U32 format, U32 type, const void* pixels) override
    {
        const size_t n = pixels ? VulkanTextureBackend::sourceBytes(format, type, width, height, 1, mRowLength) : 0;
        if (pixels && !n)
        {
            T.sync();   // a format the backend rejects: let it say so on this thread
            R->image2D(target, level, internalFormat, width, height, format, type, pixels);
            return;
        }
        if (!pixels)
        {
            T.post([r = R, target, level, internalFormat, width, height, format, type] {
                r->image2D(target, level, internalFormat, width, height, format, type, nullptr);
            });
            return;
        }
        T.postData(pixels, n, [r = R, target, level, internalFormat, width, height, format, type](const void* p) {
            r->image2D(target, level, internalFormat, width, height, format, type, p);
        });
    }
    void compressedImage2D(U32 target, S32 level, U32 internalFormat, S32 width, S32 height, S32 imageSize, const void* data) override
    {
        if (!data || imageSize <= 0)
        {
            T.post([r = R, target, level, internalFormat, width, height, imageSize] {
                r->compressedImage2D(target, level, internalFormat, width, height, imageSize, nullptr);
            });
            return;
        }
        T.postData(data, size_t(imageSize), [r = R, target, level, internalFormat, width, height, imageSize](const void* p) {
            r->compressedImage2D(target, level, internalFormat, width, height, imageSize, p);
        });
    }
    void subImage2D(U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels) override
    {
        const size_t n = pixels ? VulkanTextureBackend::sourceBytes(format, type, width, height, 1, mRowLength) : 0;
        if (!n)
        {
            T.sync();
            R->subImage2D(target, level, x, y, width, height, format, type, pixels);
            return;
        }
        T.postData(pixels, n, [r = R, target, level, x, y, width, height, format, type](const void* p) {
            r->subImage2D(target, level, x, y, width, height, format, type, p);
        });
    }
    void image3D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, S32 depth, U32 format, U32 type, const void* pixels) override
    {
        const size_t n = pixels ? VulkanTextureBackend::sourceBytes(format, type, width, height, depth, mRowLength) : 0;
        if (pixels && !n)
        {
            T.sync();
            R->image3D(target, level, internalFormat, width, height, depth, format, type, pixels);
            return;
        }
        if (!pixels)
        {
            T.post([r = R, target, level, internalFormat, width, height, depth, format, type] {
                r->image3D(target, level, internalFormat, width, height, depth, format, type, nullptr);
            });
            return;
        }
        T.postData(pixels, n, [r = R, target, level, internalFormat, width, height, depth, format, type](const void* p) {
            r->image3D(target, level, internalFormat, width, height, depth, format, type, p);
        });
    }
    void subImage3D(U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format, U32 type,
                    const void* pixels) override
    {
        const size_t n = pixels ? VulkanTextureBackend::sourceBytes(format, type, width, height, depth, mRowLength) : 0;
        if (!n)
        {
            T.sync();
            R->subImage3D(target, level, x, y, z, width, height, depth, format, type, pixels);
            return;
        }
        T.postData(pixels, n, [r = R, target, level, x, y, z, width, height, depth, format, type](const void* p) {
            r->subImage3D(target, level, x, y, z, width, height, depth, format, type, p);
        });
    }
    void generateMipmaps(U32 target) override { T.post([r = R, target] { r->generateMipmaps(target); }); }
    void copyFromFramebuffer2D(U32 target, S32 level, S32 xo, S32 yo, S32 x, S32 y, S32 w, S32 h) override
    {
        T.post([r = R, target, level, xo, yo, x, y, w, h] { r->copyFromFramebuffer2D(target, level, xo, yo, x, y, w, h); });
    }
    void copyFromFramebuffer3D(U32 target, S32 level, S32 xo, S32 yo, S32 zo, S32 x, S32 y, S32 w, S32 h) override
    {
        T.post([r = R, target, level, xo, yo, zo, x, y, w, h] { r->copyFromFramebuffer3D(target, level, xo, yo, zo, x, y, w, h); });
    }
    void copyImageFromFramebuffer2D(U32 target, S32 level, S32 internalFormat, S32 x, S32 y, S32 w, S32 h) override
    {
        T.post([r = R, target, level, internalFormat, x, y, w, h] { r->copyImageFromFramebuffer2D(target, level, internalFormat, x, y, w, h); });
    }
    void replaceWithMipLevel(U32 target, S32 mip, S32 internalFormat, S32 width, S32 height, U32 format, U32 type, U64 bytes) override
    {
        T.post([r = R, target, mip, internalFormat, width, height, format, type, bytes] {
            r->replaceWithMipLevel(target, mip, internalFormat, width, height, format, type, bytes);
        });
    }

    S32 getLevelParameter(U32 target, S32 level, U32 pname) override
    {
        T.sync();
        return R->getLevelParameter(target, level, pname);
    }
    void readImage(U32 target, S32 level, U32 format, U32 type, void* out) override
    {
        T.sync();
        R->readImage(target, level, format, type, out);
    }
    void readCompressedImage(U32 target, S32 level, void* out) override
    {
        T.sync();
        R->readCompressedImage(target, level, out);
    }
    bool isResident(U32 texture) override
    {
        T.sync();
        return R->isResident(texture);
    }
    void shutdown() override
    {
        T.sync();
        R->shutdown();
    }

private:
    Thread& T;
    VulkanTextureBackend* R;
    U32 mNext = 1;
    S32 mRowLength = 0;
};

// ------------------------------------------------------------------------------------------------------------------------

class ShaderProxy final : public LLShaderBackend
{
public:
    ShaderProxy(Thread& thread, VulkanShaderBackend* real) : T(thread), R(real)
    {
        mBinary = R->supportsProgramBinary();
        mSalt = R->programBinaryCacheSalt();
    }
    // Setup and reflection are rare: wait for the render thread, then call the real backend here.
    U32 createObject(U32 stage) override { T.sync(); return R->createObject(stage); }
    bool compileObject(U32 object, U32 count, const char* const* text, std::string& log) override
    {
        T.sync();
        return R->compileObject(object, count, text, log);
    }
    void deleteObject(U32 object) override { T.post([r = R, object] { r->deleteObject(object); }); }
    U32 createProgram() override { T.sync(); return R->createProgram(); }
    void attachObject(U32 program, U32 object) override { T.post([r = R, program, object] { r->attachObject(program, object); }); }
    void deleteProgramAndObjects(U32 program) override
    {
        mLocations.erase(program);
        T.post([r = R, program] { r->deleteProgramAndObjects(program); });
    }
    void bindAttribLocation(U32 program, U32 index, const char* name) override
    {
        T.sync();
        R->bindAttribLocation(program, index, name);
    }
    bool link(U32 program) override
    {
        mLocations.erase(program);
        T.sync();
        return R->link(program);
    }
    bool validate(U32 program) override { T.sync(); return R->validate(program); }
    std::string getObjectLog(U32 object) override { T.sync(); return R->getObjectLog(object); }
    std::string getProgramLog(U32 program) override { T.sync(); return R->getProgramLog(program); }
    void use(U32 program) override { T.post([r = R, program] { r->use(program); }); }

    S32 getAttribLocation(U32 program, const char* name) override
    {
        auto& cache = mLocations[program];
        const std::string key = std::string("a:") + name;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        T.sync();
        return cache[key] = R->getAttribLocation(program, name);
    }
    S32 getUniformLocation(U32 program, const char* name) override
    {
        auto& cache = mLocations[program];
        const std::string key = std::string("u:") + name;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        T.sync();
        return cache[key] = R->getUniformLocation(program, name);
    }
    S32 getActiveUniformCount(U32 program) override { T.sync(); return R->getActiveUniformCount(program); }
    void getActiveUniform(U32 program, S32 index, std::string& name, U32& type, S32& size) override
    {
        T.sync();
        R->getActiveUniform(program, index, name, type, size);
    }
    void bindUniformBlock(U32 program, const char* name, U32 binding) override
    {
        T.sync();
        R->bindUniformBlock(program, name, binding);
    }

    bool supportsProgramBinary() const override { return mBinary; }
    std::string programBinaryCacheSalt() const override { return mSalt; }
    void setProgramBinaryRetrievable(U32 program) override { T.post([r = R, program] { r->setProgramBinaryRetrievable(program); }); }
    bool loadProgramBinary(U32 program, U32 format, const void* data, S32 length) override
    {
        mLocations.erase(program);
        T.sync();
        return R->loadProgramBinary(program, format, data, length);
    }
    S32 getProgramBinaryLength(U32 program) override { T.sync(); return R->getProgramBinaryLength(program); }
    bool getProgramBinary(U32 program, std::vector<U8>& binary, U32& format) override
    {
        T.sync();
        return R->getProgramBinary(program, binary, format);
    }

    // Uniforms are the hot path: one record each, values copied.
    void uniform1i(U32 p, S32 l, S32 v) override { T.post([r = R, p, l, v] { r->uniform1i(p, l, v); }); }
    void uniform2i(U32 p, S32 l, S32 x, S32 y) override { T.post([r = R, p, l, x, y] { r->uniform2i(p, l, x, y); }); }
    void uniform1f(U32 p, S32 l, F32 v) override { T.post([r = R, p, l, v] { r->uniform1f(p, l, v); }); }
    void uniform2f(U32 p, S32 l, F32 x, F32 y) override { T.post([r = R, p, l, x, y] { r->uniform2f(p, l, x, y); }); }
    void uniform3f(U32 p, S32 l, F32 x, F32 y, F32 z) override { T.post([r = R, p, l, x, y, z] { r->uniform3f(p, l, x, y, z); }); }
    void uniform4f(U32 p, S32 l, F32 x, F32 y, F32 z, F32 w) override { T.post([r = R, p, l, x, y, z, w] { r->uniform4f(p, l, x, y, z, w); }); }
    void uniform1iv(U32 p, S32 l, S32 c, const S32* v) override { vec(1, p, l, c, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, const void* d) { r->uniform1iv(pp, ll, cc, static_cast<const S32*>(d)); }); }
    void uniform4iv(U32 p, S32 l, S32 c, const S32* v) override { vec(4, p, l, c, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, const void* d) { r->uniform4iv(pp, ll, cc, static_cast<const S32*>(d)); }); }
    void uniform1fv(U32 p, S32 l, S32 c, const F32* v) override { vec(1, p, l, c, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, const void* d) { r->uniform1fv(pp, ll, cc, static_cast<const F32*>(d)); }); }
    void uniform2fv(U32 p, S32 l, S32 c, const F32* v) override { vec(2, p, l, c, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, const void* d) { r->uniform2fv(pp, ll, cc, static_cast<const F32*>(d)); }); }
    void uniform3fv(U32 p, S32 l, S32 c, const F32* v) override { vec(3, p, l, c, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, const void* d) { r->uniform3fv(pp, ll, cc, static_cast<const F32*>(d)); }); }
    void uniform4fv(U32 p, S32 l, S32 c, const F32* v) override { vec(4, p, l, c, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, const void* d) { r->uniform4fv(pp, ll, cc, static_cast<const F32*>(d)); }); }
    void uniform4uiv(U32 p, S32 l, S32 c, const U32* v) override { vec(4, p, l, c, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, const void* d) { r->uniform4uiv(pp, ll, cc, static_cast<const U32*>(d)); }); }
    void uniformMatrix2fv(U32 p, S32 l, S32 c, bool tr, const F32* v) override { mat(4, p, l, c, tr, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, bool t, const void* d) { r->uniformMatrix2fv(pp, ll, cc, t, static_cast<const F32*>(d)); }); }
    void uniformMatrix3fv(U32 p, S32 l, S32 c, bool tr, const F32* v) override { mat(9, p, l, c, tr, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, bool t, const void* d) { r->uniformMatrix3fv(pp, ll, cc, t, static_cast<const F32*>(d)); }); }
    void uniformMatrix3x4fv(U32 p, S32 l, S32 c, bool tr, const F32* v) override { mat(12, p, l, c, tr, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, bool t, const void* d) { r->uniformMatrix3x4fv(pp, ll, cc, t, static_cast<const F32*>(d)); }); }
    void uniformMatrix4fv(U32 p, S32 l, S32 c, bool tr, const F32* v) override { mat(16, p, l, c, tr, v, [](VulkanShaderBackend* r, U32 pp, S32 ll, S32 cc, bool t, const void* d) { r->uniformMatrix4fv(pp, ll, cc, t, static_cast<const F32*>(d)); }); }

private:
    using VecFn = void (*)(VulkanShaderBackend*, U32, S32, S32, const void*);
    using MatFn = void (*)(VulkanShaderBackend*, U32, S32, S32, bool, const void*);
    void vec(size_t comps, U32 p, S32 l, S32 c, const void* v, VecFn fn)
    {
        if (c <= 0 || !v) return;
        T.postData(v, size_t(c) * comps * 4, [r = R, p, l, c, fn](const void* d) { fn(r, p, l, c, d); });
    }
    void mat(size_t comps, U32 p, S32 l, S32 c, bool transpose, const void* v, MatFn fn)
    {
        if (c <= 0 || !v) return;
        T.postData(v, size_t(c) * comps * 4, [r = R, p, l, c, transpose, fn](const void* d) { fn(r, p, l, c, transpose, d); });
    }

    Thread& T;
    VulkanShaderBackend* R;
    bool mBinary = false;
    std::string mSalt;
    std::unordered_map<U32, std::unordered_map<std::string, S32>> mLocations;     // locations are fixed once a program is linked
};

// ------------------------------------------------------------------------------------------------------------------------

class VertexProxy final : public LLVertexBufferBackend
{
public:
    VertexProxy(Thread& thread, VulkanVertexBufferBackend* real, Shared* shared) : T(thread), R(real), S(shared) {}
    ~VertexProxy() override
    {
        T.sync();
        delete R;
    }
    bool usesDeferredUpload() const override { return false; }
    void allocate(BufferKind kind, U32 size, BufferHandle& handle, U8*& data) override
    {
        handle.id = mNextId++;
        data = static_cast<U8*>(_aligned_malloc(size, 16));
        memset(data, 0, size);
        T.post([r = R, s = S, kind, size, id = handle.id] {
            r->allocateWithId(kind, size, id);
            s->vramBytes.store(r->getVramBytesUsed());
        });
    }
    void free(BufferKind kind, U32 size, BufferHandle handle, U8* data) override
    {
        if (mBoundVB == handle) mBoundVB = {};
        if (mBoundIB == handle) mBoundIB = {};
        T.post([r = R, s = S, kind, size, handle, data] {
            r->free(kind, size, handle, data);
            s->vramBytes.store(r->getVramBytesUsed());
        });
    }
    U64 getVramBytesUsed() override { return S->vramBytes.load(); }
    void createStatic(BufferKind kind, BufferHandle& handle, U32 size, const void* data) override
    {
        T.sync();
        R->createStatic(kind, handle, size, data);
    }
    void bind(BufferKind kind, BufferHandle handle) override
    {
        (kind == BufferKind::Vertex ? mBoundVB : mBoundIB) = handle;
        T.post([r = R, kind, handle] { r->bind(kind, handle); });
    }
    bool isBound(BufferKind kind, BufferHandle handle) const override { return (kind == BufferKind::Vertex ? mBoundVB : mBoundIB) == handle; }
    void unbind() override
    {
        mBoundVB = {};
        mBoundIB = {};
        T.post([r = R] { r->unbind(); });
    }
    void invalidateBufferBinding() override
    {
        mBoundVB = {};
        T.post([r = R] { r->invalidateBufferBinding(); });
    }
    void upload(BufferKind kind, BufferHandle handle, U32 start, U32 end, const void* data) override
    {
        T.postData(data, size_t(end) - start + 1, [r = R, kind, handle, start, end](const void* p) { r->upload(kind, handle, start, end, p); });
    }
    void setBuffers(BufferHandle vb, BufferHandle ib, U32 attribMask, const U32* offsets) override
    {
        mBoundVB = vb;
        mBoundIB = ib;
        T.postData(offsets, kOffsets * sizeof(U32), [r = R, vb, ib, attribMask](const void* p) {
            r->setBuffers(vb, ib, attribMask, static_cast<const U32*>(p));
        });
    }
    void setEnabledAttributes(U32 mask) override { T.post([r = R, mask] { r->setEnabledAttributes(mask); }); }
    void drawIndexed(U32 mode, U32 start, U32 end, U32 count, IndexType type, U32 indexByteOffset) override
    {
        T.post([r = R, s = S, mode, start, end, count, type, indexByteOffset] {
            if (!s->frameFailed) r->drawIndexed(mode, start, end, count, type, indexByteOffset);
        });
    }
    void drawArrays(U32 mode, U32 first, U32 count) override
    {
        T.post([r = R, s = S, mode, first, count] {
            if (!s->frameFailed) r->drawArrays(mode, first, count);
        });
    }
    void initContextState() override { T.post([r = R] { r->initContextState(); }); }
    void drawArraysNoInput(U32 mode, U32 first, U32 count) override
    {
        T.post([r = R, s = S, mode, first, count] {
            if (!s->frameFailed) r->drawArraysNoInput(mode, first, count);
        });
    }
    void setLabel(BufferHandle, const char*) override {}

private:
    static constexpr size_t kOffsets = 14;     // LLVertexBuffer::TYPE_MAX, the size VulkanVertexBufferBackend copies
    Thread& T;
    VulkanVertexBufferBackend* R;
    Shared* S;
    U64 mNextId = 1;
    BufferHandle mBoundVB, mBoundIB;
};

} // namespace rt
