#include "llrenderbackendselection.h"
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>
#include <process.h>
#include <string>
#include <fstream>

static int checks = 0;
static void check(bool passed)
{
    ++checks;
    if (!passed) { std::fprintf(stderr, "FAILED check %d\n", checks); std::exit(1); }
}

// mode "marker" / "retry": a failure marker from a previous Vulkan session (each mode needs its own process, the selection is cached)
static int markerMode(bool retry)
{
    // own directory per process so parallel ctest runs do not share the marker
    const std::string appdata = "marker_test_appdata_" + std::to_string(_getpid());
    _putenv_s("APPDATA", appdata.c_str());
    _mkdir(appdata.c_str()); _mkdir((appdata + "/Firestorm_x64").c_str()); _mkdir((appdata + "/Firestorm_x64/logs").c_str());
    { std::ofstream(getRenderBackendFailureMarkerPath()) << "test"; }
    check(std::ifstream(getRenderBackendFailureMarkerPath()).good());
    _putenv_s("FS_RENDER_BACKEND", retry ? "vulkan" : "");
    const auto& sel = getRenderBackendSelection();
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (retry) { check(std::strcmp(sel.selected, "vulkan") == 0); check(!std::ifstream(getRenderBackendFailureMarkerPath()).good()); }
    else
    {
        check(std::strcmp(sel.selected, "opengl") == 0);
        check(sel.fallbackReason && std::strstr(sel.fallbackReason, "previous Vulkan session failed") != nullptr);
        check(std::ifstream(getRenderBackendFailureMarkerPath()).good());
    }
#else
    check(std::strcmp(sel.selected, "opengl") == 0);
#endif
    std::remove(getRenderBackendFailureMarkerPath());
    _rmdir((appdata + "/Firestorm_x64/logs").c_str()); _rmdir((appdata + "/Firestorm_x64").c_str()); _rmdir(appdata.c_str());
    std::printf("PASS: %d marker checks\n", checks);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1) return markerMode(std::strcmp(argv[1], "retry") == 0);
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
