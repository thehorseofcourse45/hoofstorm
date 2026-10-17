#include "vkbackends.h"
#include "vkprogram.h"
#include "vkframebufferbackend.h"
#include "vkquerybackend.h"

#include <GL/gl.h> // GL constants only: the viewer's state vocabulary is GL values, so a backend has to know them
#include <cstring>
#include <algorithm>

// Mirrors LLVertexBuffer (llvertexbuffer.h / llvertexbuffer.cpp). The attribute vocabulary lives inside the
// LLVertexBuffer class today, so a backend that does not link the viewer has to repeat it. See the report.
namespace
{
enum AttributeType
{
    TYPE_VERTEX = 0, TYPE_NORMAL, TYPE_TEXCOORD0, TYPE_TEXCOORD1, TYPE_TEXCOORD2, TYPE_TEXCOORD3, TYPE_COLOR,
    TYPE_EMISSIVE, TYPE_TANGENT, TYPE_WEIGHT, TYPE_WEIGHT4, TYPE_CLOTHWEIGHT, TYPE_JOINT, TYPE_TEXTURE_INDEX, TYPE_MAX
};

// LLVertexBuffer::sTypeSize
const U32 kTypeSize[TYPE_MAX] = {16, 16, 8, 8, 8, 8, 4, 4, 16, 4, 16, 16, 8, 16};

// What LLVertexBuffer::setupVertexBuffer() told glVertexAttribPointer, as Vulkan formats.
VkFormat attribute_format(int type)
{
    switch (type)
    {
    case TYPE_VERTEX:
    case TYPE_NORMAL:        return VK_FORMAT_R32G32B32_SFLOAT;
    case TYPE_TEXCOORD0:
    case TYPE_TEXCOORD1:
    case TYPE_TEXCOORD2:
    case TYPE_TEXCOORD3:     return VK_FORMAT_R32G32_SFLOAT;
    case TYPE_COLOR:
    case TYPE_EMISSIVE:      return VK_FORMAT_R8G8B8A8_UNORM;
    case TYPE_TANGENT:
    case TYPE_WEIGHT4:
    case TYPE_CLOTHWEIGHT:   return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TYPE_WEIGHT:        return VK_FORMAT_R32_SFLOAT;
    case TYPE_JOINT:         return VK_FORMAT_R16G16B16A16_UINT;
    case TYPE_TEXTURE_INDEX: return VK_FORMAT_R32_UINT; // lives in position.w
    }
    return VK_FORMAT_UNDEFINED;
}

// LLRender::eBlendFactor order
VkBlendFactor blend_factor(U32 f)
{
    static const VkBlendFactor table[] = {
        VK_BLEND_FACTOR_ONE,                 VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_DST_COLOR,           VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,           VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    };
    return f < 10 ? table[f] : VK_BLEND_FACTOR_ZERO;
}

struct PipelineKey
{
    U32 attribMask;
    U8 topology;
    U8 cull;
    U8 polygonMode;
    U8 blendEnable;
    U8 factors[4];
    U8 colorMask;
    U8 vertexInput;
    U8 depthClamp;
    U8 depthTest;
    U8 depthWrite;
    U8 depthFunc;
    U32 program;
    VkFormat colors[4], depth;
    U32 colorCount;
    bool offscreen;
};
} // namespace

// ----------------------------------------------------------------------------------------------------------------
// State backend

void VulkanRenderStateBackend::setCapability(U32 cap, bool enabled)
{
    ++calls;
    switch (cap)
    {
    case GL_BLEND:               s.blend = enabled; break;
    case GL_CULL_FACE:           s.cull = enabled; break;
    case 0x864F:                 s.depthClamp = enabled; break;
    case GL_DEPTH_TEST:          s.depthTest = enabled; break;
    case GL_SCISSOR_TEST:        s.scissorTest = enabled; break;
    case GL_STENCIL_TEST:        s.stencilTest = enabled; break;
    case GL_POLYGON_OFFSET_FILL: s.offsetFill = enabled; break;
    case GL_POLYGON_OFFSET_LINE: s.offsetLine = enabled; break;
    default:                     ignoredCaps.insert(cap); break;
    }
}

bool VulkanRenderStateBackend::isCapabilityEnabled(U32 cap) const
{
    switch (cap)
    {
    case GL_BLEND:               return s.blend;
    case GL_CULL_FACE:           return s.cull;
    case 0x864F:                 return s.depthClamp;
    case GL_DEPTH_TEST:          return s.depthTest;
    case GL_SCISSOR_TEST:        return s.scissorTest;
    case GL_STENCIL_TEST:        return s.stencilTest;
    case GL_POLYGON_OFFSET_FILL: return s.offsetFill;
    case GL_POLYGON_OFFSET_LINE: return s.offsetLine;
    }
    return false;
}

