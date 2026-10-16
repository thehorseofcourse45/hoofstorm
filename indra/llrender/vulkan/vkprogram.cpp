#include "vkprogram.h"
#include <thread>
#include "vktexturebackend.h"

#include <cstring>
#include <memory>
#include <array>

static uint32_t sNextProgramId = 1;

VkProgram* VkProgram::create(VkContext& ctx, const std::vector<uint32_t>& vsSpirv, const std::vector<uint32_t>& fsSpirv,
                             const LLShaderTranslator::BlockLayout& layout,
                             const std::vector<LLShaderTranslator::SamplerBinding>& samplers, const std::vector<uint32_t>& gsSpirv)
{
    auto cleanup = [](VkProgram* program) { program->destroy(); };
    std::unique_ptr<VkProgram, decltype(cleanup)> guard(new VkProgram(), cleanup);
    VkProgram* p = guard.get();
    p->ctx = &ctx;
    p->id = sNextProgramId++;
    p->vs = ctx.createShaderModule(vsSpirv.data(), vsSpirv.size() * 4);
    p->fs = ctx.createShaderModule(fsSpirv.data(), fsSpirv.size() * 4);
    if (!gsSpirv.empty()) p->gs = ctx.createShaderModule(gsSpirv.data(), gsSpirv.size() * 4);
    p->layout = layout;
    p->shadow.assign((size_t) layout.size, 0);
    p->samplerNames.resize(samplers.size());
    p->samplerTypes.resize(samplers.size());
    p->samplerKinds.resize(samplers.size());
    for (size_t i = 0; i < samplers.size(); ++i)
    {
        p->samplerNames[i] = samplers[i].name;
        p->samplerTypes[i] = samplers[i].type;
        p->samplerKinds[i] = VulkanTextureBackend::samplerKind(samplers[i].type);
        p->samplerBindings.push_back((uint32_t) samplers[i].binding);
    }
    p->samplerUnit.resize(samplers.size());
    for (size_t i = 0; i < samplers.size(); ++i)
    {
        p->samplerUnit[i] = (int) i;        // GL's default for a sampler uniform is unit 0; the viewer sets every one explicitly
    }
    p->lastInfos.assign(samplers.size(), VkDescriptorImageInfo{});

    // set 0: the uniform block (an empty set layout if the program has no loose uniforms)
    VkDescriptorSetLayoutBinding ubo{};
    ubo.binding = 0;
    ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ubo.descriptorCount = 1;
    ubo.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = layout.size ? 1 : 0;
    lci.pBindings = &ubo;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &lci, nullptr, &p->uniformSetLayout));

    // set 1: one combined image sampler per sampler
    std::vector<VkDescriptorSetLayoutBinding> sb(samplers.size());
    for (size_t i = 0; i < sb.size(); ++i)
    {
        sb[i] = {};
        sb[i].binding = p->samplerBindings[i];
        sb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb[i].descriptorCount = 1;
        sb[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    }
    lci.bindingCount = (uint32_t) sb.size();
    lci.pBindings = sb.data();
    p->pushSamplers = ctx.pushDescriptors && !sb.empty() && sb.size() <= ctx.maxPushDescriptors;
    if (p->pushSamplers) lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &lci, nullptr, &p->samplerSetLayout));
    lci.flags = 0;

    // Validated SPIR-V decorations identify the viewer's active named blocks, including cached programs.
    auto blocks = [&](const std::vector<uint32_t>& words) {
        std::map<uint32_t,uint32_t> sets, bindings;
        for(size_t i=5;i<words.size();)
        {
            const auto count=words[i]>>16, op=words[i]&0xffff;
            if(!count || i+count>words.size()) throw std::runtime_error("Malformed SPIR-V instruction");
            if(op==71 && count==4) // OpDecorate
            {
                if(words[i+2]==34) sets[words[i+1]]=words[i+3];
                if(words[i+2]==33) bindings[words[i+1]]=words[i+3];
            }
            i+=count;
        }
        for(const auto& entry:sets) if(entry.second==2)
        {
            auto it=bindings.find(entry.first);
            if(it==bindings.end() || it->second>=4) throw std::runtime_error("Unsupported named uniform binding");
            p->blockMask |= 1u<<it->second;
        }
    };
    blocks(vsSpirv); blocks(fsSpirv); if(!gsSpirv.empty()) blocks(gsSpirv);
    std::vector<VkDescriptorSetLayoutBinding> bb;
    for(uint32_t i=0;i<4;++i) if(p->blockMask & (1u<<i))
        bb.push_back({i,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_ALL_GRAPHICS,nullptr});
    lci.bindingCount=static_cast<uint32_t>(bb.size()); lci.pBindings=bb.data();
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device,&lci,nullptr,&p->blockSetLayout));
    VkDescriptorSetLayout setLayouts[3] = {p->uniformSetLayout, p->samplerSetLayout, p->blockSetLayout};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 3;
    plci.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &plci, nullptr, &p->pipelineLayout));

    if (p->blockMask)
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = ctx.staticPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p->blockSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device, &ai, &p->blockSet));
        VkDescriptorBufferInfo bi{ctx.ringBuffer, 0, (VkDeviceSize) ctx.uniformBlockLimit};
        std::vector<VkWriteDescriptorSet> writes;
        for (uint32_t i = 0; i < 4; ++i) if (p->blockMask & (1u << i))
        {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = p->blockSet; w.dstBinding = i; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w.pBufferInfo = &bi;
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(ctx.device, (uint32_t) writes.size(), writes.data(), 0, nullptr);
    }
    if (layout.size)
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = ctx.staticPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p->uniformSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device, &ai, &p->uniformSet));
        VkDescriptorBufferInfo bi{ctx.ringBuffer, 0, (VkDeviceSize) layout.size};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = p->uniformSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
    }
    return guard.release();
}

