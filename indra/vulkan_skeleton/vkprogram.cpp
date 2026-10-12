#include "vkprogram.h"
#include "vktexturebackend.h"

#include <cstring>

static uint32_t sNextProgramId = 1;

VkProgram* VkProgram::create(VkContext& ctx, const std::vector<uint32_t>& vsSpirv, const std::vector<uint32_t>& fsSpirv,
                             const LLShaderTranslator::BlockLayout& layout,
                             const std::vector<LLShaderTranslator::SamplerBinding>& samplers)
{
    VkProgram* p = new VkProgram();
    p->ctx = &ctx;
    p->id = sNextProgramId++;
    p->vs = ctx.createShaderModule(vsSpirv.data(), vsSpirv.size() * 4);
    p->fs = ctx.createShaderModule(fsSpirv.data(), fsSpirv.size() * 4);
    p->layout = layout;
    p->shadow.assign((size_t) layout.size, 0);
    p->samplerNames.resize(samplers.size());
    p->samplerTypes.resize(samplers.size());
    for (const auto& s : samplers)
    {
        p->samplerNames[s.binding] = s.name;
        p->samplerTypes[s.binding] = s.type;
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
        sb[i].binding = (uint32_t) i;
        sb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb[i].descriptorCount = 1;
        sb[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    }
    lci.bindingCount = (uint32_t) sb.size();
    lci.pBindings = sb.data();
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &lci, nullptr, &p->samplerSetLayout));

    VkDescriptorSetLayout setLayouts[2] = {p->uniformSetLayout, p->samplerSetLayout};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &plci, nullptr, &p->pipelineLayout));

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
    return p;
}

void VkProgram::destroy()
{
    vkDeviceWaitIdle(ctx->device);
    if (uniformSet)
    {
        vkFreeDescriptorSets(ctx->device, ctx->staticPool, 1, &uniformSet);
    }
    vkDestroyPipelineLayout(ctx->device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx->device, samplerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx->device, uniformSetLayout, nullptr);
    vkDestroyShaderModule(ctx->device, vs, nullptr);
    vkDestroyShaderModule(ctx->device, fs, nullptr);
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
    VkCommandBuffer cmd = ctx->cmd;

    if (layout.size)
    {
        const uint32_t offset = ctx->uploadRingAlloc(shadow.data(), (uint32_t) shadow.size());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &uniformSet, 1, &offset);
    }

    if (!samplerNames.empty())
    {
        std::vector<VkDescriptorImageInfo> infos(samplerNames.size());
        for (size_t i = 0; i < samplerNames.size(); ++i)
        {
            if (!ctx->textures)
            {
                fprintf(stderr, "FATAL: a program with samplers needs a texture backend\n");
                abort();
            }
            ctx->textures->unitDescriptor((uint32_t) samplerUnit[i], samplerTypes[i], infos[i]);
        }
        bool changed = false;
        for (size_t i = 0; i < infos.size(); ++i)
        {
            changed |= infos[i].imageView != lastInfos[i].imageView || infos[i].sampler != lastInfos[i].sampler;
        }

        // sampler sets come from a pool that is reset every frame, so a set is only valid in the frame it was made
        if (changed || samplerSetFrame != ctx->frameCounter)
        {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = ctx->framePool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &samplerSetLayout;
            VK_CHECK(vkAllocateDescriptorSets(ctx->device, &ai, &samplerSet));

            std::vector<VkWriteDescriptorSet> writes(samplerNames.size());
            for (size_t i = 0; i < samplerNames.size(); ++i)
            {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = samplerSet;
                writes[i].dstBinding = (uint32_t) i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(ctx->device, (uint32_t) writes.size(), writes.data(), 0, nullptr);
            samplerSetFrame = ctx->frameCounter;
            lastInfos = infos;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &samplerSet, 0, nullptr);
    }
}
