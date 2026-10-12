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
//   --scene triangle|ui                    which scene to draw in interactive mode
//   --no-depth-clip-control                do not use VK_EXT_depth_clip_control: shows what happens to GL's clip depth range
//   --frames N                             frames to render per scene in --test mode (default 3)
//   --no-validation                        do not enable the Khronos validation layer
#include "vkbackends.h"
#include "vkprogram.h"
#include "shaderdump.h"

#include <GL/gl.h>
#include <cmath>
#include <cstring>
#include <functional>
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
            fprintf(stderr, "swapchain out of date (resizing is not supported by the skeleton)\n");
            return false;
        }
        draw();

        const bool readBack = test && frame == testFrames - 1;
        ctx.endFrame(readBack);
        if (readBack)
        {
            VK_CHECK(vkWaitForFences(ctx.device, 1, &ctx.frameFence, VK_TRUE, UINT64_MAX));
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
static bool scene_ui(VkContext& ctx, LLRenderStateBackend& st, LLVertexBufferBackend& vb, bool test, int testFrames,
                     const std::string& dumpDir, const std::string& programName)
{
    printf("scene: ui (viewer program \"%s\" from %s)\n", programName.c_str(), dumpDir.c_str());

    // 1. the viewer's GLSL -> Vulkan SPIR-V
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
    const LLShaderCompiler::Result compiled =
        LLShaderCompiler::compileProgram(shaderdump::loadStages(dumpDir, *entry), LLShaderCompiler::viewerOptions(), true);
    if (!compiled.ok() || compiled.spirv.size() != 2)
    {
        fprintf(stderr, "  shader compile failed: %s\n", compiled.error.c_str());
        return false;
    }
    printf("  translated: %zu uniform block members (%d bytes), %zu samplers, %d attribute locations\n",
           compiled.layout.members.size(), compiled.layout.size, compiled.translation.samplers.size(),
           compiled.translation.attributesLocated);

    VkProgram* program = VkProgram::create(ctx, compiled.spirv[0], compiled.spirv[1], compiled.layout, compiled.translation.samplers);
    ctx.program = program;

    // 2. a 2x2 texture; texel (u,v) with v = 0 the first row, like GL: red green / blue yellow
    const uint8_t texels[16] = {255, 0, 0, 255,   0, 255, 0, 255,    0, 0, 255, 255,   255, 255, 0, 255};
    VkTexture texture = createTexture(ctx, 2, 2, texels, false);
    if (!program->setTexture("diffuseMap", &texture))
    {
        fprintf(stderr, "  the program has no sampler called diffuseMap\n");
        return false;
    }

    // 3. uniforms, written by name into the program's block shadow (what LLGLSLShader::uniformMatrix4fv & co. would do)
    const float identity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    const float scale2[16] = {2, 0, 0, 0,  0, 2, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};   // texture coordinates 0..0.5 become 0..1
    const float zero = 0.0f;
    bool uniforms_ok = program->setUniform("modelview_projection_matrix", identity, sizeof(identity));
    uniforms_ok &= program->setUniform("texture_matrix0", scale2, sizeof(scale2));
    uniforms_ok &= program->setUniform("mirror_flag", &zero, sizeof(zero));
    if (!uniforms_ok)
    {
        fprintf(stderr, "  the program block has no modelview_projection_matrix / texture_matrix0 / mirror_flag\n");
        return false;
    }

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
    bool passed = render_frames(ctx, test, testFrames, clear,
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
    ctx.program = nullptr;
    destroyTexture(ctx, texture);
    program->destroy();
    return passed;
}

int main(int argc, char** argv)
{
    bool test = false;
    bool validation = true;
    bool clipControl = true;
    int testFrames = 3;
    std::string dumpDir, programName = "UI Shader", scene;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--test")) test = true;
        else if (!strcmp(argv[i], "--no-validation")) validation = false;
        else if (!strcmp(argv[i], "--no-depth-clip-control")) clipControl = false;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) testFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
        else if (!strcmp(argv[i], "--program") && i + 1 < argc) programName = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
    }
    if (scene.empty())
    {
        scene = test ? "both" : (dumpDir.empty() ? "triangle" : "ui");
    }
    if ((scene == "ui" || scene == "both") && dumpDir.empty())
    {
        if (scene == "ui")
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
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    HWND hwnd = CreateWindowA("vulkan_skeleton", "Vulkan skeleton: scenes through the backend interfaces",
                              WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd)
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
    printf("swapchain: %ux%u, %zu images, readback %s, validation %s\n", ctx.extent.width, ctx.extent.height,
           ctx.images.size(), ctx.canReadBack ? "yes" : "no", ctx.validation ? "on" : "off");

    bool passed = true;
    {
        VulkanRenderStateBackend stateImpl;
        VulkanVertexBufferBackend vertexImpl(&ctx, &stateImpl);
        LLRenderStateBackend& st = stateImpl;       // from here on, only the viewer's interfaces are used
        LLVertexBufferBackend& vb = vertexImpl;

        if (scene == "triangle" || scene == "both")
        {
            passed &= scene_triangle(ctx, st, vb, test, testFrames);
        }
        if (!gQuit && (scene == "ui" || scene == "both"))
        {
            passed &= scene_ui(ctx, st, vb, test, testFrames, dumpDir, programName);
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
    DestroyWindow(hwnd);

    printf("validation: %u errors, %u warnings\n", ctx.validationErrors, ctx.validationWarnings);
    if (test)
    {
        const bool ok = passed && ctx.validationErrors == 0;
        printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    return 0;
}
