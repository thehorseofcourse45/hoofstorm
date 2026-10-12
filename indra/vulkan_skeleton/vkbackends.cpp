#include "vkbackends.h"

#include <GL/gl.h> // GL constants only: the viewer's state vocabulary is GL values, so a backend has to know them
#include <cstring>

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
    vkDeviceWaitIdle(mCtx->device);
    for (auto& p : mPipelines)
    {
        vkDestroyPipeline(mCtx->device, p.second, nullptr);
    }
    if (mLayout)
    {
        vkDestroyPipelineLayout(mCtx->device, mLayout, nullptr);
    }
    for (auto& kv : mBuffers)
    {
        vkUnmapMemory(mCtx->device, kv.second.memory);
        vkDestroyBuffer(mCtx->device, kv.second.buffer, nullptr);
        vkFreeMemory(mCtx->device, kv.second.memory, nullptr);
    }
}

void VulkanVertexBufferBackend::allocate(BufferKind kind, U32 size, BufferHandle& handle, U8*& data)
{
    // Simplest possible storage: one host-visible, persistently mapped buffer. A real backend would use
    // device-local memory with a staging copy, and a suballocator (VMA) instead of one allocation per buffer.
    Buf b;
    b.size = size;
    b.kind = kind;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = kind == BufferKind::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(mCtx->device, &bci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mCtx->device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mCtx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(mCtx->device, &mai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(mCtx->device, b.buffer, b.memory, 0));
    VK_CHECK(vkMapMemory(mCtx->device, b.memory, 0, VK_WHOLE_SIZE, 0, (void**) &b.mapped));

    handle.id = mNextId++;
    mBuffers[handle.id] = b;
    mBytes += size;

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
    // The GL backend defers glDeleteBuffers by 4 frames. Vulkan needs a real fence-aware deletion queue; the
    // skeleton just waits for the GPU.
    vkDeviceWaitIdle(mCtx->device);
    vkUnmapMemory(mCtx->device, it->second.memory);
    vkDestroyBuffer(mCtx->device, it->second.buffer, nullptr);
    vkFreeMemory(mCtx->device, it->second.memory, nullptr);
    mBuffers.erase(it);
    mBytes -= size;
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
    memcpy(it->second.mapped + start, data, (size_t) end - start + 1);
}

// ----------------------------------------------------------------------------------------------------------------
// Vertex buffer backend: vertex input and draws

void VulkanVertexBufferBackend::setBuffers(BufferHandle vb, BufferHandle ib, U32 attribMask, const U32* offsets)
{
    mCurVB = vb;
    mCurIB = ib;
    mAttribMask = attribMask;
    memcpy(mOffsets, offsets, sizeof(mOffsets));
    mBoundVB = vb;
    mBoundIB = ib;
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
    key.colorMask = (U8) (st.colorMask[0] | st.colorMask[1] << 1 | st.colorMask[2] << 2 | st.colorMask[3] << 3);

    std::string k((const char*) &key, sizeof(key));
    auto it = mPipelines.find(k);
    if (it != mPipelines.end())
    {
        return it->second;
    }

    if (!mCtx->vertexShader || !mCtx->fragmentShader)
    {
        fprintf(stderr, "FATAL: no current shader (the interfaces cannot express one)\n");
        abort();
    }

    if (!mLayout)
    {
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VK_CHECK(vkCreatePipelineLayout(mCtx->device, &lci, nullptr, &mLayout));
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mCtx->vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mCtx->fragmentShader;
    stages[1].pName = "main";

    // vertex input from the attribute mask: one binding per attribute array (LLVertexBuffer stores each
    // attribute as its own array inside the buffer), binding index = attribute location = AttributeType.
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attribs;
    if (vertexInput)
    {
        for (int t = 0; t < TYPE_MAX; ++t)
        {
            if (!(mAttribMask & (1u << t)))
            {
                continue;
            }
            bindings.push_back({(uint32_t) t, kTypeSize[t], VK_VERTEX_INPUT_RATE_VERTEX});
            attribs.push_back({(uint32_t) t, (uint32_t) t, attribute_format(t), t == TYPE_TEXTURE_INDEX ? 12u : 0u});
        }
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t) bindings.size();
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = (uint32_t) attribs.size();
    vi.pVertexAttributeDescriptions = attribs.data();

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
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = st.polygonMode == GL_LINE ? VK_POLYGON_MODE_LINE : st.polygonMode == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    rs.cullMode = !st.cull ? VK_CULL_MODE_NONE : st.cullFace == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // GL default; the negative-height viewport keeps GL winding
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    // Viewport and scissor are dynamic: they change constantly (render targets, UI clipping) and are core dynamic state.
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    // No depth/stencil attachment in the skeleton, so depth and stencil state from the viewer is recorded but cannot apply.
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &mCtx->swapFormat;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &ri;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = mLayout;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline));
    mPipelines[k] = pipeline;
    return pipeline;
}