void VulkanRenderStateBackend::setBlendFunc(U32 colorSrc, U32 colorDst, U32 alphaSrc, U32 alphaDst)
{
    ++calls;
    s.blendFactors[0] = (U8) colorSrc;
    s.blendFactors[1] = (U8) colorDst;
    s.blendFactors[2] = (U8) alphaSrc;
    s.blendFactors[3] = (U8) alphaDst;
}

void VulkanRenderStateBackend::setColorMask(bool r, bool g, bool b, bool a)
{
    ++calls;
    s.colorMask[0] = r;
    s.colorMask[1] = g;
    s.colorMask[2] = b;
    s.colorMask[3] = a;
}

void VulkanRenderStateBackend::setViewport(S32 x, S32 y, S32 w, S32 h)
{
    ++calls;
    s.viewport[0] = x; s.viewport[1] = y; s.viewport[2] = w; s.viewport[3] = h;
}

void VulkanRenderStateBackend::setScissor(S32 x, S32 y, S32 w, S32 h)
{
    ++calls;
    s.scissor[0] = x; s.scissor[1] = y; s.scissor[2] = w; s.scissor[3] = h;
}

// ----------------------------------------------------------------------------------------------------------------
// Vertex buffer backend: storage

VulkanVertexBufferBackend::~VulkanVertexBufferBackend()
{
    stopPipelineWorkers();
    drainCompiledPipelines();
    vkDeviceWaitIdle(mCtx->device);
    mCtx->collectReleasedResources(); // drain callbacks that recycle storage into this backend
    mLastPipeline = VK_NULL_HANDLE;
    for (auto& p : mPipelines)
    {
        vkDestroyPipeline(mCtx->device, p.second, nullptr);
    }
    if (mCtx->exiting) return;      // buffer memory goes with the process (see VkContext::exiting)
    for (const auto& entry : mStoragePool) destroyStorage(mCtx->device, entry.second);
    for (auto& kv : mBuffers)
    {
        destroyStorage(mCtx->device, kv.second);
        for (const auto& retired : kv.second.retired) destroyStorage(mCtx->device, retired);
        vkDestroyBuffer(mCtx->device, kv.second.device, nullptr);
        vkFreeMemory(mCtx->device, kv.second.deviceMemory, nullptr);
    }
}

void VulkanVertexBufferBackend::destroyStorage(VkDevice device, const BufferStorage& storage)
{
    vkUnmapMemory(device, storage.memory);
    vkDestroyBuffer(device, storage.buffer, nullptr);
    vkFreeMemory(device, storage.memory, nullptr);
}

void VulkanVertexBufferBackend::recycleStorage(BufferKind kind, const BufferStorage& storage)
{
    // Retain only completed buffers, with a separate 64 MiB cap from in-flight renamed storage.
    // Idle pooled storage is released after kPoolIdleFrames so a quiet scene does not keep a busy scene's memory.
    if (mCtx->frameCounter >= mLastPoolTrim + 300)
    {
        mLastPoolTrim = mCtx->frameCounter;
        for (auto it = mStoragePool.begin(); it != mStoragePool.end();)
        {
            if (it->second.usedFrame + kPoolIdleFrames < mCtx->frameCounter)
            {
                mPooledBytes -= it->second.allocationBytes;
                mCtx->reap([device = mCtx->device, storage = it->second] { destroyStorage(device, storage); });
                it = mStoragePool.erase(it);
            }
            else ++it;
        }
    }
    if (storage.allocationBytes <= kPoolCap - std::min<U64>(mPooledBytes, kPoolCap))
    {
        BufferStorage pooled = storage;
        pooled.usedFrame = mCtx->frameCounter;   // when it went idle, for trimming
        mStoragePool.emplace(std::make_pair(kind, storage.capacity), pooled);
        mPooledBytes += storage.allocationBytes;
    }
    else
    {
        // on the reaper: thousands of these ran inline during teardown (5 s of the exit), and on the render thread at runtime
        mCtx->reap([device = mCtx->device, storage] { destroyStorage(device, storage); });
        ++mCtx->bufferPoolEvictions;
    }
}

