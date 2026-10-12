// Vulkan implementations of the viewer's backend interfaces:
//   LLRenderStateBackend      (llrenderstatebackend.h)   fixed pipeline state, recorded and resolved into pipelines
//   LLVertexBufferBackend     (llvertexbufferbackend.h)  buffers, vertex input and draws
#pragma once

#include "vkcontext.h"
#include "llrenderstatebackend.h"
#include "llvertexbufferbackend.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>

// Everything the viewer has told the state backend so far. GL values in, Vulkan state out.
struct RecordedState
{
    bool blend = false;
    bool cull = false;
    bool depthTest = false;
    bool depthWrite = true;
    bool scissorTest = false;
    bool stencilTest = false;
    bool offsetFill = false;
    bool offsetLine = false;

    U32 depthFunc = 0x0201; // GL_LESS
    U8 blendFactors[4] = {7, 9, 7, 9}; // LLRender::eBlendFactor: src, dst, alpha src, alpha dst (BT_ALPHA)
    bool colorMask[4] = {true, true, true, true};
    U32 cullFace = 0x0405; // GL_BACK
    U32 polygonMode = 0x1B02; // GL_FILL
    F32 offsetFactor = 0, offsetUnits = 0, lineWidth = 1, pointSize = 1;
    S32 viewport[4] = {0, 0, 0, 0};
    S32 scissor[4] = {0, 0, 0, 0};
    U32 stencilFunc = 0, stencilOpFail = 0, stencilOpDepthFail = 0, stencilOpPass = 0, stencilMask = 0xFFFFFFFF;
    S32 stencilRef = 0;
};

class VulkanRenderStateBackend final : public LLRenderStateBackend
{
public:
    RecordedState s;
    std::set<U32> ignoredCaps; // capabilities the skeleton does not translate (reported at exit)
    uint32_t calls = 0;        // how many state calls the viewer-style code made

    void setCapability(U32 cap, bool enabled) override;
    bool isCapabilityEnabled(U32 cap) const override;
    void setDepthTest(bool enabled) override { ++calls; s.depthTest = enabled; }
    void setDepthFunc(U32 func) override { ++calls; s.depthFunc = func; }
    void setDepthMask(bool writeEnabled) override { ++calls; s.depthWrite = writeEnabled; }
    void queryDepthState(bool& testEnabled, U32& func, bool& writeEnabled) const override
    {
        testEnabled = s.depthTest;
        func = s.depthFunc;
        writeEnabled = s.depthWrite;
    }
    void setBlendFunc(U32 colorSrc, U32 colorDst, U32 alphaSrc, U32 alphaDst) override;
    void setColorMask(bool r, bool g, bool b, bool a) override;
    void setLineWidth(F32 width) override { ++calls; s.lineWidth = width; }
    void setViewport(S32 x, S32 y, S32 w, S32 h) override;
    void setScissor(S32 x, S32 y, S32 w, S32 h) override;
    void setCullFace(U32 face) override { ++calls; s.cullFace = face; }
    void setPolygonMode(U32 face, U32 mode) override { ++calls; s.polygonMode = mode; }
    void setPolygonOffset(F32 factor, F32 units) override { ++calls; s.offsetFactor = factor; s.offsetUnits = units; }
    void setPointSize(F32 size) override { ++calls; s.pointSize = size; }
    void setStencilFunc(U32 func, S32 ref, U32 mask) override { ++calls; s.stencilFunc = func; s.stencilRef = ref; s.stencilMask = mask; }
    void setStencilOp(U32 sfail, U32 dpfail, U32 dppass) override { ++calls; s.stencilOpFail = sfail; s.stencilOpDepthFail = dpfail; s.stencilOpPass = dppass; }
    void setStencilMask(U32 mask) override { ++calls; s.stencilMask = mask; }
};

class VulkanVertexBufferBackend final : public LLVertexBufferBackend
{
public:
    VulkanVertexBufferBackend(VkContext* ctx, VulkanRenderStateBackend* state) : mCtx(ctx), mState(state) {}
    ~VulkanVertexBufferBackend() override;

    bool usesDeferredUpload() const override { return false; }
    void allocate(BufferKind kind, U32 size, BufferHandle& handle, U8*& data) override;
    void free(BufferKind kind, U32 size, BufferHandle handle, U8* data) override;
    U64 getVramBytesUsed() override { return mBytes; }
    void createStatic(BufferKind kind, BufferHandle& handle, U32 size, const void* data) override;
    void bind(BufferKind kind, BufferHandle handle) override;
    bool isBound(BufferKind kind, BufferHandle handle) const override;
    void unbind() override { mBoundVB = {}; mBoundIB = {}; }
    void invalidateBufferBinding() override { mBoundVB = {}; }
    void upload(BufferKind kind, BufferHandle handle, U32 start, U32 end, const void* data) override;
    void setBuffers(BufferHandle vb, BufferHandle ib, U32 attribMask, const U32* offsets) override;
    void setEnabledAttributes(U32 mask) override { (void) mask; /* no per-array enable state in Vulkan */ }
    void initContextState() override { /* no dummy VAO needed */ }
    void drawIndexed(U32 mode, U32 start, U32 end, U32 count, IndexType type, U32 indexByteOffset) override;
    void drawArrays(U32 mode, U32 first, U32 count) override;
    void drawArraysNoInput(U32 mode, U32 first, U32 count) override;
    void setLabel(BufferHandle handle, const char* label) override { (void) handle; (void) label; }

    uint32_t pipelinesCreated() const { return (uint32_t) mPipelines.size(); }
    uint32_t drawCalls = 0;
    uint32_t uploadsWithoutBind = 0; // a GL rule ("upload needs the buffer bound") that Vulkan does not have

private:
    struct Buf
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        U32 size = 0;
        BufferKind kind = BufferKind::Vertex;
    };

    VkPipeline getPipeline(U32 mode, bool vertexInput);
    void recordCommon(U32 mode, bool indexed);

    VkContext* mCtx;
    VulkanRenderStateBackend* mState;
    std::unordered_map<U64, Buf> mBuffers;
    U64 mNextId = 1;
    U64 mBytes = 0;
    BufferHandle mBoundVB, mBoundIB;

    // pending vertex input, set by setBuffers()
    BufferHandle mCurVB, mCurIB;
    U32 mAttribMask = 0;
    U32 mOffsets[14] = {};

    std::map<std::string, VkPipeline> mPipelines;
};
