// Overlap stress: every frame rewrites the vertex buffer, a texture and a uniform with frame-unique values and renders into its own
// offscreen target, after a heavy draw load that keeps the GPU behind the CPU. With two slots the next frame is recorded while the
// previous one still runs. Pixels are checked at the end, so any resource reused too early corrupts an earlier frame.
#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include <algorithm>
#include <cmath>
#include <cstring>

int main(int argc, char** argv)
{
    const int heavy = argc > 1 ? atoi(argv[1]) : 1500;
    constexpr int kFrames = 24;
    VkContext ctx;
    unsigned checks = 0;
    auto check = [&](bool ok, const char* label) { if (!ok) throw std::runtime_error(label); ++checks; printf("PASS %s\n", label); };
    try
    {
        check(ctx.init(GetModuleHandle(nullptr), nullptr, true) && ctx.validation, "validated GPU initialized");
        printf("FRAMES_IN_FLIGHT=%zu\n", ctx.slotCount);
        {
            VulkanRenderStateBackend state; VulkanVertexBufferBackend vb(&ctx, &state); VulkanShaderBackend shaders(&ctx);
            VulkanTextureBackend textures(&ctx); VulkanFramebufferBackend fb(&ctx, &state);
            const char* sources[] = {
                "#version 330\nin vec4 position;void main(){gl_Position=position;}",
                "#version 330\nuniform sampler2D diffuseMap;\nuniform float marker;\nout vec4 color;void main(){color=texture(diffuseMap,vec2(.5))+vec4(marker,0,0,0);}"};
            U32 program = shaders.createProgram();
            for (int i = 0; i < 2; ++i)
            {
                U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                check(shaders.compileObject(object, 1, &sources[i], log), "shader accepted"); shaders.attachObject(program, object);
            }
            shaders.bindAttribLocation(program, 0, "position");
            check(shaders.link(program), "shader links"); shaders.use(program);
            shaders.uniform1i(program, shaders.getUniformLocation(program, "diffuseMap"), 0);
            const S32 marker = shaders.getUniformLocation(program, "marker");
            state.setDepthTest(false); state.setCapability(0x0B44, false); state.setViewport(0, 0, 800, 600);

            U32 source; textures.generate(1, &source); textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, source);
            textures.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 128, 128, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
            textures.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);

            // left triangle, right triangle, full-screen triangle
            const float positions[] = {-1,-1,.5f,1, 0,-1,.5f,1, -1,1,.5f,1,   0,-1,.5f,1, 1,-1,.5f,1, 1,1,.5f,1,   -1,-1,.5f,1, 3,-1,.5f,1, -1,3,.5f,1};
            BufferHandle vertices; U8* shadow = nullptr; vb.allocate(BufferKind::Vertex, sizeof(positions), vertices, shadow);
            U32 offsets[14]{}; vb.setBuffers(vertices, {}, 1, offsets);

            U32 scratchTexture; textures.generate(1, &scratchTexture); textures.bind(glc::TEXTURE_2D, scratchTexture);
            textures.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 800, 600, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
            U32 scratch = fb.createFramebuffer(); fb.bind(scratch); fb.attach(scratch, 0x8CE0, glc::TEXTURE_2D, scratchTexture, 0); fb.bind(0);
            U32 targets[kFrames], target[kFrames];
            for (int k = 0; k < kFrames; ++k)
            {
                textures.generate(1, &targets[k]); textures.bind(glc::TEXTURE_2D, targets[k]);
                textures.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 800, 600, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
                target[k] = fb.createFramebuffer(); fb.bind(target[k]); fb.attach(target[k], 0x8CE0, glc::TEXTURE_2D, targets[k], 0); fb.bind(0);
            }
            auto colorOf = [](int k, uint8_t c[4]) { c[0] = 0; c[1] = uint8_t((k * 7) % 256); c[2] = uint8_t((k * 11) % 256); c[3] = 255; };
            const float clear[4] = {0, 0, 0, 1};
            int overlapped = 0;
            for (int k = 0; k < kFrames; ++k)
            {
                check(ctx.beginFrame(clear), "frame acquired");
                ctx.refreshCompleted();
                if (k > 0 && ctx.completedFrame + 1 < ctx.frameCounter) ++overlapped;   // previous frame still on the GPU
                // the probe triangle always sits at vertices 0-2; its side depends on the frame, so a stale buffer shows up as wrong pixels
                float frameVertices[sizeof(positions) / sizeof(float)];
                memcpy(frameVertices, positions, sizeof(positions));
                if (k % 2) memcpy(frameVertices, positions + 12, 12 * sizeof(float));
                vb.upload(BufferKind::Vertex, vertices, 0, sizeof(positions) - 1, frameVertices);
                std::vector<uint8_t> pixels(128 * 128 * 4); uint8_t c[4]; colorOf(k, c);
                for (int i = 0; i < 128 * 128; ++i) memcpy(&pixels[i * 4], c, 4);
                textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, source);
                textures.subImage2D(glc::TEXTURE_2D, 0, 0, 0, 128, 128, glc::RGBA, glc::UNSIGNED_BYTE, pixels.data());
                shaders.uniform1f(program, marker, float((k + 1) * 5) / 255.f);
                fb.bind(scratch); for (int i = 0; i < heavy; ++i) vb.drawArrays(0, 6, 3);
                fb.bind(target[k]); fb.setClearColor(0, 0, 0, 1); fb.clear(0x4000);
                vb.drawArrays(0, 0, 3);
                fb.bind(0);
                ctx.endFrame(false);
            }
            printf("OVERLAPPED_FRAMES=%d of %d heavy=%d\n", overlapped, kFrames - 1, heavy);
            if (ctx.slotCount > 1) check(overlapped >= kFrames / 2, "frames really overlap on the GPU");
            else check(overlapped == 0, "one slot never overlaps");
            // read everything back with one synchronous frame
            check(ctx.beginFrame(clear), "readback frame acquired");
            bool ok = true;
            for (int k = 0; k < kFrames; ++k)
            {
                fb.bind(target[k]);
                uint8_t left[4]{}, right[4]{}, expect[4]; colorOf(k, expect); expect[0] = uint8_t((k + 1) * 5);
                fb.readPixels(200, 150, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, left);
                fb.readPixels(600, 150, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, right);
                const uint8_t black[4] = {0, 0, 0, 255};
                const uint8_t* wantLeft = (k % 2) ? black : expect;
                const uint8_t* wantRight = (k % 2) ? expect : black;
                if (memcmp(left, wantLeft, 4) || memcmp(right, wantRight, 4))
                {
                    printf("MISMATCH frame %d left=%u,%u,%u,%u right=%u,%u,%u,%u\n", k, left[0], left[1], left[2], left[3], right[0], right[1], right[2], right[3]);
                    ok = false;
                }
            }
            fb.bind(0);
            ctx.endFrame(false);
            check(ok, "every overlapped frame kept its own vertices, texture and uniform");
            // ---- asynchronous pipeline compilation
            {
                ctx.asyncPipelines = true;
                U32 variant = shaders.createProgram();
                for (int i = 0; i < 2; ++i)
                {
                    U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                    // a different fragment body gives a different program, so its pipeline is not cached yet
                    const char* variantSources[] = {sources[0], "#version 330\nuniform float marker;\nout vec4 color;void main(){color=vec4(0.,marker,0.,1.);}"};
                    check(shaders.compileObject(object, 1, &variantSources[i], log), "variant shader accepted"); shaders.attachObject(variant, object);
                }
                shaders.bindAttribLocation(variant, 0, "position");
                check(shaders.link(variant), "variant links"); shaders.use(variant);
                const S32 variantMarker = shaders.getUniformLocation(variant, "marker");
                auto drawVariant = [&]() {
                    check(ctx.beginFrame(clear), "async frame acquired");
                    shaders.uniform1f(variant, variantMarker, 1.0f);
                    fb.bind(target[0]); fb.setClearColor(0, 0, 0, 1); fb.clear(0x4000);
                    const auto skipped = ctx.pipelineDrawsSkipped;
                    vb.drawArrays(0, 6, 3);
                    uint8_t px[4]{}; fb.readPixels(300, 300, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, px);
                    fb.bind(0); ctx.endFrame(false);
                    return std::make_pair(ctx.pipelineDrawsSkipped != skipped, px[1]);
                };
                auto first = drawVariant();
                check(first.first && first.second == 0, "first draw is skipped while the pipeline compiles");
                bool drawn = false;
                for (int i = 0; i < 600 && !drawn; ++i) { Sleep(10); auto r = drawVariant(); drawn = !r.first && r.second == 255; }
                check(drawn, "draw appears once the worker has built the pipeline");
                // a program deleted while its pipeline is still building must not be pulled out from under the worker
                U32 doomed = shaders.createProgram();
                for (int i = 0; i < 2; ++i)
                {
                    U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                    const char* doomedSources[] = {sources[0], "#version 330\nout vec4 color;void main(){color=vec4(.25);}"};
                    check(shaders.compileObject(object, 1, &doomedSources[i], log), "doomed shader accepted"); shaders.attachObject(doomed, object);
                }
                shaders.bindAttribLocation(doomed, 0, "position");
                check(shaders.link(doomed), "doomed program links"); shaders.use(doomed);
                check(ctx.beginFrame(clear), "doomed frame acquired");
                const auto skippedBefore = ctx.pipelineDrawsSkipped;
                vb.drawArrays(0, 6, 3);
                check(ctx.pipelineDrawsSkipped > skippedBefore, "doomed program's draw queued a build");
                shaders.use(0); shaders.deleteProgramAndObjects(doomed);
                ctx.endFrame(false);
                for (int i = 0; i < 4; ++i) { check(ctx.beginFrame(clear), "frame after deleting a compiling program"); ctx.endFrame(false); }
                VK_CHECK(vkDeviceWaitIdle(ctx.device)); ctx.collectReleasedResources();
                ctx.asyncPipelines = false;
            }
            // After a restore from minimized the first call of a frame may be a framebuffer copy or read: each starts the frame
            // itself and keeps the bound framebuffer (it used to abort with "framebuffer copies require an active frame").
            {
                check(ctx.beginFrame(clear), "frame for the red scratch target");
                fb.bind(scratch); state.setColorMask(true, true, true, true); fb.setClearColor(1, 0, 0, 1); fb.clear(0x4000); fb.bind(0);
                ctx.endFrame(false);
                fb.bind(scratch); textures.bind(glc::TEXTURE_2D, source);
                fb.copyToTexture(glc::TEXTURE_2D, 0, 0, 0, 0, 0, 0, 16, 16);
                check(ctx.recording && fb.getBound() == scratch, "a copy outside a frame begins one and keeps the bound framebuffer");
                ctx.endFrame(false);
                fb.bind(scratch); uint8_t px[4]{};
                fb.readPixels(4, 4, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, px);
                check(ctx.recording && px[0] == 255 && px[1] == 0, "a read outside a frame begins one and reads the bound framebuffer");
                fb.bind(0); ctx.endFrame(false);
                VK_CHECK(vkDeviceWaitIdle(ctx.device));
            }
            // FSR 2 temporal upscale through the backend entry (dispatchRenderBackendFsr2 forwards to fb.dispatchFsr2): a gradient
            // rendered at render size is upscaled into a display-size storage texture inside the frame, and drawing goes on after it.
            {
                constexpr U32 RW = 400, RH = 300, DW = 800, DH = 600;
                auto makeTexture = [&](U32 internal, U32 format, U32 w, U32 h, bool fsrImage) {
                    U32 name; textures.generate(1, &name);
                    if (fsrImage) textures.setStorageImage(name);     // one mip level, plus storage usage where the format has it
                    textures.bind(glc::TEXTURE_2D, name);
                    textures.image2D(glc::TEXTURE_2D, 0, internal, w, h, format, glc::FLOAT, nullptr);
                    return name;
                };
                const U32 color = makeTexture(glc::RGBA16F, glc::RGBA, RW, RH, true);
                const U32 depth = makeTexture(glc::DEPTH_COMPONENT32F, glc::DEPTH_COMPONENT, RW, RH, true);
                const U32 motion = makeTexture(glc::RG16F, glc::RG, RW, RH, true);
                const U32 output = makeTexture(glc::RGBA16F, glc::RGBA, DW, DH, true);
                const U32 mipped = makeTexture(glc::RGBA16F, glc::RGBA, RW, RH, false);
                const U32 srgb = makeTexture(glc::SRGB8_ALPHA8, glc::RGBA, DW, DH, true);
                VulkanTextureBackend::ImageInfo info;
                check(textures.imageInfo(output, info) && info.levels == 1 && info.layers == 1 && info.storage &&
                      info.format == VK_FORMAT_R16G16B16A16_SFLOAT, "opt-in texture has one level and storage usage");
                check(textures.imageInfo(mipped, info) && info.levels > 1 && !info.storage, "other textures keep their mip chain, no storage");
                check(textures.imageInfo(depth, info) && info.levels == 1 && info.format == VK_FORMAT_D32_SFLOAT, "opt-in depth has one level");
                U32 renderFb = fb.createFramebuffer(); fb.bind(renderFb);
                fb.attach(renderFb, 0x8CE0, glc::TEXTURE_2D, color, 0); fb.attach(renderFb, 0x8D00, glc::TEXTURE_2D, depth, 0); fb.bind(0);
                U32 outFb = fb.createFramebuffer(); fb.bind(outFb); fb.attach(outFb, 0x8CE0, glc::TEXTURE_2D, output, 0); fb.bind(0);

                U32 gradient = shaders.createProgram();
                const char* gradientSources[] = {
                    "#version 330\nin vec4 position;\nout vec2 uv;\nvoid main(){uv=position.xy*0.5+0.5;gl_Position=position;}",
                    "#version 330\nin vec2 uv;\nout vec4 color;\nvoid main(){color=vec4(uv.x,uv.y,0.25,1.0);}"};
                for (int i = 0; i < 2; ++i)
                {
                    U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                    check(shaders.compileObject(object, 1, &gradientSources[i], log), "gradient shader accepted"); shaders.attachObject(gradient, object);
                }
                shaders.bindAttribLocation(gradient, 0, "position");
                check(shaders.link(gradient), "gradient links"); shaders.use(gradient);

                RenderBackendFsr2Params p;
                p.colorTex = color; p.depthTex = depth; p.motionTex = motion; p.outputTex = output;
                p.renderW = RW; p.renderH = RH; p.displayW = DW; p.displayH = DH;
                p.frameTimeMs = 16.7f; p.reset = true;
                check(ctx.beginFrame(clear), "FSR 2 frame acquired");
                fb.bind(renderFb); state.setViewport(0, 0, RW, RH); fb.setClearColor(0, 0, 0, 1); fb.clear(0x4000);
                vb.drawArrays(0, 6, 3);
                check(fb.isRendering(), "gradient pass is open before the upscale");
                RenderBackendFsr2Params bad = p; bad.colorTex = mipped;
                check(!fb.dispatchFsr2(bad) && fb.isRendering() && ctx.upscalerMemBytes == 0, "multi-mip input refused, nothing recorded");
                bad = p; bad.outputTex = srgb;
                check(!fb.dispatchFsr2(bad) && ctx.upscalerMemBytes == 0 && !fb.fsr2Failed(), "sRGB output refused; two failures do not trip the rule");
                check(fb.dispatchFsr2(p), "FSR 2 dispatch accepted");
                check(!fb.isRendering() && ctx.upscalerMemBytes > 0, "the upscale ended dynamic rendering; upscaler VRAM counted");
                bool tracked = true;
                for (U32 t : {color, depth, motion, output})
                    tracked = textures.imageInfo(t, info) && info.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && tracked;
                check(tracked, "inputs and output tracked as SHADER_READ_ONLY_OPTIMAL");
                vb.drawArrays(0, 6, 3);
                check(fb.isRendering(), "the next draw reopens rendering on the bound target");
                RenderBackendFsr2Params smaller = p; smaller.renderW = 320; smaller.renderH = 240;
                check(fb.dispatchFsr2(smaller) && ctx.upscalerMemBytes > 0, "a new render size recreates the upscaler mid-frame");
                fb.bind(target[1]); state.setViewport(0, 0, 800, 600); fb.setClearColor(0, 1, 0, 1); fb.clear(0x4000);
                vb.drawArrays(0, 6, 3);
                uint8_t px[4]{}; fb.readPixels(100, 100, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, px);
                printf("  after upscale: target pixel %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
                check(px[0] >= 28 && px[0] <= 36 && px[2] >= 60 && px[2] <= 68 && px[3] == 255, "drawing into the next target works after the upscale");
                fb.bind(outFb); float out[4]{};
                fb.readPixels(DW / 2, DH / 2, 1, 1, glc::RGBA, glc::FLOAT, out);
                printf("  upscaled centre %.3f %.3f %.3f %.3f\n", out[0], out[1], out[2], out[3]);
                check(std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && out[0] > 0.2f && out[0] < 0.8f &&
                      out[2] > 0.15f && out[2] < 0.35f, "display-size output holds the upscaled gradient");
                fb.bind(0); ctx.endFrame(false);
                fb.releaseFsr2();
                check(ctx.upscalerMemBytes == 0, "release frees the upscaler and zeroes its VRAM count");

                // three failures in a row trip the rule; then nothing is attempted until a release
                check(ctx.beginFrame(clear), "FSR 2 failure frame acquired");
                ctx.failFsr2Create = true;
                bool refused = !fb.dispatchFsr2(p) && !fb.dispatchFsr2(p) && !fb.fsr2Failed();
                refused = !fb.dispatchFsr2(p) && refused;
                check(refused && fb.fsr2Failed(), "three consecutive failures trip the failure rule");
                ctx.failFsr2Create = false;
                check(!fb.dispatchFsr2(p) && ctx.upscalerMemBytes == 0, "after the rule trips no dispatch is attempted");
                fb.releaseFsr2();
                check(!fb.fsr2Failed() && fb.dispatchFsr2(p), "release clears the failure state");
                ctx.endFrame(false);
                fb.releaseFsr2();
                check(ctx.upscalerMemBytes == 0, "upscaler VRAM back to zero");
                shaders.use(0);
                fb.destroyFramebuffer(renderFb); fb.destroyFramebuffer(outFb);
                const U32 names[] = {color, depth, motion, output, mipped, srgb};
                textures.destroy(6, names);
                VK_CHECK(vkDeviceWaitIdle(ctx.device));
            }
            check(ctx.validationErrors == 0 && ctx.validationWarnings == 0, "zero Vulkan validation messages");
        }
        printf("FRAMES_CHECKS=%u PASS\n", checks);
        return 0;
    }
    catch (const std::exception& e)
    {
        printf("FAIL: %s\n", e.what());
        return 1;
    }
}