VulkanVertexBufferBackend::BufferStorage VulkanVertexBufferBackend::createStorage(BufferKind kind, U32 size)
{
    const VkDeviceSize capacity = (VkDeviceSize(size) + 4095) & ~VkDeviceSize(4095);
    auto available = mStoragePool.lower_bound(std::make_pair(kind, capacity));
    if (available != mStoragePool.end() && available->first.first == kind && available->first.second <= capacity * 2)
    {
        BufferStorage b = available->second;
        mPooledBytes -= b.allocationBytes;
        mStoragePool.erase(available);
        b.usedFrame = 0;
        ++mCtx->bufferPoolHits;
        return b;
    }
    BufferStorage b;
    b.capacity = capacity;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = capacity;
    bci.usage = (kind == BufferKind::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT) | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateBuffer(mCtx->device, &bci, nullptr, &b.buffer));
    ++mCtx->bufferAllocations;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mCtx->device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    b.allocationBytes = req.size;
    mai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(mCtx->device, &mai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(mCtx->device, b.buffer, b.memory, 0));
    VK_CHECK(vkMapMemory(mCtx->device, b.memory, 0, VK_WHOLE_SIZE, 0, (void**) &b.mapped));

    return b;
}

void VulkanVertexBufferBackend::allocateWithId(BufferKind kind, U32 size, U64 id)
{
    Buf b;
    b.shadow.assign(size, 0);
    static_cast<BufferStorage&>(b) = createStorage(kind, size);
    b.size = size;
    b.kind = kind;
    b.uploadFrame = mCtx->frameCounter;
    mNextId = id + 1;
    mBytes += b.allocationBytes;
    Buf& stored = mBuffers[id] = std::move(b);
    if (mById.size() <= id) mById.resize(id + 1024, nullptr);
    mById[id] = &stored;
}

void VulkanVertexBufferBackend::allocate(BufferKind kind, U32 size, BufferHandle& handle, U8*& data)
{
    handle.id = mNextId;
    allocateWithId(kind, size, handle.id);

    // the CPU shadow copy LLVertexBuffer writes through its striders (ll_aligned_malloc_16 in the viewer)
    data = (U8*) _aligned_malloc(size, 16);
    memset(data, 0, size);
}

void VulkanVertexBufferBackend::free(BufferKind, U32 size, BufferHandle handle, U8* data)
{
    if (data)
    {
        _aligned_free(data);
    }
    auto it = mBuffers.find(handle.id);
    if (it == mBuffers.end())
    {
        return;
    }
    dropDeviceCopy(it->second);
    const BufferStorage storage = it->second;
    auto retired = std::move(it->second.retired);
    mBytes -= storage.allocationBytes;
    for (const auto& version : retired)
    {
        mRetiredBytes -= version.allocationBytes;
        mBytes -= version.allocationBytes;
    }
    const auto kind = it->second.kind;
    mCtx->deferRelease([this, kind, storage, retired=std::move(retired)] {
        recycleStorage(kind, storage);
        for (const auto& version : retired) recycleStorage(kind, version);
    });
    if (mCurVBp == &it->second) mCurVBp = nullptr;
    if (mCurIBp == &it->second) mCurIBp = nullptr;
    mById[handle.id] = nullptr;
    mBuffers.erase(it);
    (void)size;
    if (mBoundVB == handle) mBoundVB = {};
    if (mBoundIB == handle) mBoundIB = {};
}

void VulkanVertexBufferBackend::createStatic(BufferKind, BufferHandle&, U32, const void*)
{
    fprintf(stderr, "FATAL: createStatic() is the deferred-upload (Apple) path; usesDeferredUpload() is false\n");
    abort();
}

void VulkanVertexBufferBackend::bind(BufferKind kind, BufferHandle handle)
{
    (kind == BufferKind::Vertex ? mBoundVB : mBoundIB) = handle;
}

bool VulkanVertexBufferBackend::isBound(BufferKind kind, BufferHandle handle) const
{
    return (kind == BufferKind::Vertex ? mBoundVB : mBoundIB) == handle;
}

