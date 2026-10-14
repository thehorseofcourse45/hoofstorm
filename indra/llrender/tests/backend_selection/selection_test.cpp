#include "llrenderbackendselection.h"
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int checks = 0;
static void check(bool passed)
{
    ++checks;
    if (!passed) { std::fprintf(stderr, "FAILED check %d\n", checks); std::exit(1); }
}

int main()
{
    for (bool built : {false, true})
    {
        for (const char* request : {static_cast<const char*>(nullptr), "", "opengl", "vulkan", "Vulkan", "typo"})
        {
            const auto result = chooseRenderBackend(request, built);
            const bool native=built && (!request || !*request || std::strcmp(request,"vulkan")==0);
            check(std::strcmp(result.selected,native?"vulkan":"opengl")==0);
            const bool fallback = !native && request && *request && std::strcmp(request, "opengl") != 0;
            check((result.fallbackReason != nullptr) == fallback);
            if (request && std::strcmp(request, "vulkan") == 0)
            {
                check(std::strcmp(result.requested, "vulkan") == 0);
                check(built ? result.fallbackReason==nullptr : std::strstr(result.fallbackReason,"not built")!=nullptr);
            }
        }
    }
    _putenv_s("FS_RENDER_BACKEND", "vulkan");
    const auto& first = getRenderBackendSelection();
    check(std::strcmp(first.requested, "vulkan") == 0);
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    check(std::strcmp(first.selected,"vulkan")==0);
    check(first.fallbackReason==nullptr);
#else
    check(std::strcmp(first.selected,"opengl")==0);
    check(std::strstr(first.fallbackReason, "not built") != nullptr);
#endif
    _putenv_s("FS_RENDER_BACKEND", "opengl");
    const auto& second = getRenderBackendSelection();
    check(&first == &second);
    check(std::strcmp(second.requested, "vulkan") == 0);
    std::printf("PASS: %d startup-selection checks\n", checks);
}
