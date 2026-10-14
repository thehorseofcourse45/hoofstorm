// Vulkan skeleton: draws through the viewer's backend interfaces.
//
//   LLVertexBufferBackend (buffers, vertex input, draws)   <- llvertexbufferbackend.h
//   LLRenderStateBackend  (fixed pipeline state)           <- llrenderstatebackend.h
//
// Everything the "render code" below does goes through those two interfaces, in the same call pattern the viewer's
// LLVertexBuffer and LLRender use. The Vulkan implementations are in vkbackends.cpp.
//
// Two scenes:
//   triangle  one triangle with a hand-written shader (the original skeleton test)
//   ui        two overlapping textured quads drawn with a REAL viewer program ("UI Shader"): its GLSL comes from the shader dump
//             (FS_SHADER_DUMP_DIR), is merged and translated by LLShaderTranslator and compiled by glslang (LLShaderCompiler);
//             a 2x2 texture, the program's uniform block, a depth buffer and VK_EXT_depth_clip_control (GL's -1..1 clip depth)
//
//   vulkan_skeleton.exe                    open a window and draw until closed (ui scene if --dump is given, else triangle)
//   vulkan_skeleton.exe --test --dump D    render both scenes, read the image back, check pixels, exit 0 (pass) or 1 (fail)
//   --dump DIR                             shader dump directory (manifest.txt + objects), enables the ui scene
//   --program NAME                         manifest program for the ui scene (default "UI Shader")
//   --scene triangle|ui|textures                    which scene to draw in interactive mode
//   --link-all                             every program of the dump through the shader backend (link, reflect, blob round trip), no drawing
//   --no-depth-clip-control                do not use VK_EXT_depth_clip_control: shows what happens to GL's clip depth range
//   --frames N                             frames to render per scene in --test mode (default 3)
//   --no-validation                        do not enable the Khronos validation layer
#include "vkbackends.h"
#include "vkprogram.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include "shaderdump.h"

#include <GL/gl.h>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <string>

static bool gQuit = false;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CLOSE:
    case WM_DESTROY:
        gQuit = true;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static std::string exe_dir()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

static std::vector<uint32_t> load_spirv(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
    {
        fprintf(stderr, "FATAL: cannot open %s\n", path.c_str());
        abort();
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> code((size + 3) / 4);
    fread(code.data(), 1, size, f);
    fclose(f);
    return code;
}

struct Pixel { int r, g, b; };

static Pixel pixel_at(const VkContext& ctx, int x, int y)
{
    // swapchain format is B8G8R8A8_UNORM
    const uint8_t* p = ctx.readMapped + ((size_t) y * ctx.extent.width + x) * 4;
    return {p[2], p[1], p[0]};
}

// pixel at a point given in GL normalized device coordinates (Y up)
static Pixel pixel_ndc(const VkContext& ctx, float ndcX, float ndcY)
{
    return pixel_at(ctx, (int) ((ndcX + 1.0f) * 0.5f * (float) ctx.extent.width), (int) ((1.0f - ndcY) * 0.5f * (float) ctx.extent.height));
}

static bool check(const char* what, Pixel got, int r, int g, int b, int tol)
{
    const bool ok = std::abs(got.r - r) <= tol && std::abs(got.g - g) <= tol && std::abs(got.b - b) <= tol;
    printf("  [%s] %-52s got (%3d,%3d,%3d) expected (%3d,%3d,%3d)\n", ok ? "PASS" : "FAIL", what, got.r, got.g, got.b, r, g, b);
    return ok;
}

// The frame loop of a scene. 'draw' records the frame between beginFrame and endFrame; in test mode the image of the last
// frame is read back and handed to 'verify'.
static bool render_frames(VkContext& ctx, bool test, int testFrames, const float clear[4], const std::function<void()>& draw,
                          const std::function<bool()>& verify)
{
    int frame = 0;
    while (!gQuit)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (gQuit)
        {
            break;
        }

        if (!ctx.beginFrame(clear))
        {
            Sleep(10); // minimized or recreating; keep pumping window messages
            continue;
        }
        draw();

        const bool readBack = test && frame == testFrames - 1;
        ctx.endFrame(readBack);
        if (readBack)
        {
            VK_CHECK(vkWaitForFences(ctx.device, 1, &ctx.slot().fence, VK_TRUE, UINT64_MAX));
            return verify();
        }
        ++frame;
    }
    return true;
}

static void set_common_state(VkContext& ctx, LLRenderStateBackend& st, bool depth, bool blend)
{
    // What LLGLState, LLRender and LLVertexBuffer::setBuffer() would do before a draw:
    st.setViewport(0, 0, (S32) ctx.extent.width, (S32) ctx.extent.height);
    st.setCapability(GL_SCISSOR_TEST, false);
    st.setCapability(GL_CULL_FACE, true);
    st.setCullFace(GL_BACK);
    st.setCapability(GL_BLEND, blend);
    st.setBlendFunc(7, 9, 7, 9); // LLRender::BT_ALPHA
    st.setColorMask(true, true, true, true);
    st.setPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    st.setCapability(GL_DEPTH_TEST, depth);
    st.setDepthFunc(GL_LESS);
    st.setDepthMask(true);
}

// Attribute vocabulary of LLVertexBuffer (see vkbackends.cpp)
constexpr U32 TYPE_VERTEX = 0, TYPE_TEXCOORD0 = 2, TYPE_COLOR = 6;