void VulkanVertexBufferBackend::upload(BufferKind kind, BufferHandle handle, U32 start, U32 end, const void* data)
{
    if (!isBound(kind, handle))
    {
        ++uploadsWithoutBind; // GL would have written into whatever was bound; Vulkan does not care
    }
    auto it = mBuffers.find(handle.id);
    if (it == mBuffers.end() || end >= it->second.size)
    {
        fprintf(stderr, "FATAL: upload outside the buffer\n");
        abort();
    }
    auto& buffer = it->second;
    dropDeviceCopy(buffer);
    buffer.uploadFrame = mCtx->frameCounter;
    memcpy(buffer.shadow.data() + start, data, (size_t)end - start + 1);
    mCtx->refreshCompleted();
    auto safe = [&](const BufferStorage& storage) {
        return !storage.usedFrame || storage.usedFrame <= mCtx->completedFrame;
    };
    if (!safe(buffer))
    {
        auto available = std::find_if(buffer.retired.begin(), buffer.retired.end(), safe);
        // shortcut: extra backing storage is capped at 64 MiB; suballocate streaming storage if this fallback is frequent.
        if (available == buffer.retired.end() && mRetiredBytes + buffer.allocationBytes > kRetiredCap)
        {
            if (mCtx->recording) mCtx->flushCommands(VkContext::Stall::BufferReuse);
            else
            {
                const auto start=std::chrono::steady_clock::now();
                mCtx->waitForFrame(mCtx->submittedFrame);
                mCtx->recordStall(VkContext::Stall::BufferReuse,start);
            }
            for (auto& entry : mBuffers)
            {
                entry.second.usedFrame = 0;
                for (auto& retired : entry.second.retired) retired.usedFrame = 0;
            }
        }
        else
        {
            BufferStorage replacement;
            if (available != buffer.retired.end())
            {
                replacement = *available;
                buffer.retired.erase(available);
            }
            else
            {
                replacement = createStorage(buffer.kind, buffer.size);
                mBytes += replacement.allocationBytes;
                mRetiredBytes += replacement.allocationBytes;
            }
            // Partial uploads must preserve every byte not included in this update.
            memcpy(replacement.mapped, buffer.shadow.data(), buffer.size);
            buffer.retired.push_back(buffer);
            static_cast<BufferStorage&>(buffer) = replacement;
            ++mCtx->bufferRenames;
        }
    }
    buffer.usedFrame = 0;
    memcpy(buffer.mapped + start, buffer.shadow.data() + start, (size_t) end - start + 1);
}

// ----------------------------------------------------------------------------------------------------------------
// Vertex buffer backend: vertex input and draws

void VulkanVertexBufferBackend::setBuffers(BufferHandle vb, BufferHandle ib, U32 attribMask, const U32* offsets)
{
    if (!(vb == mCurVB)) mCurVBp = nullptr;
    if (!(ib == mCurIB)) mCurIBp = nullptr;
    mCurVB = vb;
    mCurIB = ib;
    mAttribMask = attribMask;
    memcpy(mOffsets, offsets, sizeof(mOffsets));
    mBoundVB = vb;
    mBoundIB = ib;
}