void VkProgram::destroy()
{
    const auto device = ctx->device;
    const auto pool = ctx->staticPool;
    const auto set = uniformSet, bset = blockSet;
    const auto pipeline = pipelineLayout;
    const auto samplers = samplerSetLayout, uniforms = uniformSetLayout, blocks = blockSetLayout;
    const auto vertex = vs, fragment = fs, geometry = gs;
    ctx->deferRelease([device, pool, set, bset, pipeline, samplers, uniforms, blocks, vertex, fragment, geometry, refs = compileRefs] {
        while (refs->load()) std::this_thread::yield();   // a pipeline worker may still be reading the modules and layout
        if (set) vkFreeDescriptorSets(device, pool, 1, &set);
        if (bset) vkFreeDescriptorSets(device, pool, 1, &bset);
        vkDestroyPipelineLayout(device, pipeline, nullptr);
        vkDestroyDescriptorSetLayout(device, samplers, nullptr);
        vkDestroyDescriptorSetLayout(device, uniforms, nullptr);
        vkDestroyDescriptorSetLayout(device, blocks, nullptr);
        vkDestroyShaderModule(device, vertex, nullptr);
        vkDestroyShaderModule(device, fragment, nullptr);
        vkDestroyShaderModule(device, geometry, nullptr);
    });
    delete this;
}

bool VkProgram::setUniform(const char* name, const void* data, uint32_t bytes)
{
    const LLShaderTranslator::BlockMember* m = layout.find(name);
    if (!m || (int) bytes > m->size)
    {
        return false;
    }
    memcpy(shadow.data() + m->offset, data, bytes);
    return true;
}

