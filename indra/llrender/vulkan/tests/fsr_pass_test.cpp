// Runs the FSR EASU shader through the Vulkan backend: a 32x32 horizontal ramp is upscaled into a 64x64 target.
// usage: llvulkan_fsr_pass_test <directory holding fsrEasuF.glsl>
#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include <cstring>
#include <fstream>
#include <sstream>

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: llvulkan_fsr_pass_test <shader dir>\n"); return 2; }
    std::ifstream in(std::string(argv[1]) + "/fsrEasuF.glsl", std::ios::binary);
    std::stringstream body; body << in.rdbuf();
    if (!in) { printf("FAIL: cannot read fsrEasuF.glsl\n"); return 1; }

    VkContext ctx;
    unsigned checks = 0;
    auto check = [&](bool ok, const char* label) { if (!ok) throw std::runtime_error(label); ++checks; printf("PASS %s\n", label); };
    try
    {
        check(ctx.init(GetModuleHandle(nullptr), nullptr, true) && ctx.validation, "validated GPU initialized");
        {
            VulkanRenderStateBackend state; VulkanVertexBufferBackend vb(&ctx, &state); VulkanShaderBackend shaders(&ctx);
            VulkanTextureBackend textures(&ctx); VulkanFramebufferBackend fb(&ctx, &state);
            const std::string sources[] = {
                "#version 330\nin vec4 position;\nout vec2 vary_fragcoord;\n"
                "void main(){gl_Position=position;vary_fragcoord=position.xy*0.5+0.5;}\n",
                "#version 330\n" + body.str()};
            U32 program = shaders.createProgram();
            for (int i = 0; i < 2; ++i)
            {
                const char* text = sources[i].c_str(); std::string log;
                U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31);
                check(shaders.compileObject(object, 1, &text, log), "shader accepted"); shaders.attachObject(program, object);
            }
            shaders.bindAttribLocation(program, 0, "position");
            check(shaders.link(program), "shader links"); shaders.use(program);
            shaders.uniform1i(program, shaders.getUniformLocation(program, "diffuseRect"), 0);
            shaders.uniform2f(program, shaders.getUniformLocation(program, "fsr_input_size"), 32.f, 32.f);
            shaders.uniform2f(program, shaders.getUniformLocation(program, "fsr_output_size"), 64.f, 64.f);
            state.setDepthTest(false); state.setCapability(0x0B44, false); state.setViewport(0, 0, 64, 64);

            std::vector<uint8_t> ramp(32 * 32 * 4);
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) { uint8_t* p = &ramp[(y * 32 + x) * 4]; p[0] = uint8_t(x * 8); p[1] = p[2] = 64; p[3] = 255; }
            U32 source; textures.generate(1, &source); textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, source);
            textures.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 32, 32, glc::RGBA, glc::UNSIGNED_BYTE, ramp.data());
            textures.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
            textures.setWrap(glc::TEXTURE_2D, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE);     // render targets clamp

            U32 targetTexture; textures.generate(1, &targetTexture); textures.bind(glc::TEXTURE_2D, targetTexture);
            textures.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 64, 64, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
            U32 target = fb.createFramebuffer(); fb.bind(target); fb.attach(target, 0x8CE0, glc::TEXTURE_2D, targetTexture, 0); fb.bind(0);

            const float positions[] = {-1,-1,.5f,1, 3,-1,.5f,1, -1,3,.5f,1};
            BufferHandle vertices; U8* shadow = nullptr; vb.allocate(BufferKind::Vertex, sizeof(positions), vertices, shadow);
            U32 offsets[14]{}; vb.setBuffers(vertices, {}, 1, offsets);
            vb.upload(BufferKind::Vertex, vertices, 0, sizeof(positions) - 1, positions);

            const float clear[4] = {0, 0, 0, 1};
            check(ctx.beginFrame(clear), "frame acquired");
            textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, source);
            fb.bind(target); fb.setClearColor(0, 0, 0, 1); fb.clear(0x4000);
            vb.drawArrays(0, 0, 3);
            uint8_t left[4]{}, right[4]{}, midA[4]{}, midB[4]{};
            fb.readPixels(2, 32, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, left);
            fb.readPixels(61, 32, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, right);
            fb.readPixels(30, 32, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, midA);
            fb.readPixels(34, 32, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, midB);
            for (int x = 0; x < 64; x += 4) { uint8_t q[4]{}; fb.readPixels(x, 32, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, q); printf("x=%d r=%u ", x, q[0]); }
            printf("\n");
            fb.bind(0); ctx.endFrame(false);
            VK_CHECK(vkDeviceWaitIdle(ctx.device));
            printf("left=%u right=%u midA=%u midB=%u\n", left[0], right[0], midA[0], midB[0]);
            check(right[0] > left[0] + 100, "the ramp survives the upscale (right is much brighter than left)");
            check(midB[0] >= midA[0], "the ramp rises left to right");
            check(left[1] > 0 && left[3] == 255, "other channels and alpha are written");
            check(ctx.validationErrors == 0 && ctx.validationWarnings == 0, "zero Vulkan validation messages");
        }
        printf("FSR_PASS_CHECKS=%u PASS\n", checks);
        return 0;
    }
    catch (const std::exception& e)
    {
        printf("FAIL: %s\n", e.what());
        return 1;
    }
}
