/**
 * @file llrenderbackendselection.h
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

#ifndef LL_LLRENDERBACKENDSELECTION_H
#define LL_LLRENDERBACKENDSELECTION_H

struct LLRenderBackendSelection
{
    const char* requested;
    const char* selected;
    const char* fallbackReason;
};

LLRenderBackendSelection chooseRenderBackend(const char* request, bool vulkanRuntimeBuilt);
// Immutable for the process, including across GL context recreation. No API objects are created here.
const LLRenderBackendSelection& getRenderBackendSelection();
// File the viewer writes when the Vulkan renderer dies (abort or uncaught exception). While it exists an unset
// FS_RENDER_BACKEND selects OpenGL; FS_RENDER_BACKEND=vulkan deletes it and tries Vulkan again.
const char* getRenderBackendFailureMarkerPath();

#endif
