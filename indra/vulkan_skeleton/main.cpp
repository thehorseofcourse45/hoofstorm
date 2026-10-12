// Vulkan skeleton: draws one triangle through the viewer's backend interfaces.
//
//   LLVertexBufferBackend (buffers, vertex input, draws)   <- llvertexbufferbackend.h
//   LLRenderStateBackend  (fixed pipeline state)           <- llrenderstatebackend.h
//
// Everything the "render code" below does goes through those two interfaces, in the same call pattern the viewer's
// LLVertexBuffer and LLRender use. The Vulkan implementations are in vkbackends.cpp.
//
//   vulkan_skeleton.exe              open a window and draw until closed
//   vulkan_skeleton.exe --test       render a few frames, read the image back, check pixels, exit 0 (pass) or 1 (fail)
//   --frames N                       frames to render in --test mode (default 3)
//   --no-validation                  do not enable the Khronos validation layer
#include "vkbackends.h"

#include <GL/gl.h>
#include <cmath>
#include <cstring>
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

struct Pixel { int r, g, b; };

static Pixel pixel_at(const VkContext& ctx, int x, int y)
{
    // swapchain format is B8G8R8A8_UNORM
    const uint8_t* p = ctx.readMapped + ((size_t) y * ctx.extent.width + x) * 4;
    return {p[2], p[1], p[0]};
}

static bool near_pixel(Pixel p, int r, int g, int b, int tol)
{
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol;
}

static int run(VkContext& ctx, bool test, int testFrames, bool& passed)
{
    const std::string dir = exe_dir();
    ctx.vertexShader = ctx.loadShader((dir + "tri.vert.spv").c_str());
    ctx.fragmentShader = ctx.loadShader((dir + "tri.frag.spv").c_str());

    VulkanRenderStateBackend stateImpl;
    VulkanVertexBufferBackend vertexImpl(&ctx, &stateImpl);
    LLRenderStateBackend& st = stateImpl;       // from here on, only the viewer's interfaces are used
    LLVertexBufferBackend& vb = vertexImpl;

    // ---- the data, laid out the way LLVertexBuffer::calcOffsets() lays it out: one array per attribute
    constexpr U32 TYPE_VERTEX = 0, TYPE_COLOR = 6;
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

    // ---- frames
    int frame = 0;
    const float clear[4] = {0.1f, 0.1f, 0.2f, 1.0f};
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
            break;
        }

        // What LLGLState, LLRender and LLVertexBuffer::setBuffer() would do before a draw:
        st.setViewport(0, 0, (S32) ctx.extent.width, (S32) ctx.extent.height);
        st.setCapability(GL_SCISSOR_TEST, false);
        st.setCapability(GL_CULL_FACE, true);
        st.setCullFace(GL_BACK);
        st.setCapability(GL_BLEND, true);
        st.setBlendFunc(7, 9, 7, 9); // LLRender::BT_ALPHA
        st.setColorMask(true, true, true, true);
        st.setPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        st.setCapability(GL_DEPTH_TEST, false);

        vb.setBuffers(vbo, ibo, (1u << TYPE_VERTEX) | (1u << TYPE_COLOR), offsets);
        vb.drawIndexed(0 /* LLRender::TRIANGLES */, 0, 2, 3, IndexType::U16, 0);

        const bool readBack = test && frame == testFrames - 1;
        ctx.endFrame(readBack);

        if (readBack)
        {
            VK_CHECK(vkWaitForFences(ctx.device, 1, &ctx.frameFence, VK_TRUE, UINT64_MAX));
            const int w = (int) ctx.extent.width, h = (int) ctx.extent.height;
            auto at = [&](float ndcX, float ndcY) {
                return pixel_at(ctx, (int) ((ndcX + 1.0f) * 0.5f * w), (int) ((1.0f - ndcY) * 0.5f * h));
            };

            struct Check { const char* what; Pixel got; int r, g, b, tol; bool dominant; };
            // expected values are computed from the barycentric weights at each sample point
            Check checks[] = {
                {"corner is the clear color",                 pixel_at(ctx, 5, 5),   26, 26, 51, 3,  false},
                {"empty area right of the triangle is clear", at(0.45f, 0.40f),      26, 26, 51, 3,  false},
                {"near the top vertex it is blue",            at(0.0f, 0.40f),       13, 13, 230, 25, false},
                {"near the bottom-left vertex it is red",     at(-0.40f, -0.45f),    223, 19, 13, 25, false},
                {"near the bottom-right vertex it is green",  at(0.40f, -0.45f),     19, 223, 13, 25, false},
            };
            for (auto& c : checks)
            {
                const bool ok = near_pixel(c.got, c.r, c.g, c.b, c.tol);
                printf("  [%s] %-45s got (%3d,%3d,%3d) expected (%3d,%3d,%3d)\n", ok ? "PASS" : "FAIL", c.what, c.got.r, c.got.g,
                       c.got.b, c.r, c.g, c.b);
                passed &= ok;
            }
            break;
        }

        ++frame;
        if (test && frame >= testFrames)
        {
            break;
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

    vb.free(BufferKind::Vertex, 64, vbo, vertexCpu);
    vb.free(BufferKind::Index, 8, ibo, indexCpu);
    vkDestroyShaderModule(ctx.device, ctx.vertexShader, nullptr);
    vkDestroyShaderModule(ctx.device, ctx.fragmentShader, nullptr);
    ctx.vertexShader = ctx.fragmentShader = VK_NULL_HANDLE;
    return 0; // the backends are destroyed here, before the context is shut down
}

int main(int argc, char** argv)
{
    bool test = false;
    bool validation = true;
    int testFrames = 3;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--test")) test = true;
        else if (!strcmp(argv[i], "--no-validation")) validation = false;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) testFrames = atoi(argv[++i]);
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
    HWND hwnd = CreateWindowA("vulkan_skeleton", "Vulkan skeleton: triangle through the backend interfaces",
                              WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd)
    {
        fprintf(stderr, "FATAL: cannot create window\n");
        return 1;
    }

    // ---- Vulkan
    VkContext ctx;
    if (!ctx.init(hinst, hwnd, validation))
    {
        return 1;
    }
    printf("swapchain: %ux%u, %zu images, readback %s, validation %s\n", ctx.extent.width, ctx.extent.height,
           ctx.images.size(), ctx.canReadBack ? "yes" : "no", ctx.validation ? "on" : "off");

    bool passed = true;
    run(ctx, test, testFrames, passed);
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