// Everything vkCreateGraphicsPipelines reads lives in the job, which is never moved: its create-info structures point into itself.
struct VulkanVertexBufferBackend::PipelineJob
{
    std::string key;
    PipelineKey keyData{};
    std::shared_ptr<std::atomic<int>> refs;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attribs;
    VkPipelineShaderStageCreateInfo stages[3]{};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blendAttachments[4]{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo feedbackInfo{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipeline pipeline = VK_NULL_HANDLE;
    double ms_taken = 0;
    bool cacheHit = false;
};

void VulkanVertexBufferBackend::runPipelineJob(PipelineJob& job)
{
    const auto start = std::chrono::steady_clock::now();
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device, mCtx->pipelineCache, 1, &job.gp, nullptr, &job.pipeline));
    job.ms_taken = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    job.cacheHit = (job.feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
                   (job.feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
}

void VulkanVertexBufferBackend::drainCompiledPipelines()
{
    if (!mJobDoneCount.load()) return;
    std::vector<PipelineJob*> done;
    {
        std::lock_guard<std::mutex> lock(mJobMutex);
        done.swap(mJobDone);
        mJobDoneCount = 0;
    }
    for (PipelineJob* job : done)
    {
        mPipelines[job->key] = job->pipeline;
        mPending.erase(job->key);
        mCtx->pipelineCreationMs += job->ms_taken;
        ++mCtx->pipelinesCreated;
        if (job->cacheHit) ++mCtx->pipelineCacheHits;
        delete job;
    }
}

void VulkanVertexBufferBackend::waitForPipelineJobs()
{
    std::unique_lock<std::mutex> lock(mJobMutex);
    mJobIdle.wait(lock, [this] { return mJobsInFlight == 0; });
}

void VulkanVertexBufferBackend::startPipelineWorkers()
{
    if (!mJobThreads.empty()) return;
    const unsigned count = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
    for (unsigned i = 0; i < count; ++i)
    {
        mJobThreads.emplace_back([this] {
            for (;;)
            {
                PipelineJob* job = nullptr;
                {
                    std::unique_lock<std::mutex> lock(mJobMutex);
                    mJobWake.wait(lock, [this] { return mJobStop || !mJobQueue.empty(); });
                    if (mJobQueue.empty()) return;   // stopping, and nothing left
                    job = mJobQueue.front();
                    mJobQueue.pop_front();
                }
                runPipelineJob(*job);
                job->refs->fetch_sub(1);
                {
                    std::lock_guard<std::mutex> lock(mJobMutex);
                    mJobDone.push_back(job);
                    mJobDoneCount = mJobDone.size();
                    --mJobsInFlight;
                }
                mJobIdle.notify_all();
            }
        });
    }
}

void VulkanVertexBufferBackend::stopPipelineWorkers()
{
    {
        std::lock_guard<std::mutex> lock(mJobMutex);
        mJobStop = true;
    }
    mJobWake.notify_all();
    for (auto& thread : mJobThreads) thread.join();
    mJobThreads.clear();
}

VkPipeline VulkanVertexBufferBackend::getPipeline(U32 mode, bool vertexInput)
{
    const RecordedState& st = mState->s;

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.attribMask = vertexInput ? mAttribMask : 0;
    key.vertexInput = vertexInput;
    key.topology = (U8) mode;
    key.cull = st.cull ? (U8) st.cullFace : 0;
    key.polygonMode = (U8) (st.polygonMode & 0xFF);
    key.blendEnable = st.blend;
    memcpy(key.factors, st.blendFactors, 4);
    key.colorMask = (U8) ((st.colorMask[0] ? 1u : 0u) | (st.colorMask[1] ? 2u : 0u) |
                         (st.colorMask[2] ? 4u : 0u) | (st.colorMask[3] ? 8u : 0u));
    key.depthClamp = st.depthClamp && mCtx->depthClamp;
    key.depthTest = st.depthTest;
    key.depthWrite = st.depthWrite;
    key.depthFunc = (U8) (st.depthFunc & 0xFF);
    key.program = mCtx->program ? mCtx->program->id : 0;
    auto formats = mCtx->framebuffers->colorFormats();
    memcpy(key.colors, formats.data(), sizeof(key.colors));
    key.depth = mCtx->framebuffers->depthFormat();
    key.colorCount = mCtx->framebuffers->colorCount();
    key.offscreen = mCtx->framebuffers->getBound() != 0;

    if (mLastPipeline && mLastKey.size() == sizeof(key) && memcmp(mLastKey.data(), &key, sizeof(key)) == 0)
    {
        return mLastPipeline;
    }
    std::string k((const char*) &key, sizeof(key));
    drainCompiledPipelines();
    const bool async = mCtx->asyncPipelines && mCtx->syncPipelineScopes == 0;
    auto it = mPipelines.find(k);
    if (it == mPipelines.end() && !async && mPending.count(k))
    {
        waitForPipelineJobs();   // a synchronous caller needs the pipeline a worker is building
        drainCompiledPipelines();
        it = mPipelines.find(k);
    }
    if (it != mPipelines.end())
    {
        mLastKey = k;
        mLastPipeline = it->second;
        return it->second;
    }
    if (mPending.count(k))
    {
        return VK_NULL_HANDLE;
    }

    if (!mCtx->program)
    {
        fprintf(stderr, "FATAL: no current program (LLShaderBackend::use() sets it in the viewer)\n");
        abort();
    }

    PipelineJob* jobPtr = new PipelineJob;
    PipelineJob& j = *jobPtr;
    j.key = k;
    j.keyData = key;
    j.refs = mCtx->program->compileRefs;
    VkProgram& program = *mCtx->program;

    j.stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    j.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    j.stages[0].module = program.vs;
    j.stages[0].pName = "main";
    j.stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    j.stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    j.stages[1].module = program.fs;
    j.stages[1].pName = "main";
    j.stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    j.stages[2].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    j.stages[2].module = program.gs;
    j.stages[2].pName = "main";

    // vertex input from the attribute mask: one binding per attribute array (LLVertexBuffer stores each
    // attribute as its own array inside the buffer), binding index = attribute location = AttributeType.
    if (vertexInput)
    {
        for (int t = 0; t < TYPE_MAX; ++t)
        {
            if (!(mAttribMask & (1u << t)))
            {
                continue;
            }
            j.bindings.push_back({(uint32_t) t, kTypeSize[t], VK_VERTEX_INPUT_RATE_VERTEX});
            j.attribs.push_back({(uint32_t) t, (uint32_t) t, attribute_format(t), t == TYPE_TEXTURE_INDEX ? 12u : 0u});
        }
    }
    j.vi.vertexBindingDescriptionCount = (uint32_t) j.bindings.size();
    j.vi.pVertexBindingDescriptions = j.bindings.data();
    j.vi.vertexAttributeDescriptionCount = (uint32_t) j.attribs.size();
    j.vi.pVertexAttributeDescriptions = j.attribs.data();

    VkPrimitiveTopology topo;
    switch (mode) // LLRender::eGeomModes
    {
    case 0: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 1: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 2: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 3: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 4: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 5: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    default:
        fprintf(stderr, "FATAL: LLRender::LINE_LOOP has no Vulkan topology (needs an index rewrite)\n");
        abort();
    }
    j.ia.topology = topo;

    j.vp.viewportCount = 1;
    j.vp.scissorCount = 1;
    // GL's clip depth range is -1..1; without this Vulkan clips everything with z < 0
    j.clip.negativeOneToOne = VK_TRUE;
    if (mCtx->depthClipControl)
    {
        j.vp.pNext = &j.clip;
    }

    j.rs.polygonMode = st.polygonMode == GL_LINE ? VK_POLYGON_MODE_LINE : st.polygonMode == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    j.rs.cullMode = !st.cull ? VK_CULL_MODE_NONE : st.cullFace == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    j.rs.frontFace = key.offscreen ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE; // GL default; the negative-height viewport keeps GL winding
    j.rs.lineWidth = 1.0f;
    j.rs.depthClampEnable = key.depthClamp;

    j.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = st.blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = blend_factor(st.blendFactors[0]);
    cba.dstColorBlendFactor = blend_factor(st.blendFactors[1]);
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = blend_factor(st.blendFactors[2]);
    cba.dstAlphaBlendFactor = blend_factor(st.blendFactors[3]);
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = (st.colorMask[0] ? VK_COLOR_COMPONENT_R_BIT : 0) | (st.colorMask[1] ? VK_COLOR_COMPONENT_G_BIT : 0) |
                         (st.colorMask[2] ? VK_COLOR_COMPONENT_B_BIT : 0) | (st.colorMask[3] ? VK_COLOR_COMPONENT_A_BIT : 0);
    for (auto& attachment : j.blendAttachments) attachment = cba;
    j.cb.attachmentCount = key.colorCount;
    j.cb.pAttachments = j.blendAttachments;

    // Viewport and scissor are dynamic: they change constantly (render targets, UI clipping) and are core dynamic state.
    j.ds.dynamicStateCount = 2;
    j.ds.pDynamicStates = j.dyn;

    // GL depth functions (GL_NEVER .. GL_ALWAYS = 0x200 .. 0x207) are in the same order as VkCompareOp. Stencil and polygon
    // offset are still recorded but not applied.
    j.dss.depthTestEnable = st.depthTest ? VK_TRUE : VK_FALSE;
    j.dss.depthWriteEnable = st.depthTest && st.depthWrite ? VK_TRUE : VK_FALSE;
    j.dss.depthCompareOp = (VkCompareOp) (st.depthFunc - GL_NEVER);

    j.ri.colorAttachmentCount = key.colorCount;
    j.ri.pColorAttachmentFormats = j.keyData.colors;
    j.ri.depthAttachmentFormat = key.depth;

    j.gp.pNext = &j.ri;
    j.gp.stageCount = program.gs ? 3 : 2;
    j.gp.pStages = j.stages;
    j.gp.pVertexInputState = &j.vi;
    j.gp.pInputAssemblyState = &j.ia;
    j.gp.pViewportState = &j.vp;
    j.gp.pRasterizationState = &j.rs;
    j.gp.pMultisampleState = &j.ms;
    j.gp.pDepthStencilState = &j.dss;
    j.gp.pColorBlendState = &j.cb;
    j.gp.pDynamicState = &j.ds;
    j.gp.layout = program.pipelineLayout;

    j.feedbackInfo.pPipelineCreationFeedback = &j.feedback;
    j.ri.pNext = &j.feedbackInfo;

    if (async)
    {
        startPipelineWorkers();
        j.refs->fetch_add(1);
        mPending.insert(k);
        {
            std::lock_guard<std::mutex> lock(mJobMutex);
            mJobQueue.push_back(jobPtr);
            ++mJobsInFlight;
        }
        mJobWake.notify_one();
        return VK_NULL_HANDLE;
    }

    runPipelineJob(j);
    mCtx->pipelineCreationMs += j.ms_taken;
    ++mCtx->pipelinesCreated;
    if (j.cacheHit) ++mCtx->pipelineCacheHits;
    const VkPipeline pipeline = j.pipeline;
    mPipelines[k] = pipeline;
    mLastKey = k;
    mLastPipeline = pipeline;
    delete jobPtr;
    return pipeline;
}

VulkanVertexBufferBackend::Buf& VulkanVertexBufferBackend::resolve(BufferHandle h, Buf*& cache)
{
    if (!cache)
    {
        cache = h.id < mById.size() ? mById[h.id] : nullptr;
        if (!cache)
        {
            fprintf(stderr, "FATAL: draw with a buffer that was freed or never allocated\n");
            abort();
        }
    }
    cache->usedFrame = mCtx->frameCounter;
    // Only buffers left alone for about a second: earlier, most copies were dropped again by the next upload.
    if (!cache->device && cache->size && cache->uploadFrame + 60 < mCtx->frameCounter && mDeviceBytes + cache->size <= kDeviceCap)
    {
        if (mPromotionFrame != mCtx->frameCounter) { mPromotionFrame = mCtx->frameCounter; mPromotionsThisFrame = 0; }
        if (mPromotionsThisFrame < kPromotionsPerFrame) { ++mPromotionsThisFrame; promote(*cache); }
    }
    return *cache;
}

void VulkanVertexBufferBackend::promote(Buf& b)
{
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = b.size;
    bci.usage = (b.kind == BufferKind::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT) | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(mCtx->device, &bci, nullptr, &b.device));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mCtx->device, b.device, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(mCtx->device, &mai, nullptr, &b.deviceMemory) != VK_SUCCESS)
    {
        vkDestroyBuffer(mCtx->device, b.device, nullptr);     // VRAM full: keep drawing from host memory
        b.device = VK_NULL_HANDLE;
        b.uploadFrame = mCtx->frameCounter;                 // and do not retry every draw
        return;
    }
    VK_CHECK(vkBindBufferMemory(mCtx->device, b.device, b.deviceMemory, 0));
    mDeviceBytes += b.size;
    ++mCtx->bufferPromotions;
    mCtx->bufferVramBytes = mDeviceBytes;
    // The batch is submitted ahead of the frame commands that hold this draw, and the barrier orders the copy before their reads.
    VkCommandBuffer cmd = mCtx->batchCommands();
    const VkBufferCopy region{0, 0, b.size};
    vkCmdCopyBuffer(cmd, b.buffer, b.device, 1, &region);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = b.kind == BufferKind::Vertex ? VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT : VK_ACCESS_INDEX_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = b.device;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void VulkanVertexBufferBackend::dropDeviceCopy(Buf& b)
{
    if (!b.device) return;
    auto device = mCtx->device; auto buffer = b.device; auto memory = b.deviceMemory;
    mCtx->deferFree([device, buffer, memory] { vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr); });
    mDeviceBytes -= b.size;
    ++mCtx->bufferDemotions;
    mCtx->bufferVramBytes = mDeviceBytes;
    b.device = VK_NULL_HANDLE;
    b.deviceMemory = VK_NULL_HANDLE;
}

