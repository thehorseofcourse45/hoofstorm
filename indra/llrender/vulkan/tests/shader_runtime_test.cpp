// Exercise the shader implementation compiled with the actual viewer toolchain, without launching the viewer.
#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include <exception>

bool test_shaders(VkContext&, VulkanRenderStateBackend&, VulkanVertexBufferBackend&,
                  VulkanShaderBackend&, VulkanFramebufferBackend&);

int main()
{
    VkContext ctx;
    bool passed = false;
    try
    {
        if (!ctx.init(GetModuleHandle(nullptr), nullptr, true) || !ctx.validation) return 1;
        {
            VulkanRenderStateBackend state;
            VulkanVertexBufferBackend vertices(&ctx, &state);
            VulkanShaderBackend shaders(&ctx);
            VulkanTextureBackend textures(&ctx);
            VulkanFramebufferBackend framebuffers(&ctx, &state);
            passed = test_shaders(ctx, state, vertices, shaders, framebuffers);
            VK_CHECK(vkDeviceWaitIdle(ctx.device));
        }
        ctx.shutdown();
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "shader runtime test: %s\n", error.what());
        return 1;
    }
    printf("validation: %u errors, %u warnings\n", ctx.validationErrors, ctx.validationWarnings);
    return passed && ctx.validationErrors == 0 && ctx.validationWarnings == 0 ? 0 : 1;
}