// ---- scene 1: a triangle with a hand-written shader ------------------------------------------------------------------
static bool scene_triangle(VkContext& ctx, LLRenderStateBackend& st, LLVertexBufferBackend& vb, bool test, int testFrames)
{
    printf("scene: triangle (hand-written shader)\n");
    const std::string dir = exe_dir();
    VkProgram* program = VkProgram::create(ctx, load_spirv(dir + "tri.vert.spv"), load_spirv(dir + "tri.frag.spv"), {}, {});
    ctx.program = program;

    // the data, laid out the way LLVertexBuffer::calcOffsets() lays it out: one array per attribute
    U32 offsets[14] = {};
    offsets[TYPE_VERTEX] = 0;   // 3 vertices * 16 bytes (LLVector4)
    offsets[TYPE_COLOR] = 48;   // 3 colors * 4 bytes (LLColor4U)
    const U32 vertexBytes = 48 + 12;

    BufferHandle vbo, ibo;
    U8* vertexCpu = nullptr;
    U8* indexCpu = nullptr;
    vb.allocate(BufferKind::Vertex, 64, vbo, vertexCpu);
    vb.allocate(BufferKind::Index, 8, ibo, indexCpu);

    // GL orientation: counter-clockwise with Y up. Red bottom-left, green bottom-right, blue top.
    const float positions[3][4] = {{-0.5f, -0.5f, 0.0f, 1.0f}, {0.5f, -0.5f, 0.0f, 1.0f}, {0.0f, 0.5f, 0.0f, 1.0f}};
    const U8 colors[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    const U16 indices[3] = {0, 1, 2};
    memcpy(vertexCpu + offsets[TYPE_VERTEX], positions, sizeof(positions));
    memcpy(vertexCpu + offsets[TYPE_COLOR], colors, sizeof(colors));
    memcpy(indexCpu, indices, sizeof(indices));

    // LLVertexBuffer::unmapBuffer(): bind the buffer, then upload the dirty range (end is inclusive)
    vb.bind(BufferKind::Vertex, vbo);
    vb.upload(BufferKind::Vertex, vbo, 0, vertexBytes - 1, vertexCpu);
    vb.bind(BufferKind::Index, ibo);
    vb.upload(BufferKind::Index, ibo, 0, sizeof(indices) - 1, indexCpu);
    vb.unbind();

    const float clear[4] = {0.1f, 0.1f, 0.2f, 1.0f};
    bool passed = render_frames(ctx, test, testFrames, clear,
        [&] {
            set_common_state(ctx, st, false, true);
            vb.setBuffers(vbo, ibo, (1u << TYPE_VERTEX) | (1u << TYPE_COLOR), offsets);
            vb.drawIndexed(0 /* LLRender::TRIANGLES */, 0, 2, 3, IndexType::U16, 0);
        },
        [&] {
            // expected values are computed from the barycentric weights at each sample point
            bool ok = true;
            ok &= check("corner is the clear color", pixel_at(ctx, 5, 5), 26, 26, 51, 3);
            ok &= check("empty area right of the triangle is clear", pixel_ndc(ctx, 0.45f, 0.40f), 26, 26, 51, 3);
            ok &= check("near the top vertex it is blue", pixel_ndc(ctx, 0.0f, 0.40f), 13, 13, 230, 25);
            ok &= check("near the bottom-left vertex it is red", pixel_ndc(ctx, -0.40f, -0.45f), 223, 19, 13, 25);
            ok &= check("near the bottom-right vertex it is green", pixel_ndc(ctx, 0.40f, -0.45f), 19, 223, 13, 25);
            return ok;
        });

    vkDeviceWaitIdle(ctx.device);
    vb.free(BufferKind::Vertex, 64, vbo, vertexCpu);
    vb.free(BufferKind::Index, 8, ibo, indexCpu);
    ctx.program = nullptr;
    program->destroy();
    return passed;
}

// ---- scene 2: a real viewer program, a texture, a uniform block, depth ------------------------------------------------

// A program as LLGLSLShader sees it after createShader(), mapAttributes() and mapUniforms().
struct MappedProgram
{
    U32 id = 0;
    std::map<std::string, S32> location;    // uniform name (without "[0]") -> location
    std::map<std::string, int> channel;     // sampler name -> texture channel
    int activeUniforms = 0;
};

// The binary the program cache would keep (LLShaderMgr::saveCachedProgramBinary / loadCachedProgramBinary)
struct CachedBinary
{
    std::vector<U8> data;
    U32 format = 0;
};

static bool is_sampler_type(U32 t)
{
    return (t >= 0x8B5D && t <= 0x8B64) || t == 0x9108 || t == 0x900C || t == 0x8DC1;    // see LLGLSLShader::mapUniformTextureChannel
}

static void map_uniforms(LLShaderBackend& be, MappedProgram& out);

// The calls LLGLSLShader::createShader() makes, in the same order, against the backend interface only.
static bool create_program(LLShaderBackend& be, const std::string& dumpDir, const shaderdump::ManifestProgram& entry,
                           const CachedBinary* load, CachedBinary* save, MappedProgram& out)
{
    out = MappedProgram();
    out.id = be.createProgram();

    bool usingBinary = load && be.loadProgramBinary(out.id, load->format, load->data.data(), (S32) load->data.size());
    if (load && !usingBinary)
    {
        fprintf(stderr, "  cached binary rejected\n");
        return false;
    }
    if (!usingBinary)
    {
        // LLShaderMgr::loadShaderFile + attachObject: one shader object per source file
        for (const LLShaderCompiler::Stage& stage : shaderdump::loadStages(dumpDir, entry))
        {
            const U32 glStage = stage.stage == LLShaderTranslator::Stage::Vertex ? 0x8B31
                              : stage.stage == LLShaderTranslator::Stage::Fragment ? 0x8B30 : 0x8DD9;
            for (const std::string& source : stage.objects)
            {
                const U32 object = be.createObject(glStage);
                const char* text = source.c_str();
                std::string log;
                if (!object || !be.compileObject(object, 1, &text, log))
                {
                    fprintf(stderr, "  compile failed: %s\n", log.c_str());
                    return false;
                }
                be.attachObject(out.id, object);
            }
        }
        // LLGLSLShader::mapAttributes: reserved attributes keep fixed locations, then link
        const auto& attribs = LLShaderCompiler::viewerOptions().attribNames;
        for (U32 i = 0; i < attribs.size(); ++i)
        {
            be.bindAttribLocation(out.id, i, attribs[i].c_str());
        }
        if (!be.link(out.id))
        {
            fprintf(stderr, "  link failed: %s\n", be.getProgramLog(out.id).c_str());
            return false;
        }
        if (save)
        {
            save->data.resize((size_t) be.getProgramBinaryLength(out.id));
            if (!be.getProgramBinary(out.id, save->data, save->format))
            {
                fprintf(stderr, "  getProgramBinary failed\n");
                return false;
            }
        }
    }

    map_uniforms(be, out);
    return true;
}

// LLGLSLShader::mapUniforms: enumerate the active uniforms, look up locations, give samplers their texture channels
static void map_uniforms(LLShaderBackend& be, MappedProgram& out)
{
    out.activeUniforms = be.getActiveUniformCount(out.id);
    int channels = 0;
    for (S32 i = 0; i < out.activeUniforms; ++i)
    {
        std::string name;
        U32 type = 0;
        S32 size = 0;
        be.getActiveUniform(out.id, i, name, type, size);
        const S32 loc = be.getUniformLocation(out.id, name.c_str());
        if (name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0)
        {
            name.resize(name.size() - 3);
        }
        out.location[name] = loc;
        if (loc != -1 && is_sampler_type(type))
        {
            out.channel[name] = channels;
            be.uniform1i(out.id, loc, channels++);
        }
    }
}

static bool scene_ui(VkContext& ctx, LLRenderStateBackend& st, LLVertexBufferBackend& vb, VulkanShaderBackend& shaders,
                     LLTextureBackend& tb, bool test, int testFrames, const std::string& dumpDir, const std::string& programName, const CachedBinary* load,
                     CachedBinary* save)
{
    printf("scene: ui (viewer program \"%s\", %s)\n", programName.c_str(), load ? "from the SPIR-V cache" : "compiled from the dump");
    LLShaderBackend& be = shaders;      // from here on, only the viewer's interface is used

    // 1. the program, through the backend interface
    const std::vector<shaderdump::ManifestProgram> manifest = shaderdump::readManifest(dumpDir);
    const shaderdump::ManifestProgram* entry = nullptr;
    for (const auto& p : manifest)
    {
        if (p.name == programName)
        {
            entry = &p;
            break;
        }
    }
    if (!entry)
    {
        fprintf(stderr, "  program \"%s\" is not in %s/manifest.txt\n", programName.c_str(), dumpDir.c_str());
        return false;
    }
    MappedProgram prog;
    if (!create_program(be, dumpDir, *entry, load, save, prog))
    {
        return false;
    }
    if (load)
    {
        printf("  loaded the binary (%zu bytes) in %.2f ms\n", load->data.size(), shaders.lastLoadMs);
    }
    else
    {
        printf("  preprocess + translate + compile + link in %.1f ms%s\n", shaders.lastLinkMs,
               save ? (", binary " + std::to_string(save->data.size()) + " bytes").c_str() : "");
    }

    // what the viewer would learn from the program: uniforms, types, attribute locations
    bool passed = true;
    auto expect = [&](const char* what, bool ok) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        passed &= ok;
    };
    U32 type = 0;
    {
        std::string name;
        S32 size;
        for (S32 i = 0; i < prog.activeUniforms; ++i)
        {
            U32 t;
            be.getActiveUniform(prog.id, i, name, t, size);
            if (name == "modelview_projection_matrix")
            {
                type = t;
            }
        }
    }
    expect("5 block members + 1 sampler are active uniforms", prog.activeUniforms == 6);
    expect("modelview_projection_matrix reports GL_FLOAT_MAT4", type == 0x8B5C);
    expect("diffuseMap is a sampler with texture channel 0", prog.channel.count("diffuseMap") && prog.channel["diffuseMap"] == 0);
    expect("attribute locations: position 0, texcoord0 2, diffuse_color 6, normal unused",
           be.getAttribLocation(prog.id, "position") == 0 && be.getAttribLocation(prog.id, "texcoord0") == 2 &&
           be.getAttribLocation(prog.id, "diffuse_color") == 6 && be.getAttribLocation(prog.id, "normal") == -1);

    // 2. the texture sits in texture unit 0 (what LLTexUnit::bind does); the sampler uniform set above points the program at it
    const uint8_t texels[16] = {255, 0, 0, 255,   0, 255, 0, 255,    0, 0, 255, 255,   255, 255, 0, 255};   // red green / blue yellow
    U32 texture = 0;
    tb.generate(1, &texture);
    tb.activateUnit(prog.channel["diffuseMap"]);
    tb.bind(glc::TEXTURE_2D, texture);
    tb.setWrap(glc::TEXTURE_2D, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, -1);
    tb.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
    tb.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 2, 2, glc::RGBA, glc::UNSIGNED_BYTE, texels);

    // 3. uniforms, by location, with the same calls LLGLSLShader::uniformMatrix4fv & co. make
    const float identity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    const float scale2[16] = {2, 0, 0, 0,  0, 2, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};   // texture coordinates 0..0.5 become 0..1
    be.uniformMatrix4fv(prog.id, prog.location["modelview_projection_matrix"], 1, false, identity);
    be.uniformMatrix4fv(prog.id, prog.location["texture_matrix0"], 1, false, scale2);
    be.uniform1f(prog.id, prog.location["mirror_flag"], 0.0f);
    be.use(prog.id);

    // 4. geometry: two quads, each as one array per attribute. The NEAR quad (z = -0.5) is drawn FIRST and the FAR quad
    //    (z = +0.5, GL clip space) second: with the depth test the near quad must stay on top where they overlap.
    U32 offsets[14] = {};
    offsets[TYPE_VERTEX] = 0;       // 8 vertices * 16 bytes
    offsets[TYPE_TEXCOORD0] = 128;  // 8 * 8 bytes
    offsets[TYPE_COLOR] = 192;      // 8 * 4 bytes
    const U32 vertexBytes = 192 + 32;

    BufferHandle vbo, ibo;
    U8* vertexCpu = nullptr;
    U8* indexCpu = nullptr;
    vb.allocate(BufferKind::Vertex, 224, vbo, vertexCpu);
    vb.allocate(BufferKind::Index, 24, ibo, indexCpu);

    auto quad = [&](int first, float x0, float y0, float x1, float y1, float z, U8 c) {
        const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};          // counter-clockwise
        const float uv[4][2] = {{0, 0}, {0.5f, 0}, {0.5f, 0.5f}, {0, 0.5f}};
        for (int i = 0; i < 4; ++i)
        {
            const float pos[4] = {corners[i][0], corners[i][1], z, 1.0f};
            const U8 col[4] = {c, c, c, 255};
            memcpy(vertexCpu + offsets[TYPE_VERTEX] + (first + i) * 16, pos, 16);
            memcpy(vertexCpu + offsets[TYPE_TEXCOORD0] + (first + i) * 8, uv[i], 8);
            memcpy(vertexCpu + offsets[TYPE_COLOR] + (first + i) * 4, col, 4);
        }
    };
    quad(0, -0.2f, -0.2f, 0.8f, 0.8f, -0.5f, 255);   // near, white
    quad(4, -0.8f, -0.8f, 0.2f, 0.2f, 0.5f, 128);    // far, gray
    const U16 indices[12] = {0, 1, 2, 0, 2, 3,  4, 5, 6, 4, 6, 7};
    memcpy(indexCpu, indices, sizeof(indices));

    vb.bind(BufferKind::Vertex, vbo);
    vb.upload(BufferKind::Vertex, vbo, 0, vertexBytes - 1, vertexCpu);
    vb.bind(BufferKind::Index, ibo);
    vb.upload(BufferKind::Index, ibo, 0, sizeof(indices) - 1, indexCpu);
    vb.unbind();

    const float clear[4] = {0.1f, 0.1f, 0.2f, 1.0f};
    const bool clipControl = ctx.depthClipControl;
    passed &= render_frames(ctx, test, testFrames, clear,
        [&] {
            set_common_state(ctx, st, true, false);
            // the attribute mask the program reads: position, texcoord0, diffuse_color (the shader's attribute table)
            const U32 mask = (1u << TYPE_VERTEX) | (1u << TYPE_TEXCOORD0) | (1u << TYPE_COLOR);
            vb.setBuffers(vbo, ibo, mask, offsets);
            vb.drawIndexed(0 /* TRIANGLES */, 0, 3, 6, IndexType::U16, 0);     // near quad
            vb.drawIndexed(0 /* TRIANGLES */, 4, 7, 6, IndexType::U16, 12);    // far quad
        },
        [&] {
            bool ok = true;
            printf("  depth clip control %s\n", clipControl ? "ON: GL clip depth -1..1 is used as is" : "OFF: Vulkan clips z < 0");
            ok &= check("corner is the clear color", pixel_at(ctx, 5, 5), 26, 26, 51, 3);
            // the far quad alone: texel x vertex color 128
            ok &= check("far quad only, red texel x gray", pixel_ndc(ctx, -0.5f, -0.5f), 128, 0, 0, 3);
            ok &= check("far quad only, blue texel x gray", pixel_ndc(ctx, -0.5f, 0.05f), 0, 0, 128, 3);
            if (clipControl)
            {
                // the near quad, one texel each (white vertex color): sampler, texcoords, texture matrix and uniform block work
                ok &= check("near quad, red texel (overlap: depth test wins)", pixel_ndc(ctx, 0.0f, 0.0f), 255, 0, 0, 3);
                ok &= check("near quad, green texel", pixel_ndc(ctx, 0.55f, -0.05f), 0, 255, 0, 3);
                ok &= check("near quad, blue texel", pixel_ndc(ctx, 0.05f, 0.55f), 0, 0, 255, 3);
                ok &= check("near quad, yellow texel", pixel_ndc(ctx, 0.55f, 0.55f), 255, 255, 0, 3);
            }
            else
            {
                // z = -0.5 is outside Vulkan's 0..1 clip range: the near quad is gone, only the far quad is left
                ok &= check("near quad is clipped: overlap shows the far quad", pixel_ndc(ctx, 0.0f, 0.0f), 128, 128, 0, 3);
                ok &= check("near quad is clipped: its own area is clear", pixel_ndc(ctx, 0.55f, 0.55f), 26, 26, 51, 3);
            }
            return ok;
        });

    vkDeviceWaitIdle(ctx.device);
    vb.free(BufferKind::Vertex, 224, vbo, vertexCpu);
    vb.free(BufferKind::Index, 24, ibo, indexCpu);
    be.use(0);
    tb.destroy(1, &texture);
    be.deleteProgramAndObjects(prog.id);
    return passed;
}