bool VulkanVertexBufferBackend::recordCommon(U32 mode, bool indexed, bool vertexInput)
{
    if (!mCtx->framebuffers->ensureFrame()) throw std::runtime_error("Draw attempted while Vulkan window is minimized");
    if (!mCtx->program) throw std::runtime_error("Vulkan draw requires a bound shader");
    const RecordedState& st = mState->s;
    VkCommandBuffer cmd = mCtx->slot().cmd;
    const auto renderExtent = mCtx->framebuffers->extent();
    const bool offscreen = mCtx->framebuffers->getBound() != 0;
    const float fbH = (float) renderExtent.height;

    // Resolving a missing texture can submit pending commands; bind pipeline state afterwards.
    mCtx->framebuffers->prepareDraw();
    mCtx->program->prepareDraw();
    mCtx->framebuffers->beginRendering();
    const VkPipeline pipeline = getPipeline(mode, vertexInput);
    if (!pipeline)
    {
        ++mCtx->pipelineDrawsSkipped;   // still compiling; the draw is dropped and appears once the worker finishes
        return false;
    }
    auto& dc = mCtx->cache();
    if (dc.pipeline != pipeline)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        dc.pipeline = pipeline;
    }   // uniform block snapshot and descriptor sets of the current program

    // GL's window origin is bottom-left, Vulkan's is top-left. A negative viewport height flips Y so that GL
    // clip space, GL winding and GL viewport rectangles keep working unchanged (core since Vulkan 1.1).
    int vx = st.viewport[2] ? st.viewport[0] : 0, vy = st.viewport[2] ? st.viewport[1] : 0;
    int vw = st.viewport[2] ? st.viewport[2] : (int) renderExtent.width, vh = st.viewport[2] ? st.viewport[3] : (int) renderExtent.height;
    // Offscreen row zero is GL's texture bottom; the window needs a Y flip for presentation.
    VkViewport viewport{(float) vx, offscreen ? (float) vy : fbH - (float) vy, (float) vw,
                         offscreen ? (float) vh : -(float) vh, 0.0f, 1.0f};
    if (!dc.viewportSet || memcmp(&dc.viewport, &viewport, sizeof(viewport)) != 0)
    {
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        dc.viewport = viewport;
    }

    VkRect2D scissor{{0, 0}, renderExtent};
    if (st.scissorTest)
    {
        int x0 = std::clamp(st.scissor[0], 0, (int) renderExtent.width);
        int y0 = std::clamp(st.scissor[1], 0, (int) renderExtent.height);
        int x1 = std::clamp(st.scissor[0] + std::max(0, st.scissor[2]), x0, (int) renderExtent.width);
        int y1 = std::clamp(st.scissor[1] + std::max(0, st.scissor[3]), y0, (int) renderExtent.height);
        scissor.offset = {x0, offscreen ? y0 : (int) renderExtent.height - y1};
        scissor.extent = {(uint32_t) (x1 - x0), (uint32_t) (y1 - y0)};
    }
    if (!dc.viewportSet || memcmp(&dc.scissor, &scissor, sizeof(scissor)) != 0)
    {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        dc.scissor = scissor;
    }
    dc.viewportSet = true;

    ++mCtx->statDraws;
    if (mCtx->queries) mCtx->queries->passDraw(mCtx->programHandle);
    ++mCtx->statPassDraws[{mCtx->framebuffers->getBound(), renderExtent.width, renderExtent.height, mCtx->framebuffers->colorCount()}].first;
    if (!vertexInput) { ++drawCalls; return true; }
    VkBuffer vb = resolve(mCurVB, mCurVBp).drawBuffer();
    for (int t = 0; t < TYPE_MAX; ++t)
    {
        if (mAttribMask & (1u << t))
        {
            VkDeviceSize offset = mOffsets[t == TYPE_TEXTURE_INDEX ? TYPE_VERTEX : t];
            if (dc.vertexBuffer[t] != vb || dc.vertexOffset[t] != offset)
            {
                vkCmdBindVertexBuffers(cmd, (uint32_t) t, 1, &vb, &offset);
                dc.vertexBuffer[t] = vb;
                dc.vertexOffset[t] = offset;
            }
        }
    }
    (void) indexed;
    ++drawCalls;
    return true;
}

