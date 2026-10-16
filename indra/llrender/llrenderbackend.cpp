/**
 * @file llrenderbackend.cpp
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

#include "linden_common.h"
#include "llrenderbackendselection.h"
#include "llrenderbackend.h"
#include "llrenderstatebackend.h"
#include "llshaderbackend.h"
#include "lltexturebackend.h"
#include "llframebufferbackend.h"
#include "llvertexbufferbackend.h"
#include <map>
#include <mutex>

static bool sObjectsCreated = false;
bool renderBackendObjectsCreated() { return sObjectsCreated; }

// Freeze the startup request before allocation; an installed native session supplies the complete active set.
LLRenderStateBackend& getRenderStateBackend()
{
    getRenderBackendSelection();
    sObjectsCreated = true;
    if (auto* backend = activeVulkanRenderStateBackend()) return *backend;
    static LLRenderStateBackend* backend = createGLRenderStateBackend();
    return *backend;
}

static std::mutex sShaderNamesMutex;
static std::map<U32, std::string> sShaderNames;
void setShaderDebugName(U32 program, const std::string& name)
{
    std::lock_guard<std::mutex> lock(sShaderNamesMutex);
    sShaderNames[program] = name;
}
std::string getShaderDebugName(U32 program)
{
    std::lock_guard<std::mutex> lock(sShaderNamesMutex);
    auto it = sShaderNames.find(program);
    return it == sShaderNames.end() ? std::string() : it->second;
}

LLShaderBackend& getShaderBackend()
{
    getRenderBackendSelection();
    sObjectsCreated = true;
    if (auto* backend = activeVulkanShaderBackend()) return *backend;
    static LLShaderBackend* backend = createGLShaderBackend();
    return *backend;
}

LLTextureBackend& getTextureBackend()
{
    getRenderBackendSelection();
    sObjectsCreated = true;
    if (auto* backend = activeVulkanTextureBackend()) return *backend;
    static LLTextureBackend* backend = createGLTextureBackend();
    return *backend;
}

LLFramebufferBackend& getFramebufferBackend()
{
    getRenderBackendSelection();
    sObjectsCreated = true;
    if (auto* backend = activeVulkanFramebufferBackend()) return *backend;
    static LLFramebufferBackend* backend = createGLFramebufferBackend();
    return *backend;
}

LLVertexBufferBackend* createViewerVertexBufferBackend()
{
    getRenderBackendSelection();
    sObjectsCreated = true;
    if (auto* backend = createActiveVulkanVertexBufferBackend()) return backend;
    return createGLVertexBufferBackend();
}