// ---- scene 3: the texture backend, driven through LLTextureBackend only -----------------------------------------------------
// Two kinds of checks: pixels drawn with the viewer's UI shader (mips, filters, wrap modes, formats, swizzle, BC1, fallback) or with a
// small samplerCubeArray shader (cube map array layers and faces), and CPU-side API checks (readback, sub images, row length,
// half floats, level queries, replaceWithMipLevel).

namespace
{
struct Probe
{
    std::string name;
    int x, y, w, h;             // pixel rectangle of the quad (origin top-left)
    float u0, v0, u1, v1;       // texture coordinates of its corners (u0,v0 bottom-left)
    int sx, sy;                 // pixel inside the quad that is checked
    int r, g, b, tol;
    std::function<void()> setup;    // texture unit state for this draw
};

// a program built from GLSL given as text, the way the viewer hands shader objects to the backend
bool create_program_from_source(LLShaderBackend& be, const char* vsSource, const char* fsSource, MappedProgram& out)
{
    out = MappedProgram();
    out.id = be.createProgram();
    const struct { U32 stage; const char* source; } objects[2] = { { 0x8B31, vsSource }, { 0x8B30, fsSource } };
    for (const auto& o : objects)
    {
        const U32 object = be.createObject(o.stage);
        std::string log;
        if (!object || !be.compileObject(object, 1, &o.source, log))
        {
            fprintf(stderr, "  compile failed: %s\n", log.c_str());
            return false;
        }
        be.attachObject(out.id, object);
    }
    const auto& attribs = LLShaderCompiler::viewerOptions().attribNames;
    for (U32 i = 0; i < attribs.size(); ++i)
    {
        be.bindAttribLocation(out.id, i, attribs[i].c_str());
    }
    if (!be.link(out.id))
    {
        fprintf(stderr, "  link failed: %s\n", be.getProgramLog(out.id).c_str());
        return false;
    }
    map_uniforms(be, out);
    return true;
}
} // namespace

