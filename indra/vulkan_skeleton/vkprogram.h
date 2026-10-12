// Vulkan skeleton: a translated viewer program (shader modules, descriptor layouts, uniform block shadow) and textures.
// Standalone test program (not part of the viewer build). See doc/vulkan/shader-layer-design.md.
#pragma once

#include "vkcontext.h"
#include "llshadertranslator.h"

#include <map>
#include <string>
#include <vector>

struct VkTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

// RGBA8 (UNORM, no mipmaps) texture filled through a staging buffer. Blocks until the upload is finished.
VkTexture createTexture(VkContext& ctx, uint32_t width, uint32_t height, const uint8_t* rgba, bool linearFilter);
void destroyTexture(VkContext& ctx, VkTexture& tex);

// What LLGLSLShader holds for GL, for Vulkan:
//   set 0, binding 0: the program's uniform block (std140), a copy of 'shadow' is put in the context's ring buffer per draw
//   set 1, binding N: one combined image sampler per sampler declared in the program
struct VkProgram
{
    VkContext* ctx = nullptr;
    uint32_t id = 0;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout uniformSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet uniformSet = VK_NULL_HANDLE;

    LLShaderTranslator::BlockLayout layout;
    std::vector<uint8_t> shadow;                        // CPU copy of the uniform block (LLGLSLShader's value cache, in effect)
    std::vector<std::string> samplerNames;              // index = descriptor binding
    std::vector<const VkTexture*> textures;             // what each sampler currently samples
    VkDescriptorSet samplerSet = VK_NULL_HANDLE;
    uint64_t samplerSetFrame = 0;
    bool texturesDirty = true;

    // 'vsSpirv'/'fsSpirv' are the modules from LLShaderCompiler (or any SPIR-V that follows the same set/binding convention).
    static VkProgram* create(VkContext& ctx, const std::vector<uint32_t>& vsSpirv, const std::vector<uint32_t>& fsSpirv,
                             const LLShaderTranslator::BlockLayout& layout,
                             const std::vector<LLShaderTranslator::SamplerBinding>& samplers);
    void destroy();

    // false if the program has no such uniform / sampler
    bool setUniform(const char* name, const void* data, uint32_t bytes);
    bool setTexture(const char* samplerName, const VkTexture* texture);

    // Called before every draw: snapshot the uniform block into the ring buffer and bind the descriptor sets.
    void prepareDraw();
};
