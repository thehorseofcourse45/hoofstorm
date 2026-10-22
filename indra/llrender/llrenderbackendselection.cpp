/**
 * @file llrenderbackendselection.cpp
 * @brief Viewer startup backend selection
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llrenderbackendselection.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

LLRenderBackendSelection chooseRenderBackend(const char* request, bool vulkanRuntimeBuilt)
{
    if (!request || !*request)
    {
        // Unset: Vulkan when the runtime is built; set FS_RENDER_BACKEND=opengl to opt out.
        return {"default", vulkanRuntimeBuilt ? "vulkan" : "opengl", nullptr};
    }
    if (std::strcmp(request, "opengl") == 0)
    {
        return {"opengl", "opengl", nullptr};
    }
    if (std::strcmp(request, "vulkan") != 0)
    {
        return {"invalid", "opengl", "FS_RENDER_BACKEND must be opengl or vulkan"};
    }
    return vulkanRuntimeBuilt ? LLRenderBackendSelection{"vulkan", "vulkan", nullptr}
        : LLRenderBackendSelection{"vulkan", "opengl", "Vulkan runtime was not built"};
}

const char* getRenderBackendFailureMarkerPath()
{
    static const std::string path = []()
    {
        std::string appdata = ".";
#ifdef _WIN32
        char* value = nullptr;
        if (_dupenv_s(&value, nullptr, "APPDATA") == 0 && value) appdata = value;
        std::free(value);
#else
        if (const char* value = std::getenv("APPDATA")) appdata = value;
#endif
        return appdata + "/Firestorm_x64/logs/vulkan_failed.flag";
    }();
    return path.c_str();
}

const LLRenderBackendSelection& getRenderBackendSelection()
{
    static const std::string markerReason = std::string("The previous Vulkan session failed; using OpenGL. Delete ")
        + getRenderBackendFailureMarkerPath() + " or set FS_RENDER_BACKEND=vulkan to try Vulkan again";
    static const LLRenderBackendSelection selection = []()
    {
#ifdef _WIN32
        char* request = nullptr;
        if (_dupenv_s(&request, nullptr, "FS_RENDER_BACKEND") != 0)
        {
            return LLRenderBackendSelection{"unreadable", "opengl", "Cannot read FS_RENDER_BACKEND"};
        }
#else
        const char* request = std::getenv("FS_RENDER_BACKEND");
#endif
        auto result = chooseRenderBackend(request,
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
            true
#else
            false
#endif
        );
        const bool unset = !request || !*request;
        const bool wantsVulkan = request && std::strcmp(request, "vulkan") == 0;
#ifdef _WIN32
        std::free(request);
#endif
        if (std::strcmp(result.selected, "vulkan") == 0)
        {
            if (std::ifstream(getRenderBackendFailureMarkerPath()).good())
            {
                if (wantsVulkan) std::remove(getRenderBackendFailureMarkerPath());     // explicit retry
                else if (unset) result = {"default", "opengl", markerReason.c_str()};
            }
        }
        return result;
    }();
    return selection;
}