void VkProgram::prepareDraw()
{
    VkCommandBuffer cmd = ctx->slot().cmd;

    if (!samplerNames.empty())
    {
        if (!ctx->textures)
        {
            fprintf(stderr, "FATAL: a program with samplers needs a texture backend\n");
            abort();
        }
        auto& infos = infoScratch;
        infos.resize(samplerNames.size());
        ctx->textures->beginBarrierBatch();
        for (size_t i = 0; i < samplerNames.size(); ++i)
        {
            ctx->textures->unitDescriptor((uint32_t) samplerUnit[i], samplerKinds[i], samplerTypes[i], infos[i]);
        }
        ctx->textures->flushBarriers(ctx->slot().cmd);
        bool changed = false;
        for (size_t i = 0; i < infos.size(); ++i)
        {
            changed |= infos[i].imageView != lastInfos[i].imageView || infos[i].sampler != lastInfos[i].sampler ||
                       infos[i].imageLayout != lastInfos[i].imageLayout;
        }

        // sampler sets come from a pool that is reset every frame, so a set is only valid in the frame it was made
        if (!pushSamplers && (changed || samplerSetFrame != ctx->frameCounter))
        {
            if (samplerSetCacheFrame != ctx->frameCounter)
            {
                samplerSetCache.clear();
                samplerSetCacheFrame = ctx->frameCounter;
            }
            uint64_t hash = 1469598103934665603ull;
            for (const auto& info : infos)
            {
                for (uint64_t v : {(uint64_t) info.imageView, (uint64_t) info.sampler, (uint64_t) info.imageLayout})
                    hash = (hash ^ v) * 1099511628211ull;
            }
            bool found = false;
            auto range = samplerSetCache.equal_range(hash);
            for (auto it = range.first; it != range.second && !found; ++it)
            {
                const auto& e = it->second.infos;
                bool same = true;
                for (size_t i = 0; i < infos.size() && same; ++i)
                    same = e[i].imageView == infos[i].imageView && e[i].sampler == infos[i].sampler && e[i].imageLayout == infos[i].imageLayout;
                if (same) { samplerSet = it->second.set; found = true; }
            }
            if (!found)
            {
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = ctx->slot().samplerPool;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &samplerSetLayout;
                VK_CHECK(vkAllocateDescriptorSets(ctx->device, &ai, &samplerSet));

                auto& writes = writeScratch;
                writes.resize(samplerNames.size());
                for (size_t i = 0; i < samplerNames.size(); ++i)
                {
                    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    writes[i].dstSet = samplerSet;
                    writes[i].dstBinding = samplerBindings[i];
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[i].pImageInfo = &infos[i];
                }
                vkUpdateDescriptorSets(ctx->device, (uint32_t) writes.size(), writes.data(), 0, nullptr);
                samplerSetCache.emplace(hash, SamplerSetEntry{infos, samplerSet});
                ++ctx->statSamplerSets;
            }
            samplerSetFrame = ctx->frameCounter;
            lastInfos = infos;
        }
    }
    // Reserve enough space before binding descriptors; overflow submits and resets command state.
    if(ctx->ringOffset + (blockMask ? 4*(ctx->uniformBlockLimit+ctx->uboAlignment) : 0) + shadow.size()+ctx->uboAlignment > ctx->slot().ringBase + ctx->ringSize)
    { ctx->flushCommands(VkContext::Stall::UniformRing); ctx->ringOffset=ctx->slot().ringBase; ++ctx->uniformGeneration; }
    uint32_t offsets[4], count = 0, uniformOffset = 0;
    if (blockMask)
    {
        for (uint32_t i = 0; i < 4; ++i) if (blockMask & (1u << i))
        {
            auto& buffer = ctx->uniformBuffers.at(ctx->uniformBindings[i]);
            if (buffer.data.empty()) buffer.data.assign(ctx->uniformBlockLimit, 0);
            if (buffer.frame != ctx->frameCounter || buffer.generation != ctx->uniformGeneration)
            {
                buffer.offset = ctx->uploadRingAlloc(buffer.data.data(), static_cast<uint32_t>(buffer.data.size()));
                buffer.frame = ctx->frameCounter; buffer.generation = ctx->uniformGeneration;
            }
            offsets[count++] = buffer.offset;
        }
    }
    if (layout.size)
    {
        uint32_t offset;
        if (uploadedFrame == ctx->frameCounter && uploadedGeneration == ctx->uniformGeneration && uploadedShadow == shadow)
        {
            offset = uploadedOffset;
        }
        else
        {
            offset = ctx->uploadRingAlloc(shadow.data(), (uint32_t) shadow.size());
            uploadedShadow = shadow;
            uploadedOffset = offset;
            uploadedFrame = ctx->frameCounter;
            uploadedGeneration = ctx->uniformGeneration;
        }
        uniformOffset = offset;
    }

    // Sets stay bound across draws of one program (its layout objects are its own), so bind only what changed.
    auto& dc = ctx->cache();
    if (dc.layout != pipelineLayout)
    {
        dc.layout = pipelineLayout;
        ++ctx->layoutEpoch;
        dc.sets[0] = dc.sets[1] = dc.sets[2] = VK_NULL_HANDLE;
    }
    if (blockMask && (dc.sets[2] != blockSet || dc.blockCount != count || memcmp(dc.blockOffsets, offsets, count * sizeof(uint32_t)) != 0))
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 2, 1, &blockSet, count, offsets);
        dc.sets[2] = blockSet;
        dc.blockCount = count;
        memcpy(dc.blockOffsets, offsets, count * sizeof(uint32_t));
    }
    if (layout.size && (dc.sets[0] != uniformSet || dc.uniformOffset != uniformOffset))
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &uniformSet, 1, &uniformOffset);
        dc.sets[0] = uniformSet;
        dc.uniformOffset = uniformOffset;
    }
    if (pushSamplers)
    {
        // a push set lives in this command buffer until another layout is bound, so push only when the textures changed
        bool differs = pushedSerial != ctx->cmdSerial || pushedEpoch != ctx->layoutEpoch;
        for (size_t i = 0; i < infoScratch.size() && !differs; ++i)
            differs = infoScratch[i].imageView != lastInfos[i].imageView || infoScratch[i].sampler != lastInfos[i].sampler ||
                      infoScratch[i].imageLayout != lastInfos[i].imageLayout;
        if (differs)
        {
            auto& writes = writeScratch;
            writes.resize(samplerNames.size());
            for (size_t i = 0; i < samplerNames.size(); ++i)
            {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstBinding = samplerBindings[i];
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &infoScratch[i];
            }
            ctx->cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, (uint32_t) writes.size(), writes.data());
            ++ctx->statSamplerSets;
            lastInfos = infoScratch;
            pushedSerial = ctx->cmdSerial;
            pushedEpoch = ctx->layoutEpoch;
        }
    }
    else if (!samplerNames.empty() && dc.sets[1] != samplerSet)
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &samplerSet, 0, nullptr);
        dc.sets[1] = samplerSet;
    }

}
