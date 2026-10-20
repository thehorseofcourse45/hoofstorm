// Overlap stress: every frame rewrites the vertex buffer, a texture and a uniform with frame-unique values and renders into its own
// offscreen target, after a heavy draw load that keeps the GPU behind the CPU. With two slots the next frame is recorded while the
// previous one still runs. Pixels are checked at the end, so any resource reused too early corrupts an earlier frame.
#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include "llfsr2.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

// Column-major 4x4 helpers (GL layout, as glm) for the FSR 2 motion-vector case.
struct Mat4 { float m[16]; };
static Mat4 identity4() { Mat4 r{}; for (int i = 0; i < 4; ++i) r.m[i * 5] = 1.f; return r; }
static Mat4 mul4(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += (double) a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = (float) s;
        }
    return r;
}
static Mat4 perspective4(float fovy, float aspect, float zn, float zf)
{
    Mat4 r{};
    const float f = 1.f / std::tan(fovy * 0.5f);
    r.m[0] = f / aspect; r.m[5] = f; r.m[10] = (zf + zn) / (zn - zf); r.m[11] = -1.f; r.m[14] = 2.f * zf * zn / (zn - zf);
    return r;
}
static Mat4 translate4(float x, float y, float z) { Mat4 r = identity4(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static Mat4 rotateY4(float a)
{
    Mat4 r = identity4();
    r.m[0] = std::cos(a); r.m[2] = -std::sin(a); r.m[8] = std::sin(a); r.m[10] = std::cos(a);
    return r;
}
static Mat4 inverse4(const Mat4& a)   // Gauss-Jordan in double
{
    double w[4][8];
    for (int row = 0; row < 4; ++row)
        for (int c = 0; c < 8; ++c) w[row][c] = c < 4 ? a.m[c * 4 + row] : (c - 4 == row ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int row = c + 1; row < 4; ++row) if (std::fabs(w[row][c]) > std::fabs(w[pivot][c])) pivot = row;
        for (int k = 0; k < 8; ++k) std::swap(w[c][k], w[pivot][k]);
        const double d = w[c][c];
        for (int k = 0; k < 8; ++k) w[c][k] /= d;
        for (int row = 0; row < 4; ++row)
            if (row != c) { const double f = w[row][c]; for (int k = 0; k < 8; ++k) w[row][k] -= f * w[c][k]; }
    }
    Mat4 r{};
    for (int row = 0; row < 4; ++row) for (int c = 0; c < 4; ++c) r.m[c * 4 + row] = (float) w[row][c + 4];
    return r;
}
static void transform4(const Mat4& a, const double v[4], double out[4])
{
    for (int row = 0; row < 4; ++row)
    {
        out[row] = 0;
        for (int k = 0; k < 4; ++k) out[row] += (double) a.m[k * 4 + row] * v[k];
    }
}
static bool readViewerShader(const char* name, std::string& text)
{
    std::ifstream in(std::string(LL_VIEWER_DEFERRED_SHADERS) + "/" + name, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

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
            // FSR 2 motion vectors through the viewer's fsr2MotionF.glsl: a plane is drawn into a render-size depth target the way the
            // scene is (offscreen, GL clip depth), then the motion pass writes UV-unit camera motion into an RG16F target.
            {
                constexpr U32 MW = 80, MH = 60;
                auto makeImage = [&](U32 internal, U32 format, U32 type, bool fsrImage) {
                    U32 name; textures.generate(1, &name);
                    if (fsrImage) textures.setStorageImage(name);
                    textures.bind(glc::TEXTURE_2D, name);
                    textures.image2D(glc::TEXTURE_2D, 0, internal, MW, MH, format, type, nullptr);
                    textures.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
                    return name;
                };
                const U32 depthTex = makeImage(glc::DEPTH_COMPONENT32F, glc::DEPTH_COMPONENT, glc::FLOAT, true);
                const U32 sceneTex = makeImage(glc::RGBA8, glc::RGBA, glc::UNSIGNED_BYTE, false);
                const U32 motionTex = makeImage(glc::RG16F, glc::RG, glc::FLOAT, true);
                const U32 markerTex = makeImage(glc::RGBA8, glc::RGBA, glc::UNSIGNED_BYTE, false);
                VulkanTextureBackend::ImageInfo info;
                check(textures.imageInfo(motionTex, info) && info.levels == 1 && info.format == VK_FORMAT_R16G16_SFLOAT,
                      "motion target is RG16F with one level");
                U32 sceneFb = fb.createFramebuffer(); fb.bind(sceneFb);
                fb.attach(sceneFb, 0x8CE0, glc::TEXTURE_2D, sceneTex, 0); fb.attach(sceneFb, 0x8D00, glc::TEXTURE_2D, depthTex, 0);
                U32 motionFb = fb.createFramebuffer(); fb.bind(motionFb); fb.attach(motionFb, 0x8CE0, glc::TEXTURE_2D, motionTex, 0);
                U32 markerFb = fb.createFramebuffer(); fb.bind(markerFb); fb.attach(markerFb, 0x8CE0, glc::TEXTURE_2D, markerTex, 0);
                fb.bind(0);

                auto build = [&](const std::string& vs, const std::string& fs, const char* label) {
                    U32 p = shaders.createProgram();
                    const char* src[] = {vs.c_str(), fs.c_str()};
                    for (int i = 0; i < 2; ++i)
                    {
                        U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                        const bool ok = shaders.compileObject(object, 1, &src[i], log);
                        if (!ok) printf("%s\n", log.c_str());
                        check(ok, label); shaders.attachObject(p, object);
                    }
                    shaders.bindAttribLocation(p, 0, "position");
                    check(shaders.link(p), label);
                    return p;
                };
                const std::string white = "#version 330\nout vec4 color;\nvoid main(){color=vec4(1.0);}";
                const U32 planeProg = build("#version 330\nin vec4 position;\nuniform float plane_z;\n"
                                            "void main(){gl_Position=vec4(position.xy,plane_z,1.0);}", white, "depth plane program");
                const U32 markerProg = build("#version 330\nin vec4 position;\nuniform vec4 place;\n"
                                             "void main(){gl_Position=vec4(position.xy*place.zw+place.xy,0.0,1.0);}", white, "marker program");
                std::string vertexBody, fragmentBody;
                check(readViewerShader("postDeferredNoTCV.glsl", vertexBody) && readViewerShader("fsr2MotionF.glsl", fragmentBody),
                      "viewer motion shaders readable");
                const U32 motionProg = build("#version 400\n#define VERTEX_SHADER 1\n" + vertexBody,
                                             "#version 400\n#define FRAGMENT_SHADER 1\n" + fragmentBody, "fsr2MotionF.glsl program");
                const S32 locDepth = shaders.getUniformLocation(motionProg, "depthMap");
                const S32 locInvCur = shaders.getUniformLocation(motionProg, "inv_view_proj_cur");
                const S32 locCur = shaders.getUniformLocation(motionProg, "view_proj_cur");
                const S32 locPrev = shaders.getUniformLocation(motionProg, "view_proj_prev");
                const S32 locSign = shaders.getUniformLocation(motionProg, "motion_y_sign");

                // camera at (1.5, 0.75, 2) looking down -z; 60 degree vertical field of view; the plane 10 m in front of it
                const float kPi = 3.14159265358979f;
                const Mat4 proj = perspective4(kPi / 3.f, float(MW) / float(MH), 0.5f, 100.f);
                const Mat4 viewCur = translate4(-1.5f, -0.75f, -2.f);
                const Mat4 vpCur = mul4(proj, viewCur);
                const Mat4 invCur = inverse4(vpCur);
                constexpr double kPlaneDistance = 10.0;
                const float planeZ = float((proj.m[10] * -kPlaneDistance + proj.m[14]) / kPlaneDistance);
                printf("  motion: depth clip control %s\n", ctx.depthClipControl ? "on" : "OFF (GL depth reconstruction is wrong)");
                check(ctx.depthClipControl, "GL clip depth range (VK_EXT_depth_clip_control) for the depth convention");

                std::vector<float> motion(MW * MH * 4);
                auto at = [&](U32 x, U32 y) { return &motion[(y * MW + x) * 4]; };
                auto runMotion = [&](bool drawPlane, const Mat4& vpPrev) {
                    check(ctx.beginFrame(clear), "motion frame acquired");
                    fb.bind(sceneFb); state.setViewport(0, 0, MW, MH); state.setDepthMask(true);
                    fb.setClearColor(0, 0, 0, 1); fb.setClearDepth(1.0); fb.clear(0x4000 | 0x100);
                    if (drawPlane)
                    {
                        state.setDepthTest(true); state.setDepthFunc(0x0207);   // GL_ALWAYS
                        shaders.use(planeProg); shaders.uniform1f(planeProg, shaders.getUniformLocation(planeProg, "plane_z"), planeZ);
                        vb.drawArrays(0, 6, 3);
                        state.setDepthTest(false);
                    }
                    fb.bind(motionFb); fb.setClearColor(9, 9, 0, 0); fb.clear(0x4000);   // a skipped pass leaves 9s
                    shaders.use(motionProg);
                    textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, depthTex);
                    shaders.uniform1i(motionProg, locDepth, 0);
                    shaders.uniformMatrix4fv(motionProg, locInvCur, 1, false, invCur.m);
                    shaders.uniformMatrix4fv(motionProg, locCur, 1, false, vpCur.m);
                    shaders.uniformMatrix4fv(motionProg, locPrev, 1, false, vpPrev.m);
                    shaders.uniform1f(motionProg, locSign, kFsr2MotionYSign);
                    vb.drawArrays(0, 6, 3);
                    fb.readPixels(0, 0, MW, MH, glc::RGBA, glc::FLOAT, motion.data());
                    fb.bind(0); ctx.endFrame(false);
                };
                const U32 cx = MW / 2, cy = MH / 2;
                const double curUv[2] = {(cx + 0.5) / MW, (cy + 0.5) / MH};

                // 1. static camera: previous == current gives exactly zero, everywhere
                runMotion(true, vpCur);
                int nonzero = 0;
                for (U32 i = 0; i < MW * MH; ++i) nonzero += motion[i * 4] != 0.f || motion[i * 4 + 1] != 0.f;
                printf("  static camera: centre %g %g, %d non-zero pixels\n", at(cx, cy)[0], at(cx, cy)[1], nonzero);
                check(at(cx, cy)[0] == 0.f && at(cx, cy)[1] == 0.f && nonzero == 0, "static camera gives exactly zero motion");

                // 2. previous camera 0.4 m right and 0.5 m up of the current one, same direction. For a plane facing the camera the
                // motion is the same at every pixel: prevNdc - curNdc = -P * shift / distance, in UV units half of that.
                const float shiftX = 0.4f, shiftY = 0.5f;
                const Mat4 vpShift = mul4(proj, translate4(-1.5f - shiftX, -0.75f - shiftY, -2.f));
                runMotion(true, vpShift);
                const double expectX = -0.5 * proj.m[0] * shiftX / kPlaneDistance;
                const double expectY = -0.5 * proj.m[5] * shiftY / kPlaneDistance * kFsr2MotionYSign;
                printf("  shifted camera: centre %.6f %.6f, expected %.6f %.6f (%.2f, %.2f px)\n", at(cx, cy)[0], at(cx, cy)[1], expectX,
                       expectY, expectX * MW, expectY * MH);
                bool uniform = true;
                const U32 probes[3][2] = {{cx, cy}, {3, 4}, {MW - 2, MH - 3}};
                for (const auto& p : probes)
                    uniform = std::fabs(at(p[0], p[1])[0] - expectX) < 1e-4 && std::fabs(at(p[0], p[1])[1] - expectY) < 1e-4 && uniform;
                check(uniform, "shifted previous camera gives the analytic UV motion (centre and corners)");

                // The sign, end to end: draw a marker where the previous camera saw the centre pixel's point, in the same offscreen
                // way. FSR 3.1 looks for the previous frame at uv + motion in memory UV; the marker must be there, not at uv - motion.
                const double ndc[4] = {curUv[0] * 2 - 1, curUv[1] * 2 - 1, planeZ, 1};
                double world[4], prevClip[4];
                transform4(invCur, ndc, world);
                transform4(vpShift, world, prevClip);
                check(ctx.beginFrame(clear), "marker frame acquired");
                fb.bind(markerFb); state.setViewport(0, 0, MW, MH); fb.setClearColor(0, 0, 0, 1); fb.clear(0x4000);
                shaders.use(markerProg);
                shaders.uniform4f(markerProg, shaders.getUniformLocation(markerProg, "place"), float(prevClip[0] / prevClip[3]),
                                  float(prevClip[1] / prevClip[3]), 1.f / MW, 1.f / MH);   // half a pixel
                vb.drawArrays(0, 6, 3);
                const float* mv = at(cx, cy);
                const S32 px = S32(std::floor((curUv[0] + mv[0]) * MW)), py = S32(std::floor((curUv[1] + mv[1]) * MH));
                const S32 wrongY = S32(std::floor((curUv[1] - mv[1]) * MH));
                uint8_t hit[4]{}, miss[4]{};
                fb.readPixels(px, py, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, hit);
                fb.readPixels(px, wrongY, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, miss);
                fb.bind(0); ctx.endFrame(false);
                printf("  marker: centre pixel %u,%u + motion -> %d,%d lit %u; flipped row %d lit %u\n", cx, cy, px, py, hit[0], wrongY, miss[0]);
                check(hit[0] == 255 && miss[0] == 0 && py != wrongY, "uv + motion finds the previous position in memory rows (Y sign)");

                // 3. sky: no geometry (depth 1.0, the far plane) and a 2 degree turn: finite everywhere, matches the reference
                const Mat4 vpTurn = mul4(proj, mul4(rotateY4(2.f * kPi / 180.f), viewCur));
                runMotion(false, vpTurn);
                bool finite = true;
                for (U32 i = 0; i < MW * MH; ++i) finite = std::isfinite(motion[i * 4]) && std::isfinite(motion[i * 4 + 1]) && finite;
                const double farNdc[4] = {curUv[0] * 2 - 1, curUv[1] * 2 - 1, 1, 1};
                double farWorld[4], cur[4], prev[4];
                transform4(invCur, farNdc, farWorld);
                transform4(vpCur, farWorld, cur);
                transform4(vpTurn, farWorld, prev);
                const double skyX = (prev[0] / prev[3] - cur[0] / cur[3]) * 0.5;
                const double skyY = (prev[1] / prev[3] - cur[1] / cur[3]) * 0.5 * kFsr2MotionYSign;
                printf("  sky turn: centre %.6f %.6f, expected %.6f %.6f\n", at(cx, cy)[0], at(cx, cy)[1], skyX, skyY);
                check(finite && std::fabs(skyX) > 0.01 && std::fabs(at(cx, cy)[0] - skyX) < 2e-4 && std::fabs(at(cx, cy)[1] - skyY) < 2e-4,
                      "far-plane pixels reproject under rotation without NaN");

                shaders.use(0);
                for (U32 p : {planeProg, markerProg, motionProg}) shaders.deleteProgramAndObjects(p);
                for (U32 f : {sceneFb, motionFb, markerFb}) fb.destroyFramebuffer(f);
                const U32 names[] = {depthTex, sceneTex, motionTex, markerTex};
                textures.destroy(4, names);
                VK_CHECK(vkDeviceWaitIdle(ctx.device));
            }
            // FSR 2 reactive mask through the viewer's fsr2ReactiveF.glsl: a synthetic G-buffer normal target whose alpha holds the
            // four GBUFFER_FLAG_* codes in repeating columns (0.0, 0.34, 0.67, 1.0), in both normal formats the viewer allocates
            // (RGBA16 with HDR, RGB10_A2 without). The R8 mask must be exactly 0 everywhere with the pipeline's v1 flag (none), and
            // 0.9 (clamped) on the marked code's columns, exactly 0 on the others, when a code is selected.
            {
                constexpr U32 RW = 64, RH = 48;
                const float codes[4] = {0.0f, 0.34f, 0.67f, 1.0f};   // llshadermgr.cpp GBUFFER_FLAG_SKIP_ATMOS/HAS_ATMOS/HAS_PBR/HAS_HDRI
                auto makeImage = [&](U32 internal, U32 format, U32 type, bool fsrImage) {
                    U32 name; textures.generate(1, &name);
                    if (fsrImage) textures.setStorageImage(name);
                    textures.bind(glc::TEXTURE_2D, name);
                    textures.image2D(glc::TEXTURE_2D, 0, internal, RW, RH, format, type, nullptr);
                    textures.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
                    return name;
                };
                const U32 reactiveTex = makeImage(glc::R8, glc::RED, glc::UNSIGNED_BYTE, true);
                VulkanTextureBackend::ImageInfo info;
                check(textures.imageInfo(reactiveTex, info) && info.levels == 1 && info.format == VK_FORMAT_R8_UNORM,
                      "reactive target is R8 with one level");
                U32 reactiveFb = fb.createFramebuffer(); fb.bind(reactiveFb); fb.attach(reactiveFb, 0x8CE0, glc::TEXTURE_2D, reactiveTex, 0);
                fb.bind(0);

                auto build = [&](const std::string& vs, const std::string& fs, const char* label) {
                    U32 p = shaders.createProgram();
                    const char* src[] = {vs.c_str(), fs.c_str()};
                    for (int i = 0; i < 2; ++i)
                    {
                        U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                        const bool ok = shaders.compileObject(object, 1, &src[i], log);
                        if (!ok) printf("%s\n", log.c_str());
                        check(ok, label); shaders.attachObject(p, object);
                    }
                    shaders.bindAttribLocation(p, 0, "position");
                    check(shaders.link(p), label);
                    return p;
                };
                const U32 gbufferProg = build("#version 330\nin vec4 position;\nvoid main(){gl_Position=vec4(position.xy,0.0,1.0);}",
                                              "#version 330\nout vec4 color;\nuniform vec4 codes;\n"
                                              "void main(){int band=int(gl_FragCoord.x)%4;color=vec4(0.5,0.5,0.0,codes[band]);}",
                                              "synthetic G-buffer program");
                std::string vertexBody, fragmentBody;
                check(readViewerShader("postDeferredNoTCV.glsl", vertexBody) && readViewerShader("fsr2ReactiveF.glsl", fragmentBody),
                      "viewer reactive shaders readable");
                const U32 reactiveProg = build("#version 400\n#define VERTEX_SHADER 1\n" + vertexBody,
                                               "#version 400\n#define FRAGMENT_SHADER 1\n" + fragmentBody, "fsr2ReactiveF.glsl program");
                const S32 locNormal = shaders.getUniformLocation(reactiveProg, "normalMap");
                const S32 locFlag = shaders.getUniformLocation(reactiveProg, "reactive_flag");
                const S32 locValue = shaders.getUniformLocation(reactiveProg, "reactive_value");

                std::vector<float> mask(RW * RH * 4);
                auto runReactive = [&](U32 gbufferFb, U32 normalTex, float flag, float value) {
                    check(ctx.beginFrame(clear), "reactive frame acquired");
                    fb.bind(gbufferFb); state.setViewport(0, 0, RW, RH);
                    shaders.use(gbufferProg);
                    shaders.uniform4f(gbufferProg, shaders.getUniformLocation(gbufferProg, "codes"), codes[0], codes[1], codes[2], codes[3]);
                    vb.drawArrays(0, 6, 3);
                    fb.bind(reactiveFb); fb.setClearColor(1, 1, 1, 1); fb.clear(0x4000);   // a skipped pass leaves 255
                    shaders.use(reactiveProg);
                    textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, normalTex);
                    shaders.uniform1i(reactiveProg, locNormal, 0);
                    shaders.uniform1f(reactiveProg, locFlag, flag);
                    shaders.uniform1f(reactiveProg, locValue, value);
                    vb.drawArrays(0, 6, 3);
                    fb.readPixels(0, 0, RW, RH, glc::RGBA, glc::FLOAT, mask.data());
                    fb.bind(0); ctx.endFrame(false);
                };
                // marked: 0.9 to within one R8 step where the column's code is `flag` (the GPU rounds 229.5 either way; an unclamped
                // 1.0 would read 255); everything else must be exactly 0
                auto score = [&](float flag, U32& marked, U32& wrong) {
                    marked = wrong = 0;
                    for (U32 y = 0; y < RH; ++y)
                        for (U32 x = 0; x < RW; ++x)
                        {
                            const float v = mask[(y * RW + x) * 4];
                            const bool flagged = flag >= 0.f && std::fabs(codes[x % 4] - flag) < 0.1f;
                            if (flagged && std::fabs(v - kFsr2ReactiveMax) <= 1.f / 255.f) ++marked;
                            else if (flagged || v != 0.f) ++wrong;
                        }
                };
                const struct { U32 internal, format, type; const char* name; } normalFormats[] = {
                    {glc::RGBA16, glc::RGBA, glc::UNSIGNED_BYTE, "RGBA16"}, {glc::RGB10_A2, glc::RGBA, glc::UNSIGNED_BYTE, "RGB10_A2"}};
                U32 normalTexs[2], gbufferFbs[2];
                for (int f = 0; f < 2; ++f)
                {
                    const auto& nf = normalFormats[f];
                    const U32 normalTex = normalTexs[f] = makeImage(nf.internal, nf.format, nf.type, false);
                    const U32 gbufferFb = gbufferFbs[f] = fb.createFramebuffer();
                    fb.bind(gbufferFb); fb.attach(gbufferFb, 0x8CE0, glc::TEXTURE_2D, normalTex, 0); fb.bind(0);
                    U32 marked = 0, wrong = 0;
                    // 1. the pipeline's v1 values: no G-buffer code is marked, the mask is exactly 0
                    runReactive(gbufferFb, normalTex, kFsr2ReactiveFlag, kFsr2ReactiveMax);
                    score(kFsr2ReactiveFlag, marked, wrong);
                    printf("  reactive %s, v1 flag %g: %u marked, %u non-zero\n", nf.name, kFsr2ReactiveFlag, marked, wrong);
                    check(kFsr2ReactiveFlag < 0.f && marked == 0 && wrong == 0, "reactive v1 (no flag) is exactly 0 everywhere");
                    // 2. the HAS_PBR code selected and a value above the cap: 0.9 on its columns, exactly 0 on the other three codes
                    runReactive(gbufferFb, normalTex, codes[2], 1.0f);
                    score(codes[2], marked, wrong);
                    printf("  reactive %s, flag %.2f value 1.0: %u of %u marked (column 2 reads %.6f = %ld/255), %u wrong\n", nf.name,
                           codes[2], marked, RW * RH / 4, mask[2 * 4], std::lround(mask[2 * 4] * 255.f), wrong);
                    check(marked == RW * RH / 4 && wrong == 0, "reactive flagged code is 0.9 (clamped), others exactly 0");
                }

                shaders.use(0);
                for (U32 p : {gbufferProg, reactiveProg}) shaders.deleteProgramAndObjects(p);
                for (U32 f : {reactiveFb, gbufferFbs[0], gbufferFbs[1]}) fb.destroyFramebuffer(f);
                const U32 names[] = {reactiveTex, normalTexs[0], normalTexs[1]};
                textures.destroy(3, names);
                VK_CHECK(vkDeviceWaitIdle(ctx.device));
            }
            // FSR 2 chain as renderFinalize runs it, at the llvulkan layer: a static scene (one triangle with an axis-aligned bottom
            // and left edge at non-integer render pixels) is drawn every frame through the viewer's jittered projection
            // (fsr2JitterProjection), the viewer's motion shader writes the motion, the reactive mask is cleared once, the upscale
            // goes to a display-size storage target, the glow alpha is copied back with an alpha-only draw and a tonemap pass reads
            // the display-size result. The Y convention is checked end to end: with the jitter the pipeline reports
            // (fsr2DispatchJitter), the converged edges sit where they are at display resolution; with either axis flipped they
            // do not. With the upscaler off, the plain chain gives the same pixels before and after the upscaler ran.
            {
                constexpr U32 RW = 64, RH = 48, DW = 128, DH = 96;
                const int phases = fsr2JitterPhaseCount(float(DW) / float(RW));
                auto makeImage = [&](U32 internal, U32 format, U32 type, U32 w, U32 h, bool fsrImage) {
                    U32 name; textures.generate(1, &name);
                    if (fsrImage) textures.setStorageImage(name);
                    textures.bind(glc::TEXTURE_2D, name);
                    textures.image2D(glc::TEXTURE_2D, 0, internal, w, h, format, type, nullptr);
                    textures.setFilter(glc::TEXTURE_2D, glc::LINEAR, glc::LINEAR);
                    return name;
                };
                auto makeFb = [&](U32 colorTex, U32 depthTex) {
                    U32 f = fb.createFramebuffer(); fb.bind(f); fb.attach(f, 0x8CE0, glc::TEXTURE_2D, colorTex, 0);
                    if (depthTex) fb.attach(f, 0x8D00, glc::TEXTURE_2D, depthTex, 0);
                    fb.bind(0);
                    return f;
                };
                auto build = [&](const std::string& vs, const std::string& fs, const char* label) {
                    U32 p = shaders.createProgram();
                    const char* src[] = {vs.c_str(), fs.c_str()};
                    for (int i = 0; i < 2; ++i)
                    {
                        U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                        const bool ok = shaders.compileObject(object, 1, &src[i], log);
                        if (!ok) printf("%s\n", log.c_str());
                        check(ok, label); shaders.attachObject(p, object);
                    }
                    shaders.bindAttribLocation(p, 0, "position");
                    check(shaders.link(p), label);
                    return p;
                };
                const U32 colorTex = makeImage(glc::RGBA16F, glc::RGBA, glc::FLOAT, RW, RH, true);       // mRT->screen
                const U32 depthTex = makeImage(glc::DEPTH_COMPONENT24, glc::DEPTH_COMPONENT, glc::FLOAT, RW, RH, true);  // its depth
                const U32 motionTex = makeImage(glc::RG16F, glc::RG, glc::FLOAT, RW, RH, true);          // mFSR2MotionMap
                const U32 reactiveTex = makeImage(glc::R8, glc::RED, glc::UNSIGNED_BYTE, RW, RH, true);  // mFSR2ReactiveMap
                const U32 outputTex = makeImage(glc::RGBA16F, glc::RGBA, glc::FLOAT, DW, DH, true);      // mFSR2Map
                const U32 postTex = makeImage(glc::RGBA8, glc::RGBA, glc::UNSIGNED_BYTE, DW, DH, false); // mPostPingMap (display)
                const U32 plainTex = makeImage(glc::RGBA8, glc::RGBA, glc::UNSIGNED_BYTE, RW, RH, false); // mPostPingMap (off)
                VulkanTextureBackend::ImageInfo info;
                check(textures.imageInfo(depthTex, info) && info.levels == 1 && info.format == VK_FORMAT_D32_SFLOAT,
                      "chain: the viewer's DEPTH_COMPONENT24 depth opted in is single-level D32_SFLOAT");
                const U32 sceneFb = makeFb(colorTex, depthTex), motionFb = makeFb(motionTex, 0), reactiveFb = makeFb(reactiveTex, 0);
                const U32 outFb = makeFb(outputTex, 0), postFb = makeFb(postTex, 0), plainFb = makeFb(plainTex, 0);

                const U32 sceneProg = build("#version 330\nin vec4 position;\nuniform mat4 mvp;\n"
                                            "void main(){gl_Position=mvp*vec4(position.xy,0.5,1.0);}",
                                            "#version 330\nout vec4 color;\nvoid main(){color=vec4(2.0,1.0,0.5,0.25);}", "chain scene program");
                const S32 locMvp = shaders.getUniformLocation(sceneProg, "mvp");
                std::string motionV, motionF, copyV, copyF;
                check(readViewerShader("postDeferredNoTCV.glsl", motionV) && readViewerShader("fsr2MotionF.glsl", motionF) &&
                      readViewerShader("../interface/copyV.glsl", copyV) && readViewerShader("../interface/copyF.glsl", copyF),
                      "chain: viewer motion and copy shaders readable");
                const U32 motionProg = build("#version 400\n#define VERTEX_SHADER 1\n" + motionV,
                                             "#version 400\n#define FRAGMENT_SHADER 1\n" + motionF, "chain motion program");
                const U32 copyProg = build("#version 400\n#define VERTEX_SHADER 1\n" + copyV,
                                           "#version 400\n#define FRAGMENT_SHADER 1\n" + copyF, "chain copy program (gCopyProgram)");
                const U32 toneProg = build("#version 330\nin vec4 position;\nout vec2 tc;\nvoid main(){tc=position.xy*0.5+0.5;gl_Position=position;}",
                                           "#version 330\nuniform sampler2D diffuseMap;\nin vec2 tc;\nout vec4 color;\n"
                                           "void main(){vec4 c=texture(diffuseMap,tc);color=vec4(c.rgb/(1.0+c.rgb),c.a);}", "chain tonemap program");

                // the left triangle (vertices 0-2: (-1,-1), (0,-1), (-1,1)) placed so its left edge is at render x 16.15 and its
                // bottom edge at render y 12.15 (display 32.3 and 24.3); the hypotenuse stays away from the measured profiles
                const float L = 16.15f / (RW * 0.5f) - 1.f, R = 40.15f / (RW * 0.5f) - 1.f;
                const float B = 12.15f / (RH * 0.5f) - 1.f, T = 36.15f / (RH * 0.5f) - 1.f;
                Mat4 model = identity4();
                model.m[0] = R - L; model.m[12] = R; model.m[5] = (T - B) * 0.5f; model.m[13] = (T + B) * 0.5f;
                const float edgeX = (L + 1.f) * 0.5f * DW, edgeY = (B + 1.f) * 0.5f * DH;
                const Mat4 ident = identity4();
                const float bg = 0.05f, fg = 2.0f;
                const float clearScene[4] = {bg, bg, bg, 0.f};

                auto drawScene = [&](float jx, float jy) {
                    Mat4 jitter; fsr2JitterProjection(jx, jy, float(RW), float(RH), jitter.m);
                    const Mat4 mvp = mul4(jitter, model);
                    fb.bind(sceneFb); state.setViewport(0, 0, RW, RH); state.setDepthMask(true);
                    fb.setClearColor(clearScene[0], clearScene[1], clearScene[2], clearScene[3]); fb.setClearDepth(1.0); fb.clear(0x4000 | 0x100);
                    state.setDepthTest(true); state.setDepthFunc(0x0207);   // GL_ALWAYS, depth written
                    shaders.use(sceneProg); shaders.uniformMatrix4fv(sceneProg, locMvp, 1, false, mvp.m);
                    vb.drawArrays(0, 0, 3);
                    state.setDepthTest(false);
                };
                auto drawMotion = [&]() {   // static camera: previous == current, exactly zero
                    fb.bind(motionFb); state.setViewport(0, 0, RW, RH);
                    shaders.use(motionProg);
                    textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, depthTex);
                    shaders.uniform1i(motionProg, shaders.getUniformLocation(motionProg, "depthMap"), 0);
                    for (const char* n : {"inv_view_proj_cur", "view_proj_cur", "view_proj_prev"})
                        shaders.uniformMatrix4fv(motionProg, shaders.getUniformLocation(motionProg, n), 1, false, ident.m);
                    shaders.uniform1f(motionProg, shaders.getUniformLocation(motionProg, "motion_y_sign"), kFsr2MotionYSign);
                    vb.drawArrays(0, 6, 3);
                };
                auto drawTonemap = [&](U32 srcTex, U32 dstFb, U32 w, U32 h) {
                    fb.bind(dstFb); state.setViewport(0, 0, w, h);
                    shaders.use(toneProg);
                    textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, srcTex);
                    shaders.uniform1i(toneProg, shaders.getUniformLocation(toneProg, "diffuseMap"), 0);
                    vb.drawArrays(0, 6, 3);
                };
                RenderBackendFsr2Params p;
                p.colorTex = colorTex; p.depthTex = depthTex; p.motionTex = motionTex; p.reactiveTex = reactiveTex; p.outputTex = outputTex;
                p.renderW = RW; p.renderH = RH; p.displayW = DW; p.displayH = DH;
                p.frameTimeMs = 16.7f; p.sharpness = 0.f; p.cameraNear = 0.5f; p.cameraFar = 100.f; p.fovY = 1.0f;

                // the overlap stress left the right triangle in vertices 0-2: put the original positions back
                check(ctx.beginFrame(clear), "chain: vertex reset frame acquired");
                vb.upload(BufferKind::Vertex, vertices, 0, sizeof(positions) - 1, positions);
                ctx.endFrame(false);

                // the plain chain (upscaler off): scene at render size, tonemap at render size
                std::vector<uint8_t> plainBefore(RW * RH * 4), plainAfter(RW * RH * 4);
                auto runPlain = [&](std::vector<uint8_t>& out) {
                    check(ctx.beginFrame(clear), "chain: plain frame acquired");
                    drawScene(0.f, 0.f);
                    drawTonemap(colorTex, plainFb, RW, RH);
                    fb.readPixels(0, 0, RW, RH, glc::RGBA, glc::UNSIGNED_BYTE, out.data());
                    fb.bind(0); ctx.endFrame(false);
                };
                runPlain(plainBefore);

                // the reactive mask is cleared once when it is made and kept (v1 marks nothing)
                check(ctx.beginFrame(clear), "chain: reactive clear frame acquired");
                fb.bind(reactiveFb); fb.setClearColor(0, 0, 0, 0); fb.clear(0x4000); fb.bind(0);
                ctx.endFrame(false);

                // signX / signY multiply the jitter the pipeline reports; (1, 1) is what renderFinalize passes
                std::vector<float> out(DW * DH * 4);
                auto converge = [&](float signX, float signY) {
                    bool all = true;
                    for (int f = 0; f < 2 * phases && all; ++f)
                    {
                        all = ctx.beginFrame(clear);
                        if (!all) break;
                        float jx, jy; fsr2JitterOffset(f, phases, jx, jy);
                        drawScene(jx, jy);
                        drawMotion();
                        RenderBackendFsr2Params q = p;
                        fsr2DispatchJitter(jx, jy, q.jitterX, q.jitterY);
                        q.jitterX *= signX; q.jitterY *= signY;
                        q.reset = f == 0;
                        all = fb.dispatchFsr2(q) && all;
                        fb.bind(0); ctx.endFrame(false);
                    }
                    check(ctx.beginFrame(clear), "chain: output read frame acquired");
                    fb.bind(outFb); fb.readPixels(0, 0, DW, DH, glc::RGBA, glc::FLOAT, out.data());
                    fb.bind(0); ctx.endFrame(false);
                    return all;
                };
                // coverage profiles across the bottom edge (column 60, rows 14-35) and the left edge (row 36, columns 20-47),
                // against the exact display-resolution coverage; the sum of absolute differences in pixels
                auto value = [&](U32 x, U32 y) { return (out[(y * DW + x) * 4] - bg) / (fg - bg); };
                auto edgeError = [&](bool vertical) {
                    double e = 0;
                    if (vertical) for (U32 y = 14; y < 36; ++y) e += std::fabs(value(60, y) - std::clamp(y + 1.f - edgeY, 0.f, 1.f));
                    else for (U32 x = 20; x < 48; ++x) e += std::fabs(value(x, 36) - std::clamp(x + 1.f - edgeX, 0.f, 1.f));
                    return e;
                };
                // four runs, the pipeline's jitter last (its output feeds the checks below): for each X sign the Y as reported must
                // give the cleaner bottom edge, and for each Y sign the X as reported the cleaner left edge (X is the method check:
                // its convention is not in doubt)
                double xErr[2][2], yErr[2][2];     // [X as reported][Y as reported]
                bool dispatched = true;
                for (int run = 0; run < 4; ++run)
                {
                    const int ax = run >> 1, ay = run & 1;
                    dispatched = converge(ax ? 1.f : -1.f, ay ? 1.f : -1.f) && dispatched;
                    xErr[ax][ay] = edgeError(false);
                    yErr[ax][ay] = edgeError(true);
                    printf("  chain run X %s Y %s: left-edge error %.3f, bottom-edge error %.3f (display px)\n", ax ? "as reported" : "flipped",
                           ay ? "as reported" : "flipped", xErr[ax][ay], yErr[ax][ay]);
                }
                printf("  chain edges at display x %.2f, y %.2f; %d phases\n", edgeX, edgeY, phases);
                check(dispatched, "chain: motion, cleared reactive and upscale dispatched every frame of every run");
                check(xErr[1][0] < xErr[0][0] && xErr[1][1] < xErr[0][1] && xErr[1][1] < 0.25,
                      "chain: X jitter as reported reconstructs the left edge, flipped X is worse (method check)");
                check(yErr[0][1] < yErr[0][0] && yErr[1][1] < yErr[1][0] && yErr[1][1] < 0.25,
                      "chain: Y jitter as reported (kFsr2JitterYSign) reconstructs the bottom edge, flipped Y is worse");

                // the glow mask: the SDK writes alpha 1, renderFinalize copies the scene alpha back with an alpha-only draw
                const float inside[2] = {60.f, 40.f}, outside[2] = {8.f, 8.f};
                auto px = [&](const float* at) { return &out[(U32(at[1]) * DW + U32(at[0])) * 4]; };
                const float rgbIn[3] = {px(inside)[0], px(inside)[1], px(inside)[2]};
                printf("  chain: upscaled inside %.3f %.3f %.3f a %.3f, outside %.3f a %.3f\n", rgbIn[0], rgbIn[1], rgbIn[2], px(inside)[3],
                       px(outside)[0], px(outside)[3]);
                check(std::fabs(rgbIn[0] - fg) < 0.05f && std::fabs(px(outside)[0] - bg) < 0.01f, "chain: display-size output is the HDR scene, not blank");
                check(ctx.beginFrame(clear), "chain: post frame acquired");
                fb.bind(outFb); state.setViewport(0, 0, DW, DH);
                state.setColorMask(false, false, false, true);
                shaders.use(copyProg);
                textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, colorTex);
                shaders.uniform1i(copyProg, shaders.getUniformLocation(copyProg, "diffuseMap"), 0);
                vb.drawArrays(0, 6, 3);
                state.setColorMask(true, true, true, true);
                fb.readPixels(0, 0, DW, DH, glc::RGBA, glc::FLOAT, out.data());
                drawTonemap(outputTex, postFb, DW, DH);
                std::vector<uint8_t> post(DW * DH * 4);
                fb.readPixels(0, 0, DW, DH, glc::RGBA, glc::UNSIGNED_BYTE, post.data());
                fb.bind(0); ctx.endFrame(false);
                printf("  chain: after alpha copy inside %.3f %.3f %.3f a %.3f, outside a %.3f\n", px(inside)[0], px(inside)[1], px(inside)[2],
                       px(inside)[3], px(outside)[3]);
                check(px(inside)[0] == rgbIn[0] && px(inside)[1] == rgbIn[1] && px(inside)[2] == rgbIn[2] &&
                      std::fabs(px(inside)[3] - 0.25f) < 0.01f && px(outside)[3] < 0.01f,
                      "chain: alpha-only copy restores the glow mask and keeps the upscaled colour");
                uint8_t lo = 255, hi = 0;
                for (U32 i = 0; i < DW * DH; ++i) { lo = std::min(lo, post[i * 4]); hi = std::max(hi, post[i * 4]); }
                check(textures.imageInfo(postTex, info) && info.width == DW && info.height == DH && hi - lo > 100,
                      "chain: the tonemap pass reads the display-size result (display-size, non-blank)");

                // a recreated swapchain resets the history on the next dispatch only
                check(ctx.beginFrame(clear), "chain: swapchain reset frame acquired");
                drawScene(0.f, 0.f); drawMotion();
                const bool before = fb.dispatchFsr2(p) && !fb.fsr2LastReset();
                ++ctx.swapchainGeneration;
                const bool after = fb.dispatchFsr2(p) && fb.fsr2LastReset();
                const bool next = fb.dispatchFsr2(p) && !fb.fsr2LastReset();
                --ctx.swapchainGeneration;
                fb.bind(0); ctx.endFrame(false);
                check(before && after && next, "chain: a new swapchain generation resets the history once");

                fb.releaseFsr2();
                runPlain(plainAfter);
                check(ctx.upscalerMemBytes == 0 && plainBefore == plainAfter, "chain: upscaler off gives the same pixels as before it ran");

                shaders.use(0);
                for (U32 pr : {sceneProg, motionProg, copyProg, toneProg}) shaders.deleteProgramAndObjects(pr);
                for (U32 f : {sceneFb, motionFb, reactiveFb, outFb, postFb, plainFb}) fb.destroyFramebuffer(f);
                const U32 names[] = {colorTex, depthTex, motionTex, reactiveTex, outputTex, postTex, plainTex};
                textures.destroy(7, names);
                VK_CHECK(vkDeviceWaitIdle(ctx.device));
            }
            // Texture LOD bias for the upscaler: sampled (not render-target) 2D textures with mip filtering get the bias, also
            // through textureLod; render targets do not; 0 restores the plain sampler.
            {
                auto makeMipped = [&]() {
                    U32 name; textures.generate(1, &name); textures.bind(glc::TEXTURE_2D, name);
                    for (int level = 0; level < 5; ++level)
                    {
                        const U32 size = 16u >> level;
                        std::vector<uint8_t> texels(size * size * 4);
                        for (U32 i = 0; i < size * size; ++i) { texels[i * 4] = uint8_t(level * 50); texels[i * 4 + 3] = 255; }
                        textures.image2D(glc::TEXTURE_2D, level, glc::RGBA8, size, size, glc::RGBA, glc::UNSIGNED_BYTE, texels.data());
                    }
                    textures.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST_MIPMAP_NEAREST);
                    return name;
                };
                const U32 sampled = makeMipped(), targetTex = makeMipped();
                U32 probeTex; textures.generate(1, &probeTex); textures.bind(glc::TEXTURE_2D, probeTex);
                textures.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 4, 4, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
                U32 probeFb = fb.createFramebuffer(); fb.bind(probeFb); fb.attach(probeFb, 0x8CE0, glc::TEXTURE_2D, probeTex, 0);
                U32 targetFb = fb.createFramebuffer(); fb.bind(targetFb); fb.attach(targetFb, 0x8CE0, glc::TEXTURE_2D, targetTex, 0);
                fb.bind(0);
                U32 lodProg = shaders.createProgram();
                const char* lodSources[] = {"#version 330\nin vec4 position;\nvoid main(){gl_Position=position;}",
                                            "#version 330\nuniform sampler2D tex;\nout vec4 color;\nvoid main(){color=textureLod(tex,vec2(0.5),2.0);}"};
                for (int i = 0; i < 2; ++i)
                {
                    U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31); std::string log;
                    check(shaders.compileObject(object, 1, &lodSources[i], log), "LOD bias probe shader accepted"); shaders.attachObject(lodProg, object);
                }
                shaders.bindAttribLocation(lodProg, 0, "position");
                check(shaders.link(lodProg), "LOD bias probe links");
                check(ctx.beginFrame(clear), "LOD bias target frame acquired");    // rendering into level 0 makes it a render target
                fb.bind(targetFb); state.setViewport(0, 0, 16, 16); fb.setClearColor(0, 0, 0, 1); fb.clear(0x4000);
                fb.bind(0); ctx.endFrame(false);
                auto levelRead = [&](U32 tex) {
                    check(ctx.beginFrame(clear), "LOD bias frame acquired");
                    fb.bind(probeFb); state.setViewport(0, 0, 4, 4);
                    shaders.use(lodProg); shaders.uniform1i(lodProg, shaders.getUniformLocation(lodProg, "tex"), 0);
                    textures.activateUnit(0); textures.bind(glc::TEXTURE_2D, tex);
                    vb.drawArrays(0, 6, 3);
                    uint8_t px[4]{}; fb.readPixels(1, 1, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, px);
                    fb.bind(0); ctx.endFrame(false);
                    return int(px[0]) / 50;
                };
                const int plain = levelRead(sampled);
                textures.setLodBias(-1.0f);
                const int biased = levelRead(sampled), target = levelRead(targetTex);
                textures.setLodBias(0.0f);
                const int restored = levelRead(sampled);
                printf("  LOD bias: level %d plain, %d with -1, render target %d, %d restored\n", plain, biased, target, restored);
                check(plain == 2 && biased == 1 && target == 2 && restored == 2,
                      "LOD bias moves sampled textures one level, leaves render targets, and 0 restores");
                shaders.use(0); shaders.deleteProgramAndObjects(lodProg);
                fb.destroyFramebuffer(probeFb); fb.destroyFramebuffer(targetFb);
                const U32 names[] = {sampled, targetTex, probeTex};
                textures.destroy(3, names);
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