void VulkanVertexBufferBackend::recordCommon(U32 mode, bool indexed)
{
    const RecordedState& st = mState->s;
    VkCommandBuffer cmd = mCtx->cmd;
    const float fbH = (float) mCtx->extent.height;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, getPipeline(mode, true));

    // GL's window origin is bottom-left, Vulkan's is top-left. A negative viewport height flips Y so that GL
    // clip space, GL winding and GL viewport rectangles keep working unchanged (core since Vulkan 1.1).
    int vx = st.viewport[2] ? st.viewport[0] : 0, vy = st.viewport[2] ? st.viewport[1] : 0;
    int vw = st.viewport[2] ? st.viewport[2] : (int) mCtx->extent.width, vh = st.viewport[2] ? st.viewport[3] : (int) mCtx->extent.height;
    VkViewport viewport{(float) vx, fbH - (float) vy, (float) vw, -(float) vh, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, mCtx->extent};
    if (st.scissorTest)
    {
        scissor.offset = {st.scissor[0], (int32_t) fbH - st.scissor[1] - st.scissor[3]};
        scissor.extent = {(uint32_t) st.scissor[2], (uint32_t) st.scissor[3]};
    }
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    auto bufferOf = [&](BufferHandle h) {
        auto it = mBuffers.find(h.id);
        if (it == mBuffers.end())
        {
            fprintf(stderr, "FATAL: draw with a buffer that was freed or never allocated\n");
            abort();
        }
        return it->second.buffer;
    };

    VkBuffer vb = bufferOf(mCurVB);
    for (int t = 0; t < TYPE_MAX; ++t)
    {
        if (mAttribMask & (1u << t))
        {
            VkDeviceSize offset = mOffsets[t == TYPE_TEXTURE_INDEX ? TYPE_VERTEX : t];
            vkCmdBindVertexBuffers(cmd, (uint32_t) t, 1, &vb, &offset);
        }
    }
    (void) indexed;
    ++drawCalls;
}

void VulkanVertexBufferBackend::drawIndexed(U32 mode, U32 start, U32 end, U32 count, IndexType type, U32 indexByteOffset)
{
    (void) start; // glDrawRangeElements start/end are optimisation hints in GL; Vulkan has no use for them
    (void) end;
    recordCommon(mode, true);

    const U32 indexSize = type == IndexType::U32 ? 4 : 2;
    auto it = mBuffers.find(mCurIB.id);
    if (it == mBuffers.end())
    {
        fprintf(stderr, "FATAL: drawIndexed without an index buffer\n");
        abort();
    }
    vkCmdBindIndexBuffer(mCtx->cmd, it->second.buffer, 0, type == IndexType::U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(mCtx->cmd, count, 1, indexByteOffset / indexSize, 0, 0);
}

void VulkanVertexBufferBackend::drawArrays(U32 mode, U32 first, U32 count)
{
    recordCommon(mode, false);
    // GL's drawArrays has first as a vertex index; the per-attribute buffer offsets already cover the array start
    vkCmdDraw(mCtx->cmd, count, 1, first, 0);
}

void VulkanVertexBufferBackend::drawArraysNoInput(U32, U32, U32)
{
    fprintf(stderr, "FATAL: drawArraysNoInput() is not exercised by the skeleton\n");
    abort();
}
