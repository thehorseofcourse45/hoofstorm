// Vulkan implementations of the viewer's backend interfaces:
//   LLRenderStateBackend      (llrenderstatebackend.h)   fixed pipeline state, recorded and resolved into pipelines
//   LLVertexBufferBackend     (llvertexbufferbackend.h)  buffers, vertex input and draws
#pragma once

#include "vkcontext.h"
#include "llrenderstatebackend.h"
#include "llvertexbufferbackend.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <unordered_map>

// Everything the viewer has told the state backend so far. GL values in, Vulkan state out.
struct RecordedState
{
    bool blend = false;
    bool cull = false;
    bool depthClamp = false;
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
    VulkanVertexBufferBackend(VkContext* ctx, VulkanRenderStateBackend* state)
        : mCtx(ctx), mState(state), mSlabs(std::make_shared<StorageSlabs>()) {}
    ~VulkanVertexBufferBackend() override;

    bool usesDeferredUpload() const override { return false; }
    void allocate(BufferKind kind, U32 size, BufferHandle& handle, U8*& data) override;
    void allocateWithId(BufferKind kind, U32 size, U64 id);     // GPU side of allocate() for a handle the caller already chose (threaded renderer)
    void free(BufferKind kind, U32 size, BufferHandle handle, U8* data) override;
    U64 getVramBytesUsed() override { return mBytes + mPooledBytes; }
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
    struct BufferStorage
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        VkDeviceSize allocationBytes = 0, capacity = 0;
        uint64_t usedFrame = 0;
        int32_t block = -1;     // StorageSlabs block and slot, or -1 for memory of its own
        uint32_t slot = 0;
    };
    // Storage up to kMaxSlot is carved from kBlock-sized allocations in equal power-of-two slots. One vkAllocateMemory
    // per storage cost a driver call each, and a crowd coming into view made thousands at once (render-thread frames
    // of 0.5-3 s, the main thread blocked behind them in stateSort).
    struct StorageSlabs
    {
        static constexpr VkDeviceSize kBlock = 16u << 20, kMinSlot = 4096, kMaxSlot = 1u << 20;
        struct Block
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint8_t* mapped = nullptr;
            VkDeviceSize slotSize = 0;
            uint32_t memoryType = 0;
            std::vector<uint32_t> free;
        };
        ~StorageSlabs();
        VkDevice device = VK_NULL_HANDLE;     // set with the first block
        std::atomic<int64_t>* liveBytes = nullptr;   // VkContext::hostBufferMemBytes
        std::mutex mutex;              // the reaper returns slots from its own thread
        std::vector<Block> blocks;     // emptied blocks are freed by destroyStorage (memory null) and reused
    };
    struct Buf : BufferStorage
    {
        U32 size = 0;
        BufferKind kind = BufferKind::Vertex;
        std::vector<BufferStorage> retired;
        std::vector<U8> shadow; // last uploaded bytes; avoids reading uncached GPU-visible memory when renaming
        // Host-visible storage on a discrete GPU is system RAM, read over PCIe on every vertex fetch. Buffers that stop
        // changing get a VRAM copy for drawing; an upload drops it again.
        VkBuffer device = VK_NULL_HANDLE;
        VkDeviceMemory deviceMemory = VK_NULL_HANDLE;
        uint64_t uploadFrame = 0;
        VkBuffer drawBuffer() const { return device ? device : buffer; }
    };
    void promote(Buf& b);
    void dropDeviceCopy(Buf& b);
    U64 mDeviceBytes = 0;
    static constexpr U64 kDeviceCap = 2048ull << 20;     // ponytail: fixed VRAM budget for vertex data, size it from the heap if it binds
    // Each promotion is its own vkAllocateMemory; crowds promoted thousands at once and stalled the render thread for 100+ ms.
    // ponytail: per-frame cap, suballocate device copies from large blocks if the remaining allocations still show in a profile
    static constexpr U32 kPromotionsPerFrame = 16;
    U64 mPromotionFrame = 0;
    U32 mPromotionsThisFrame = 0;
    BufferStorage createStorage(BufferKind kind, U32 size);
    static void destroyStorage(VkDevice device, StorageSlabs& slabs, const BufferStorage& storage);
    void recycleStorage(BufferKind kind, const BufferStorage& storage);

    struct PipelineJob;
    VkPipeline getPipeline(U32 mode, bool vertexInput);    // null: the pipeline is still compiling on a worker, skip the draw
    bool recordCommon(U32 mode, bool indexed, bool vertexInput = true);
    void runPipelineJob(PipelineJob& job);
    void drainCompiledPipelines();
    void waitForPipelineJobs();
    void startPipelineWorkers();
    void stopPipelineWorkers();
    void closeLoop(U32 last, U32 first);

    VkContext* mCtx;
    VulkanRenderStateBackend* mState;
    std::shared_ptr<StorageSlabs> mSlabs;     // shared with reaper jobs, which can outlive this backend
    std::unordered_map<U64, Buf> mBuffers;
    std::vector<Buf*> mById;    // mBuffers entries indexed by id (ids are sequential), null once freed; ponytail: grows 8 bytes per buffer ever made
    U64 mNextId = 1;
    U64 mBytes = 0;
    U64 mRetiredBytes = 0, mPooledBytes = 0;
    static constexpr U64 kPoolCap = 256u << 20, kRetiredCap = 128u << 20;   // completed spare storage / in-flight renamed versions
    static constexpr U64 kPoolIdleFrames = 900;
    U64 mLastPoolTrim = 0;
    std::multimap<std::pair<BufferKind, VkDeviceSize>, BufferStorage> mStoragePool;
    BufferHandle mBoundVB, mBoundIB;

    // pending vertex input, set by setBuffers()
    BufferHandle mCurVB, mCurIB;
    Buf *mCurVBp = nullptr, *mCurIBp = nullptr;     // mBuffers entries for mCurVB/mCurIB, looked up on first draw; cleared by setBuffers and free
    Buf& resolve(BufferHandle h, Buf*& cache);      // cached lookup of a draw buffer, aborts when the buffer is gone
    U32 mAttribMask = 0;
    U32 mOffsets[14] = {};

    std::map<std::string, VkPipeline> mPipelines;
    std::string mLastKey;                    // consecutive draws mostly repeat one pipeline: skip the map lookup
    VkPipeline mLastPipeline = VK_NULL_HANDLE;

    // Pipelines compile on worker threads when VkContext::asyncPipelines is set. mPipelines and mPending belong to the render thread.
    std::set<std::string> mPending;
    std::mutex mJobMutex;
    std::condition_variable mJobWake, mJobIdle;
    std::deque<PipelineJob*> mJobQueue;
    std::vector<PipelineJob*> mJobDone;
    std::atomic<size_t> mJobDoneCount{0};
    size_t mJobsInFlight = 0;
    bool mJobStop = false;
    std::vector<std::thread> mJobThreads;
};