void VulkanVertexBufferBackend::drawIndexed(U32 mode, U32 start, U32 end, U32 count, IndexType type, U32 indexByteOffset)
{
    (void) start; // glDrawRangeElements start/end are optimisation hints in GL; Vulkan has no use for them
    (void) end;
    const bool loop = mode == 6;
    if (!recordCommon(loop ? 5 : mode, true)) return;

    const U32 indexSize = type == IndexType::U32 ? 4 : 2;
    Buf& ib = resolve(mCurIB, mCurIBp);
    const VkIndexType indexType = type == IndexType::U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    auto& dc = mCtx->drawCache;
    if (dc.indexBuffer != ib.drawBuffer() || dc.indexType != indexType)
    {
        vkCmdBindIndexBuffer(mCtx->slot().cmd, ib.drawBuffer(), 0, indexType);
        dc.indexBuffer = ib.drawBuffer();
        dc.indexType = indexType;
    }
    vkCmdDrawIndexed(mCtx->slot().cmd, count, 1, indexByteOffset / indexSize, 0, 0);
    if(loop && count>1)
    {
        if(indexByteOffset + uint64_t(count)*indexSize > ib.size) throw std::runtime_error("Line loop indices outside buffer");
        U32 first=0,last=0;
        memcpy(&first,ib.mapped+indexByteOffset,indexSize);
        memcpy(&last,ib.mapped+indexByteOffset+(count-1)*indexSize,indexSize);
        closeLoop(last,first);
    }
}

