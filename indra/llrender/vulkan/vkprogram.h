// Shared Vulkan runtime: a translated viewer program (shader modules, descriptor layouts, uniform block shadow) and textures.
// See doc/vulkan/shader-layer-design.md.
#pragma once

#include "vkcontext.h"
#include "llshadertranslator.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// What LLGLSLShader holds for GL, for Vulkan:
//   set 0, binding 0: the program's uniform block (std140), a copy of 'shadow' is put in the context's ring buffer per draw
//   set 1, binding N: one combined image sampler per sampler declared in the program
struct VkProgram
{
    VkContext* ctx = nullptr;
    uint32_t id = 0;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, gs = VK_NULL_HANDLE;
    VkDescriptorSetLayout uniformSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout blockSetLayout = VK_NULL_HANDLE;
    uint32_t blockMask = 0;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet uniformSet = VK_NULL_HANDLE;
    VkDescriptorSet blockSet = VK_NULL_HANDLE;          // set 2: dynamic uniform buffers over the ring, written once; the offsets change per draw

    LLShaderTranslator::BlockLayout layout;
    std::vector<uint8_t> shadow;                        // CPU copy of the uniform block (LLGLSLShader's value cache, in effect)
    std::vector<std::string> samplerNames;              // index = descriptor binding
    std::vector<int> samplerKinds;                      // VulkanTextureBackend::samplerKind of each sampler
    std::vector<std::string> samplerTypes;              // GLSL type of each sampler ("sampler2D", "samplerCube", ...)
    std::vector<uint32_t> samplerBindings;              // active samplers can leave gaps in SPIR-V bindings
    std::vector<int> samplerUnit;                       // texture unit each sampler reads (the value of a GL sampler uniform)
    std::vector<VkDescriptorImageInfo> lastInfos;       // what the current sampler set was written with
    VkDescriptorSet samplerSet = VK_NULL_HANDLE;
    bool pushSamplers = false;                          // sampler set is a push descriptor set
    uint64_t pushedSerial = ~0ull, pushedEpoch = ~0ull; // command buffer and layout epoch lastInfos was pushed in
    uint64_t samplerSetFrame = 0;
    std::vector<uint8_t> uploadedShadow;                // what the ring holds from this frame, to skip a copy when the next draw is identical
    uint64_t uploadedFrame = ~0ull, uploadedGeneration = 0;
    uint32_t uploadedOffset = 0;
    // Sampler sets written this frame, by hash of their image infos: a draw whose textures were seen before binds the old set.
    struct SamplerSetEntry { std::vector<VkDescriptorImageInfo> infos; VkDescriptorSet set; };
    std::unordered_multimap<uint64_t, SamplerSetEntry> samplerSetCache;
    uint64_t samplerSetCacheFrame = 0;
    std::vector<VkDescriptorImageInfo> infoScratch;
    std::vector<VkWriteDescriptorSet> writeScratch;
    std::shared_ptr<std::atomic<int>> compileRefs = std::make_shared<std::atomic<int>>(0);   // pipeline jobs still reading the modules

    // 'vsSpirv'/'fsSpirv' are the modules from LLShaderCompiler (or any SPIR-V that follows the same set/binding convention).
    static VkProgram* create(VkContext& ctx, const std::vector<uint32_t>& vsSpirv, const std::vector<uint32_t>& fsSpirv,
                             const LLShaderTranslator::BlockLayout& layout,
                             const std::vector<LLShaderTranslator::SamplerBinding>& samplers,
                             const std::vector<uint32_t>& gsSpirv = {});
    void destroy();

    // false if the program has no such uniform
    bool setUniform(const char* name, const void* data, uint32_t bytes);

    // Called before every draw: snapshot the uniform block into the ring buffer and bind the descriptor sets.
    void prepareDraw();
};
