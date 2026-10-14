#include "factory_test_stubs.h"
#include "llrenderbackendselection.h"
#include <cstdio>
#include <cstring>

int allocations[5] = {};
static int checks = 0;
static void check(bool passed)
{
    ++checks;
    if (!passed) { std::fprintf(stderr, "FAILED check %d\n", checks); std::exit(1); }
}

// Changing the request during allocation proves that selection precedes API object creation.
template<class T> T* allocate(int index)
{
    _putenv_s("FS_RENDER_BACKEND", "opengl");
    ++allocations[index];
    return new T;
}
LLRenderStateBackend* createGLRenderStateBackend() { return allocate<LLRenderStateBackend>(0); }
LLShaderBackend* createGLShaderBackend() { return allocate<LLShaderBackend>(1); }
LLTextureBackend* createGLTextureBackend() { return allocate<LLTextureBackend>(2); }
LLFramebufferBackend* createGLFramebufferBackend() { return allocate<LLFramebufferBackend>(3); }
LLVertexBufferBackend* createGLVertexBufferBackend() { return allocate<LLVertexBufferBackend>(4); }

int main(int argc, char** argv)
{
    check(argc == 2);
    const int first = std::atoi(argv[1]);
    check(first >= 0 && first < 5);
    _putenv_s("FS_RENDER_BACKEND", "vulkan");
    LLVertexBufferBackend* vertex = nullptr;
    switch (first)
    {
        case 0: getRenderStateBackend(); break;
        case 1: getShaderBackend(); break;
        case 2: getTextureBackend(); break;
        case 3: getFramebufferBackend(); break;
        case 4: vertex = createViewerVertexBufferBackend(); break;
    }
    const auto& selection = getRenderBackendSelection();
    check(std::strcmp(selection.requested, "vulkan") == 0);
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    check(std::strcmp(selection.selected,"vulkan")==0);
    check(selection.fallbackReason==nullptr);
#else
    check(std::strcmp(selection.selected,"opengl")==0);
    check(selection.fallbackReason!=nullptr);
#endif
    for (int index = 0; index < 5; ++index) check(allocations[index] == (index == first ? 1 : 0));
    auto* state = &getRenderStateBackend();
    auto* shader = &getShaderBackend();
    auto* texture = &getTextureBackend();
    auto* framebuffer = &getFramebufferBackend();
    check(state == &getRenderStateBackend());
    check(shader == &getShaderBackend());
    check(texture == &getTextureBackend());
    check(framebuffer == &getFramebufferBackend());
    if (!vertex) vertex = createViewerVertexBufferBackend();
    for (int index = 0; index < 5; ++index) check(allocations[index] == 1);
    delete vertex;
    vertex = createViewerVertexBufferBackend();
    check(allocations[4] == 2);
    check(std::strcmp(getRenderBackendSelection().requested, "vulkan") == 0);
    delete vertex;
    std::printf("PASS: %d factory-order/lifetime checks (first factory %d)\n", checks, first);
}