void VulkanVertexBufferBackend::drawArrays(U32 mode, U32 first, U32 count)
{
    const bool loop = mode == 6;
    if (!recordCommon(loop ? 5 : mode, false)) return;
    // GL's drawArrays has first as a vertex index; the per-attribute buffer offsets already cover the array start
    vkCmdDraw(mCtx->slot().cmd, count, 1, first, 0);
    if(loop && count>1) closeLoop(first+count-1,first);
}

void VulkanVertexBufferBackend::closeLoop(U32 last, U32 first)
{
    const U32 indices[2]={last,first};
    BufferHandle handle; U8* shadow=nullptr;
    allocate(BufferKind::Index,sizeof indices,handle,shadow);
    upload(BufferKind::Index,handle,0,sizeof indices-1,indices);
    vkCmdBindIndexBuffer(mCtx->slot().cmd,mBuffers.at(handle.id).buffer,0,VK_INDEX_TYPE_UINT32);
    mCtx->drawCache.indexBuffer = VK_NULL_HANDLE;     // the cache no longer matches what is bound
    vkCmdDrawIndexed(mCtx->slot().cmd,2,1,0,0,0);
    free(BufferKind::Index,sizeof indices,handle,shadow);
}
void VulkanVertexBufferBackend::drawArraysNoInput(U32 mode, U32 first, U32 count)
{
    const bool loop=mode==6;
    // The viewer downscale caller has already configured its screen-triangle arrays.
    if (!recordCommon(loop?5:mode,false,mBoundVB.id != 0 && mAttribMask != 0)) return;
    vkCmdDraw(mCtx->slot().cmd,count,1,first,0);
    if(loop && count>1) closeLoop(first+count-1,first);
}