static bool scene_textures(VkContext& ctx, LLRenderStateBackend& st, LLVertexBufferBackend& vb, VulkanShaderBackend& shaders,
                           VulkanTextureBackend& textures, bool test, int testFrames, const std::string& dumpDir)
{
    printf("scene: textures (LLTextureBackend on the GPU)\n");
    LLShaderBackend& be = shaders;
    LLTextureBackend& tb = textures;
    bool passed = true;
    auto expect = [&](const char* what, bool ok) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        passed &= ok;
    };

    // ---- programs: the viewer's UI shader, and a cube map array lookup
    const std::vector<shaderdump::ManifestProgram> manifest = shaderdump::readManifest(dumpDir);
    const shaderdump::ManifestProgram* entry = nullptr;
    for (const auto& p : manifest)
    {
        if (p.name == "UI Shader")
        {
            entry = &p;
        }
    }
    MappedProgram ui, cubes;
    if (!entry || !create_program(be, dumpDir, *entry, nullptr, nullptr, ui))
    {
        fprintf(stderr, "  cannot build the UI Shader\n");
        return false;
    }
    const char* cubeVs =
        "#version 420\n"
        "uniform mat4 modelview_projection_matrix;\n"
        "in vec3 position;\n"
        "void main() { gl_Position = modelview_projection_matrix * vec4(position, 1.0); }\n";
    const char* cubeFs =
        "#version 420\n"
        "uniform samplerCubeArray probes;\n"
        "uniform vec3 dir;\n"
        "uniform float layer;\n"
        "out vec4 frag_color;\n"
        "void main() { frag_color = textureLod(probes, vec4(dir, layer), 0.0); }\n";
    if (!create_program_from_source(be, cubeVs, cubeFs, cubes))
    {
        return false;
    }
    const float identity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    be.uniformMatrix4fv(ui.id, ui.location["modelview_projection_matrix"], 1, false, identity);
    be.uniformMatrix4fv(ui.id, ui.location["texture_matrix0"], 1, false, identity);
    be.uniform1f(ui.id, ui.location["mirror_flag"], 0.0f);
    be.uniformMatrix4fv(cubes.id, cubes.location["modelview_projection_matrix"], 1, false, identity);
    const int uiUnit = ui.channel["diffuseMap"], cubeUnit = cubes.channel["probes"];

    auto make = [&](U32& name, U32 target) {
        tb.generate(1, &name);
        tb.activateUnit(0);
        tb.bind(target, name);
    };
    auto solid = [](int n, U8 r, U8 g, U8 b, U8 a, std::vector<U8>& v) {
        v.resize((size_t) n * 4);
        for (int i = 0; i < n; ++i) { v[i * 4] = r; v[i * 4 + 1] = g; v[i * 4 + 2] = b; v[i * 4 + 3] = a; }
    };

    // ---- textures
    // 1. an 8x8 texture whose four mip levels are uploaded one by one, each a different solid color
    U32 mips = 0;
    make(mips, glc::TEXTURE_2D);
    tb.setWrap(glc::TEXTURE_2D, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, -1);
    tb.setFilter(glc::TEXTURE_2D, glc::LINEAR, glc::LINEAR_MIPMAP_NEAREST);
    const U8 levelColor[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    for (int l = 0; l < 4; ++l)
    {
        std::vector<U8> px;
        solid((8 >> l) * (8 >> l), levelColor[l][0], levelColor[l][1], levelColor[l][2], 255, px);
        tb.image2D(glc::TEXTURE_2D, l, glc::RGBA8, 8 >> l, 8 >> l, glc::RGBA, glc::UNSIGNED_BYTE, px.data());
    }
    tb.setLevelRange(glc::TEXTURE_2D, 0, 3);

    // 2. level 0 only (left half red, right half blue), the rest from generateMipmaps()
    U32 autoMips = 0;
    make(autoMips, glc::TEXTURE_2D);
    tb.setWrap(glc::TEXTURE_2D, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, -1);
    tb.setFilter(glc::TEXTURE_2D, glc::LINEAR, glc::LINEAR_MIPMAP_NEAREST);
    {
        std::vector<U8> px(8 * 8 * 4);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
            {
                U8* p = &px[(y * 8 + x) * 4];
                p[0] = x < 4 ? 255 : 0; p[1] = 0; p[2] = x < 4 ? 0 : 255; p[3] = 255;
            }
        tb.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 8, 8, glc::RGBA, glc::UNSIGNED_BYTE, px.data());
        tb.generateMipmaps(glc::TEXTURE_2D);
    }

    // 3. 2x1: red, green. Filters and wrap modes are switched between draws.
    U32 pair = 0;
    make(pair, glc::TEXTURE_2D);
    {
        const U8 px[8] = {255, 0, 0, 255,  0, 255, 0, 255};
        tb.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 2, 1, glc::RGBA, glc::UNSIGNED_BYTE, px);
    }

    // 4. RGB bytes into an RGBA image
    U32 rgb = 0;
    make(rgb, glc::TEXTURE_2D);
    tb.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
    {
        const U8 px[12] = {10, 200, 30,  1, 2, 3,  4, 5, 6,  7, 8, 9};     // texel (0,0) = (10,200,30)
        tb.image2D(glc::TEXTURE_2D, 0, glc::RGB8, 2, 2, glc::RGB, glc::UNSIGNED_BYTE, px);
    }

    // 5. one red byte, swizzled to gray with alpha one (the viewer's GL_LUMINANCE emulation)
    U32 lum = 0;
    make(lum, glc::TEXTURE_2D);
    tb.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
    {
        const S32 swizzle[4] = {(S32) glc::RED, (S32) glc::RED, (S32) glc::RED, (S32) glc::ONE};
        tb.setSwizzle(glc::TEXTURE_2D, swizzle);
        const U8 px[1] = {128};
        tb.image2D(glc::TEXTURE_2D, 0, glc::R8, 1, 1, glc::RED, glc::UNSIGNED_BYTE, px);
    }

    // 6. a pre-compressed BC1 block: every texel is color0 = 565 red
    U32 bc1 = 0;
    make(bc1, glc::TEXTURE_2D);
    tb.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
    const U8 bc1Block[8] = {0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    tb.compressedImage2D(glc::TEXTURE_2D, 0, glc::COMPRESSED_RGB_S3TC_DXT1, 4, 4, 8, bc1Block);

    // 7. a cube map: bound to a unit that a 2D sampler reads, so the draw must fall back to white
    U32 cubeMap = 0;
    make(cubeMap, glc::TEXTURE_CUBE_MAP);
    {
        const U8 px[4] = {255, 0, 255, 255};
        for (U32 f = 0; f < 6; ++f)
        {
            tb.image2D(glc::TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, glc::RGBA8, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, px);
        }
    }

    // 8. a cube map array: 2 cubes, every face of every cube a different solid color
    U32 cubeArray = 0;
    make(cubeArray, glc::TEXTURE_CUBE_MAP_ARRAY);
    tb.setWrap(glc::TEXTURE_CUBE_MAP_ARRAY, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE);
    tb.setFilter(glc::TEXTURE_CUBE_MAP_ARRAY, glc::NEAREST, glc::NEAREST);
    tb.image3D(glc::TEXTURE_CUBE_MAP_ARRAY, 0, glc::RGBA8, 2, 2, 2 * 6, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
    auto faceColor = [](int layer, int face, U8 c[3]) { c[0] = (U8) (40 + layer * 100); c[1] = (U8) (30 + face * 40); c[2] = (U8) (200 - face * 20); };
    for (int layer = 0; layer < 2; ++layer)
        for (int face = 0; face < 6; ++face)
        {
            U8 c[3];
            faceColor(layer, face, c);
            std::vector<U8> px;
            solid(4, c[0], c[1], c[2], 255, px);
            tb.subImage3D(glc::TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, layer * 6 + face, 2, 2, 1, glc::RGBA, glc::UNSIGNED_BYTE, px.data());
        }

    // ---- API checks that need no drawing
    {
        // sub images, row length, readback
        U32 t = 0;
        make(t, glc::TEXTURE_2D);
        std::vector<U8> base(4 * 4 * 4);
        for (int i = 0; i < 16; ++i) { base[i * 4] = (U8) (i * 10); base[i * 4 + 1] = (U8) i; base[i * 4 + 2] = 7; base[i * 4 + 3] = 255; }
        tb.image2D(glc::TEXTURE_2D, 0, glc::RGBA8, 4, 4, glc::RGBA, glc::UNSIGNED_BYTE, base.data());
        std::vector<U8> back(4 * 4 * 4);
        tb.readImage(glc::TEXTURE_2D, 0, glc::RGBA, glc::UNSIGNED_BYTE, back.data());
        expect("readImage returns what image2D uploaded", back == base);

        // upload a 2x2 rectangle to (1,1) from a source whose rows are 4 pixels apart: source pixel (x,y) lands at (1+x, 1+y)
        std::vector<U8> expected = base;
        std::vector<U8> src(4 * 4 * 4);
        for (int i = 0; i < 16; ++i) { src[i * 4] = 100; src[i * 4 + 1] = (U8) (200 + i); src[i * 4 + 2] = 9; src[i * 4 + 3] = 255; }
        tb.setUnpackRowLength(4);
        tb.subImage2D(glc::TEXTURE_2D, 0, 1, 1, 2, 2, glc::RGBA, glc::UNSIGNED_BYTE, src.data());
        tb.setUnpackRowLength(0);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                memcpy(&expected[((1 + y) * 4 + 1 + x) * 4], &src[(y * 4 + x) * 4], 4);
        tb.readImage(glc::TEXTURE_2D, 0, glc::RGBA, glc::UNSIGNED_BYTE, back.data());
        expect("subImage2D with a source row length changes only that rectangle", back == expected);

        expect("level queries: width/height of level 0, 1 and 2", tb.getLevelParameter(glc::TEXTURE_2D, 0, glc::TEXTURE_WIDTH) == 4 &&
               tb.getLevelParameter(glc::TEXTURE_2D, 1, glc::TEXTURE_WIDTH) == 2 && tb.getLevelParameter(glc::TEXTURE_2D, 2, glc::TEXTURE_HEIGHT) == 1 &&
               tb.getLevelParameter(glc::TEXTURE_2D, 0, glc::TEXTURE_COMPRESSED) == 0);
        tb.destroy(1, &t);
    }
    {
        // float data into a half float image and back
        U32 t = 0;
        make(t, glc::TEXTURE_2D);
        const float px[8] = {0.5f, 1.0f, 0.25f, 2.0f,  0.0f, -1.0f, 100.0f, 0.125f};
        tb.image2D(glc::TEXTURE_2D, 0, glc::RGBA16F, 2, 1, glc::RGBA, glc::FLOAT, px);
        float back[8] = {};
        tb.readImage(glc::TEXTURE_2D, 0, glc::RGBA, glc::FLOAT, back);
        expect("RGBA16F: float in, float out (all values exact in half precision)", memcmp(px, back, sizeof(px)) == 0);
        tb.destroy(1, &t);
    }
    {
        // BC1 readback and size queries
        tb.activateUnit(0);
        tb.bind(glc::TEXTURE_2D, bc1);
        U8 back[8] = {};
        tb.readCompressedImage(glc::TEXTURE_2D, 0, back);
        expect("BC1: the compressed block reads back unchanged", memcmp(back, bc1Block, 8) == 0 &&
               tb.getLevelParameter(glc::TEXTURE_2D, 0, glc::TEXTURE_COMPRESSED) == 1 &&
               tb.getLevelParameter(glc::TEXTURE_2D, 0, glc::TEXTURE_COMPRESSED_IMAGE_SIZE) == 8);
    }
    {
        // replaceWithMipLevel: level 2 of the mip texture (2x2 blue) becomes level 0 of a 2x2 texture
        U32 t = 0;
        make(t, glc::TEXTURE_2D);
        for (int l = 0; l < 4; ++l)
        {
            std::vector<U8> px;
            solid((8 >> l) * (8 >> l), levelColor[l][0], levelColor[l][1], levelColor[l][2], 255, px);
            tb.image2D(glc::TEXTURE_2D, l, glc::RGBA8, 8 >> l, 8 >> l, glc::RGBA, glc::UNSIGNED_BYTE, px.data());
        }
        tb.replaceWithMipLevel(glc::TEXTURE_2D, 2, glc::RGBA8, 2, 2, glc::RGBA, glc::UNSIGNED_BYTE, 16);
        U8 back[16] = {};
        tb.readImage(glc::TEXTURE_2D, 0, glc::RGBA, glc::UNSIGNED_BYTE, back);
        bool blue = tb.getLevelParameter(glc::TEXTURE_2D, 0, glc::TEXTURE_WIDTH) == 2;
        for (int i = 0; i < 4; ++i) blue &= back[i * 4] == 0 && back[i * 4 + 1] == 0 && back[i * 4 + 2] == 255 && back[i * 4 + 3] == 255;
        expect("replaceWithMipLevel: level 2 becomes the 2x2 level 0", blue);
        tb.destroy(1, &t);
    }

    // ---- geometry: one quad per probe, drawn with the UI shader (and the cube array ones with their own)
    std::vector<Probe> probes;
    auto bindUnit = [&](U32 tex, U32 target) { tb.activateUnit(uiUnit); tb.bind(target, tex); };
    auto addMip = [&](int k) {
        const int size = 8 >> k;
        probes.push_back({"mip level " + std::to_string(k) + " is chosen by the size on screen", 10 + k * 20, 10, size, size, 0, 0, 1, 1, 0, 0,
                          levelColor[k][0], levelColor[k][1], levelColor[k][2], 2, [=] { bindUnit(mips, glc::TEXTURE_2D); }});
    };
    for (int k = 0; k < 4; ++k) addMip(k);
    probes.push_back({"generateMipmaps: level 3 is the average of red and blue", 100, 10, 1, 1, 0, 0, 1, 1, 0, 0, 128, 0, 128, 4,
                      [=] { bindUnit(autoMips, glc::TEXTURE_2D); }});
    probes.push_back({"generateMipmaps: level 2, left pixel red", 110, 10, 2, 2, 0, 0, 1, 1, 0, 0, 255, 0, 0, 3,
                      [=] { bindUnit(autoMips, glc::TEXTURE_2D); }});
    probes.push_back({"generateMipmaps: level 2, right pixel blue", 110, 10, 2, 2, 0, 0, 1, 1, 1, 0, 0, 0, 255, 3,
                      [=] { bindUnit(autoMips, glc::TEXTURE_2D); }});
    // magnification of the 2x1 texture on a 16 pixel quad: pixel 7 (u = 0.47) is inside the red texel
    probes.push_back({"point filter: hard edge", 10, 40, 16, 16, 0, 0, 1, 1, 7, 8, 255, 0, 0, 2, [&] {
        bindUnit(pair, glc::TEXTURE_2D);
        tb.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
        tb.setWrap(glc::TEXTURE_2D, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, -1);
    }});
    probes.push_back({"linear filter: blend of the two texels", 40, 40, 16, 16, 0, 0, 1, 1, 7, 8, 143, 112, 0, 4, [&] {
        bindUnit(pair, glc::TEXTURE_2D);
        tb.setFilter(glc::TEXTURE_2D, glc::LINEAR, glc::LINEAR);
        tb.setWrap(glc::TEXTURE_2D, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, -1);
    }});
    // u runs 0..2 over 32 pixels: pixel 20 is u = 1.28, pixel 26 is u = 1.66
    auto wrapProbe = [&](const char* name, int x, U32 wrap, int px, int r, int g, int b) {
        probes.push_back({name, x, 70, 32, 8, 0, 0, 2, 1, px, 4, r, g, b, 2, [&tb, bindUnit, wrap, pair] {
            bindUnit(pair, glc::TEXTURE_2D);
            tb.setFilter(glc::TEXTURE_2D, glc::NEAREST, glc::NEAREST);
            tb.setWrap(glc::TEXTURE_2D, wrap, wrap, -1);
        }});
    };
    wrapProbe("wrap repeat, u = 1.28 is red again", 10, glc::REPEAT, 20, 255, 0, 0);
    wrapProbe("wrap clamp, u = 1.28 stays green", 50, glc::CLAMP_TO_EDGE, 20, 0, 255, 0);
    wrapProbe("wrap repeat, u = 1.66 is green", 90, glc::REPEAT, 26, 0, 255, 0);
    wrapProbe("wrap mirror, u = 1.66 mirrors to red", 130, glc::MIRRORED_REPEAT, 26, 255, 0, 0);
    probes.push_back({"RGB bytes expand to RGBA", 10, 90, 8, 8, 0, 0, 0.5f, 0.5f, 2, 2, 10, 200, 30, 2, [=] { bindUnit(rgb, glc::TEXTURE_2D); }});
    probes.push_back({"R8 with a luminance swizzle is gray", 30, 90, 8, 8, 0, 0, 1, 1, 4, 4, 128, 128, 128, 2, [=] { bindUnit(lum, glc::TEXTURE_2D); }});
    probes.push_back({"BC1 block is sampled as red", 50, 90, 8, 8, 0, 0, 1, 1, 4, 4, 255, 0, 0, 3, [=] { bindUnit(bc1, glc::TEXTURE_2D); }});
    probes.push_back({"a cube map on a 2D sampler falls back to white", 70, 90, 8, 8, 0, 0, 1, 1, 4, 4, 255, 255, 255, 2,
                      [=] { bindUnit(cubeMap, glc::TEXTURE_CUBE_MAP); }});
    probes.push_back({"an empty unit falls back to white", 90, 90, 8, 8, 0, 0, 1, 1, 4, 4, 255, 255, 255, 2, [=] { bindUnit(0, glc::TEXTURE_2D); }});

    // cube array probes: a quad each, the shader gets the direction and the layer as uniforms
    struct CubeProbe { int x, layer, face; };
    const CubeProbe cubeProbes[] = {{10, 0, 0}, {30, 1, 3}, {50, 1, 5}, {70, 0, 2}};
    const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    const U32 n = 128;      // vertex capacity (4 per quad)
    const size_t quads = probes.size() + 4;
    if (quads > n / 4)
    {
        fprintf(stderr, "too many probes\n");
        return false;
    }
    U32 offsets[14] = {};
    offsets[TYPE_VERTEX] = 0;
    offsets[TYPE_TEXCOORD0] = n * 16;
    offsets[TYPE_COLOR] = n * 16 + n * 8;
    const U32 vertexBytes = offsets[TYPE_COLOR] + n * 4;
    BufferHandle vbo, ibo;
    U8* vertexCpu = nullptr;
    U8* indexCpu = nullptr;
    vb.allocate(BufferKind::Vertex, vertexBytes, vbo, vertexCpu);
    vb.allocate(BufferKind::Index, (U32) (n / 4 * 12), ibo, indexCpu);
    const float W = (float) ctx.extent.width, H = (float) ctx.extent.height;
    auto quad = [&](int index, int x, int y, int w, int h, float u0, float v0, float u1, float v1) {
        const float x0 = x * 2.0f / W - 1.0f, x1 = (x + w) * 2.0f / W - 1.0f;
        const float ytop = 1.0f - y * 2.0f / H, ybottom = 1.0f - (y + h) * 2.0f / H;
        const float corners[4][2] = {{x0, ybottom}, {x1, ybottom}, {x1, ytop}, {x0, ytop}};
        const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
        for (int i = 0; i < 4; ++i)
        {
            const float pos[4] = {corners[i][0], corners[i][1], 0.0f, 1.0f};
            const U8 white[4] = {255, 255, 255, 255};
            memcpy(vertexCpu + offsets[TYPE_VERTEX] + (index * 4 + i) * 16, pos, 16);
            memcpy(vertexCpu + offsets[TYPE_TEXCOORD0] + (index * 4 + i) * 8, uv[i], 8);
            memcpy(vertexCpu + offsets[TYPE_COLOR] + (index * 4 + i) * 4, white, 4);
        }
        const U16 idx[6] = {(U16) (index * 4), (U16) (index * 4 + 1), (U16) (index * 4 + 2), (U16) (index * 4), (U16) (index * 4 + 2),
                            (U16) (index * 4 + 3)};
        memcpy(indexCpu + index * 12, idx, 12);
    };
    for (size_t i = 0; i < probes.size(); ++i)
    {
        const Probe& p = probes[i];
        quad((int) i, p.x, p.y, p.w, p.h, p.u0, p.v0, p.u1, p.v1);
    }
    for (int i = 0; i < 4; ++i)
    {
        quad((int) probes.size() + i, cubeProbes[i].x, 120, 12, 12, 0, 0, 1, 1);
    }
    vb.bind(BufferKind::Vertex, vbo);
    vb.upload(BufferKind::Vertex, vbo, 0, vertexBytes - 1, vertexCpu);
    vb.bind(BufferKind::Index, ibo);
    vb.upload(BufferKind::Index, ibo, 0, (U32) (n / 4 * 12) - 1, indexCpu);
    vb.unbind();

    const float clear[4] = {0.1f, 0.1f, 0.2f, 1.0f};
    const uint32_t mismatchesBefore = textures.kindMismatches;
    passed &= render_frames(ctx, test, testFrames, clear,
        [&] {
            set_common_state(ctx, st, false, false);
            const U32 mask = (1u << TYPE_VERTEX) | (1u << TYPE_TEXCOORD0) | (1u << TYPE_COLOR);
            vb.setBuffers(vbo, ibo, mask, offsets);
            be.use(ui.id);
            for (size_t i = 0; i < probes.size(); ++i)
            {
                probes[i].setup();
                vb.drawIndexed(0, (U32) (i * 4), (U32) (i * 4 + 3), 6, IndexType::U16, (U32) (i * 12));
            }
            be.use(cubes.id);
            vb.setBuffers(vbo, ibo, 1u << TYPE_VERTEX, offsets);     // the cube shader only reads position
            for (int i = 0; i < 4; ++i)
            {
                tb.activateUnit(cubeUnit);
                tb.bind(glc::TEXTURE_CUBE_MAP_ARRAY, cubeArray);
                const float* d = dirs[cubeProbes[i].face];
                be.uniform3f(cubes.id, cubes.location["dir"], d[0], d[1], d[2]);
                be.uniform1f(cubes.id, cubes.location["layer"], (float) cubeProbes[i].layer);
                const size_t q = probes.size() + i;
                vb.drawIndexed(0, (U32) (q * 4), (U32) (q * 4 + 3), 6, IndexType::U16, (U32) (q * 12));
            }
        },
        [&] {
            bool ok = true;
            ok &= check("corner is the clear color", pixel_at(ctx, 5, 5), 26, 26, 51, 3);
            for (const Probe& p : probes)
            {
                ok &= check(p.name.c_str(), pixel_at(ctx, p.x + p.sx, p.y + p.sy), p.r, p.g, p.b, p.tol);
            }
            for (int i = 0; i < 4; ++i)
            {
                U8 c[3];
                faceColor(cubeProbes[i].layer, cubeProbes[i].face, c);
                char name[96];
                snprintf(name, sizeof(name), "cube map array: layer %d, face %d", cubeProbes[i].layer, cubeProbes[i].face);
                ok &= check(name, pixel_at(ctx, cubeProbes[i].x + 6, 126), c[0], c[1], c[2], 2);
            }
            return ok;
        });
    expect("a draw read a texture of the wrong kind and used the fallback", textures.kindMismatches - mismatchesBefore >= 1);
    printf("  images created %u, samplers %u, views %u\n", textures.imagesCreated, textures.samplersCreated, textures.viewsCreated);

    vkDeviceWaitIdle(ctx.device);
    vb.free(BufferKind::Vertex, vertexBytes, vbo, vertexCpu);
    vb.free(BufferKind::Index, (U32) (n / 4 * 12), ibo, indexCpu);
    be.use(0);
    const U32 names[] = {mips, autoMips, pair, rgb, lum, bc1, cubeMap, cubeArray};
    tb.destroy((S32) (sizeof(names) / sizeof(names[0])), names);
    be.deleteProgramAndObjects(ui.id);
    be.deleteProgramAndObjects(cubes.id);
    return passed;
}

// Every program of the dump through the backend interface on the real device (no drawing): create, link, reflect, serialize,
// load the blob into a second program, and compare the reflection of both.
static bool link_all(VkContext& ctx, VulkanShaderBackend& shaders, const std::string& dumpDir)
{
    LLShaderBackend& be = shaders;
    const std::vector<shaderdump::ManifestProgram> manifest = shaderdump::readManifest(dumpDir);
    int linked = 0, failed = 0, reflect_ok = 0, roundtrip_ok = 0, total_uniforms = 0, max_samplers = 0;
    double link_ms = 0.0, load_ms = 0.0;
    size_t blob_bytes = 0;

    auto reflection = [&](U32 id) {
        std::vector<std::string> out;
        for (S32 i = 0, n = be.getActiveUniformCount(id); i < n; ++i)
        {
            std::string name;
            U32 type;
            S32 size;
            be.getActiveUniform(id, i, name, type, size);
            std::string loc = std::to_string(be.getUniformLocation(id, name.c_str()));
            out.push_back(name + ":" + std::to_string(type) + ":" + std::to_string(size) + "@" + loc);
        }
        for (const std::string& a : LLShaderCompiler::viewerOptions().attribNames)
        {
            out.push_back("attrib " + a + "=" + std::to_string(be.getAttribLocation(id, a.c_str())));
        }
        return out;
    };

    for (const shaderdump::ManifestProgram& entry : manifest)
    {
        MappedProgram prog;
        CachedBinary blob;
        if (!create_program(be, dumpDir, entry, nullptr, &blob, prog))
        {
            fprintf(stderr, "  FAIL %s\n", entry.name.c_str());
            ++failed;
            continue;
        }
        ++linked;
        link_ms += shaders.lastLinkMs;
        blob_bytes += blob.data.size();
        const std::vector<std::string> first = reflection(prog.id);
        total_uniforms += prog.activeUniforms;
        int samplers = 0;
        for (auto& kv : prog.channel) { (void) kv; ++samplers; }
        max_samplers = std::max(max_samplers, samplers);
        bool ok = !first.empty();     // every active uniform must also resolve to a location (checked below)
        for (auto& kv : prog.location) ok &= kv.second >= 0;
        reflect_ok += ok;

        MappedProgram again;
        if (create_program(be, dumpDir, entry, &blob, nullptr, again) && reflection(again.id) == first)
        {
            ++roundtrip_ok;
        }
        else
        {
            fprintf(stderr, "  blob round trip differs: %s\n", entry.name.c_str());
        }
        load_ms += shaders.lastLoadMs;
        be.deleteProgramAndObjects(again.id);
        be.deleteProgramAndObjects(prog.id);
    }
    (void) ctx;
    printf("programs in the dump              : %zu (no stages skipped)\n", manifest.size());
    printf("linked through LLShaderBackend    : %d, failed %d\n", linked, failed);
    printf("reflection consistent             : %d / %d\n", reflect_ok, linked);
    printf("blob round trip identical         : %d / %d\n", roundtrip_ok, linked);
    printf("link %.0f ms in total (%.1f ms per program), blob load %.1f ms in total (%.2f ms per program), blobs %.0f KB\n", link_ms,
           linked ? link_ms / linked : 0.0, load_ms, linked ? load_ms / linked : 0.0, blob_bytes / 1024.0);
    printf("active uniforms %d, at most %d samplers in one program\n", total_uniforms, max_samplers);
    return failed == 0 && reflect_ok == linked && roundtrip_ok == linked && linked > 0;
}

bool test_framebuffers(VkContext&, VulkanRenderStateBackend&, VulkanVertexBufferBackend&,
                       VulkanShaderBackend&, VulkanTextureBackend&, VulkanFramebufferBackend&);

bool test_window_lifecycle(VkContext&);
bool test_shaders(VkContext&, VulkanRenderStateBackend&, VulkanVertexBufferBackend&,
                  VulkanShaderBackend&, VulkanFramebufferBackend&);

static int run(int argc, char** argv)
{
    bool test = false;
    bool validation = true;
    bool clipControl = true;
    bool linkAll = false, headless = false;
    int testFrames = 3;
    std::string dumpDir, programName = "UI Shader", scene;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--test")) test = true;
        else if (!strcmp(argv[i], "--headless")) { headless = true; test = true; }
        else if (!strcmp(argv[i], "--no-validation")) validation = false;
        else if (!strcmp(argv[i], "--no-depth-clip-control")) clipControl = false;
        else if (!strcmp(argv[i], "--link-all")) linkAll = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) testFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
        else if (!strcmp(argv[i], "--program") && i + 1 < argc) programName = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
    }
    if (!scene.empty() && scene != "triangle" && scene != "ui" && scene != "textures" && scene != "shader" && scene != "framebuffers" && scene != "window")
    {
        fprintf(stderr, "unknown scene: %s\n", scene.c_str());
        return 1;
    }
    if (headless && scene == "window")
    {
        fprintf(stderr, "the window lifecycle test requires a presentation surface; omit --headless\n");
        return 1;
    }
    if (scene == "framebuffers" || scene == "window" || scene == "shader") test = true;
    if (linkAll)
    {
        test = true;
    }
    if (testFrames < 1)
    {
        fprintf(stderr, "--frames must be positive\n");
        return 1;
    }
    if (scene.empty())
    {
        scene = test ? "both" : (dumpDir.empty() ? "triangle" : "ui");
    }
    if ((scene == "ui" || scene == "textures" || scene == "both") && dumpDir.empty())
    {
        if (scene == "ui" || scene == "textures")
        {
            fprintf(stderr, "the ui scene needs --dump <shader dump directory> (see shader_probe/README.md)\n");
            return 1;
        }
        printf("note: no --dump given, the ui scene is skipped\n");
        scene = "triangle";
    }

    // ---- window
    const int W = 800, H = 600;
    HINSTANCE hinst = GetModuleHandle(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = "vulkan_skeleton";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT rect{0, 0, W, H};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = headless ? nullptr : CreateWindowA("vulkan_skeleton", "Vulkan skeleton: scenes through the backend interfaces",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd && !headless)
    {
        fprintf(stderr, "FATAL: cannot create window\n");
        return 1;
    }

    // ---- Vulkan
    VkContext ctx;
    if (!ctx.init(hinst, hwnd, validation, clipControl))
    {
        return 1;
    }
    printf("%s: %ux%u, %zu images, readback %s, validation %s\n", headless ? "headless GPU target" : "swapchain", ctx.extent.width, ctx.extent.height,
           ctx.images.size(), ctx.canReadBack ? "yes" : "no", ctx.validation ? "on" : "off");

    bool passed = true;
    {
        VulkanRenderStateBackend stateImpl;
        VulkanVertexBufferBackend vertexImpl(&ctx, &stateImpl);
        VulkanShaderBackend shaderImpl(&ctx);
        VulkanTextureBackend textureImpl(&ctx);
        VulkanFramebufferBackend framebufferImpl(&ctx, &stateImpl);
        LLRenderStateBackend& st = stateImpl;       // from here on, only the viewer's interfaces are used
        LLVertexBufferBackend& vb = vertexImpl;

        if (!gQuit && !linkAll && (scene == "shader" || scene == "both"))
            passed &= test_shaders(ctx, stateImpl, vertexImpl, shaderImpl, framebufferImpl);
        if (!gQuit && !linkAll && (scene == "window" || scene == "both"))
            passed &= test_window_lifecycle(ctx);
        if (!gQuit && !linkAll && (scene == "framebuffers" || scene == "both"))
            passed &= test_framebuffers(ctx, stateImpl, vertexImpl, shaderImpl, textureImpl, framebufferImpl);
        if (!gQuit && !linkAll && (scene == "textures" || scene == "both"))
        {
            passed &= scene_textures(ctx, st, vb, shaderImpl, textureImpl, test, testFrames, dumpDir);
        }
        if (linkAll)
        {
            if (dumpDir.empty())
            {
                fprintf(stderr, "--link-all needs --dump\n");
                passed = false;
            }
            else
            {
                passed &= link_all(ctx, shaderImpl, dumpDir);
            }
            scene = "none";
        }
        if (scene == "triangle" || scene == "both")
        {
            passed &= scene_triangle(ctx, st, vb, test, testFrames);
        }
        if (!gQuit && (scene == "ui" || scene == "both"))
        {
            CachedBinary cache;
            passed &= scene_ui(ctx, st, vb, shaderImpl, textureImpl, test, testFrames, dumpDir, programName, nullptr, &cache);
            if (test && !gQuit)
            {
                // the same program again, from the binary the first run produced: no GLSL, no glslang
                passed &= scene_ui(ctx, st, vb, shaderImpl, textureImpl, test, testFrames, dumpDir, programName, &cache, nullptr);
            }
        }

        vkDeviceWaitIdle(ctx.device);
        printf("draw calls: %u, pipelines created: %u, state calls: %u, vram bytes: %llu\n", vertexImpl.drawCalls,
               vertexImpl.pipelinesCreated(), stateImpl.calls, (unsigned long long) vb.getVramBytesUsed());
        printf("uploads without a bound buffer (a GL-only rule): %u\n", vertexImpl.uploadsWithoutBind);
        for (U32 cap : stateImpl.ignoredCaps)
        {
            printf("ignored capability 0x%04x\n", cap);
        }
    } // the backends are destroyed here, before the context is shut down
    ctx.shutdown();
    if (hwnd) DestroyWindow(hwnd);

    printf("validation: %u errors, %u warnings\n", ctx.validationErrors, ctx.validationWarnings);
    if (test)
    {
        const bool ok = passed && ctx.validationErrors == 0;
        printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    try { return run(argc, argv); }
    catch (const std::exception& error)
    {
        fprintf(stderr, "Vulkan initialization/rendering failed: %s\n", error.what());
        return 1;
    }
}
