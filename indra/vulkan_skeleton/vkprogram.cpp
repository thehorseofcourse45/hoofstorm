#include "vkprogram.h"

#include <cstring>

static uint32_t sNextProgramId = 1;

VkTexture createTexture(VkContext& ctx, uint32_t width, uint32_t height, const uint8_t* rgba, bool linearFilter)
{
    VkTexture tex;
    const VkDeviceSize bytes = (VkDeviceSize) width * height * 4;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device, &ici, nullptr, &tex.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.device, tex.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &tex.memory));
    VK_CHECK(vkBindImageMemory(ctx.device, tex.image, tex.memory, 0));

    // staging buffer
    VkBuffer staging;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &staging));
    vkGetBufferMemoryRequirements(ctx.device, staging, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &stagingMemory));
    VK_CHECK(vkBindBufferMemory(ctx.device, staging, stagingMemory, 0));
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(ctx.device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
    memcpy(mapped, rgba, (size_t) bytes);
    vkUnmapMemory(ctx.device, stagingMemory);

    // one-time command buffer: undefined -> transfer dst, copy, -> shader read
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = ctx.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));
    vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &cmd);
    vkDestroyBuffer(ctx.device, staging, nullptr);
    vkFreeMemory(ctx.device, stagingMemory, nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = tex.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ici.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &tex.view));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &sci, nullptr, &tex.sampler));
    return tex;
}

void destroyTexture(VkContext& ctx, VkTexture& tex)
{
    vkDeviceWaitIdle(ctx.device);
    vkDestroySampler(ctx.device, tex.sampler, nullptr);
    vkDestroyImageView(ctx.device, tex.view, nullptr);
    vkDestroyImage(ctx.device, tex.image, nullptr);
    vkFreeMemory(ctx.device, tex.memory, nullptr);
    tex = VkTexture();
}

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
    for (const auto& s : samplers)
    {
        p->samplerNames[s.binding] = s.name;
    }
    p->textures.assign(samplers.size(), nullptr);

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

bool VkProgram::setTexture(const char* samplerName, const VkTexture* texture)
{
    for (size_t i = 0; i < samplerNames.size(); ++i)
    {
        if (samplerNames[i] == samplerName)
        {
            if (textures[i] != texture)
            {
                textures[i] = texture;
                texturesDirty = true;
            }
            return true;
        }
    }
    return false;
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
        // sampler sets come from a pool that is reset every frame, so a set is only valid in the frame it was made
        if (texturesDirty || samplerSetFrame != ctx->frameCounter)
        {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = ctx->framePool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &samplerSetLayout;
            VK_CHECK(vkAllocateDescriptorSets(ctx->device, &ai, &samplerSet));

            std::vector<VkDescriptorImageInfo> infos(samplerNames.size());
            std::vector<VkWriteDescriptorSet> writes(samplerNames.size());
            for (size_t i = 0; i < samplerNames.size(); ++i)
            {
                if (!textures[i])
                {
                    fprintf(stderr, "FATAL: sampler '%s' has no texture bound\n", samplerNames[i].c_str());
                    abort();
                }
                infos[i] = {textures[i]->sampler, textures[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = samplerSet;
                writes[i].dstBinding = (uint32_t) i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(ctx->device, (uint32_t) writes.size(), writes.data(), 0, nullptr);
            samplerSetFrame = ctx->frameCounter;
            texturesDirty = false;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &samplerSet, 0, nullptr);
    }
}
