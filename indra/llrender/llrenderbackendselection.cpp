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
#include <cstdlib>
#include <cstring>

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

const LLRenderBackendSelection& getRenderBackendSelection()
{
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
        const auto result = chooseRenderBackend(request,
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
            true
#else
            false
#endif
        );
#ifdef _WIN32
        std::free(request);
#endif
        return result;
    }();
    return selection;
}
