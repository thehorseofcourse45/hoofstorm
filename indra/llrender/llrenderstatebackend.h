/**
 * @file llrenderstatebackend.h
 * @brief Graphics API backend interface for fixed pipeline state (blend, depth, cull, ...)
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

#ifndef LL_LLRENDERSTATEBACKEND_H
#define LL_LLRENDERSTATEBACKEND_H

#include "stdtypes.h"

// Applies fixed pipeline state to the graphics API. See doc/vulkan/pipeline-state-abstraction.md.
//
// The viewer keeps its own CPU-side copy of this state (LLGLState::sStateMap, the LLGLDepthTest statics, ...)
// and only calls the backend when a value really changes, after flushing the immediate-mode vertex batch
// (gGL.flush()). The GL backend applies each call immediately. A Vulkan backend would record the value and
// resolve a pipeline at the next draw.
//
// Capability and function values use the GL constants (GL_BLEND, GL_LEQUAL, ...) as the viewer's state
// vocabulary for now; other backends translate them.
//
// Threading: call from the thread that owns the graphics context.
class LLRenderStateBackend
{
public:
    virtual ~LLRenderStateBackend() = default;

    // enable or disable one capability (GL_BLEND, GL_CULL_FACE, GL_STENCIL_TEST, GL_SCISSOR_TEST,
    // GL_POLYGON_OFFSET_*, GL_MULTISAMPLE, ...)
    virtual void setCapability(U32 cap, bool enabled) = 0;
    // what the API reports for a capability; used only by debug state validation
    virtual bool isCapabilityEnabled(U32 cap) const = 0;

    virtual void setDepthTest(bool enabled) = 0;
    virtual void setDepthFunc(U32 func) = 0;
    virtual void setDepthMask(bool writeEnabled) = 0;
    // what the API reports for the depth state; used only by debug state validation
    virtual void queryDepthState(bool& testEnabled, U32& func, bool& writeEnabled) const = 0;
};

// OpenGL implementation, in llrenderstatebackend_gl.cpp
LLRenderStateBackend* createGLRenderStateBackend();

// The active backend. Created on first use as the GL backend.
LLRenderStateBackend& getRenderStateBackend();

#endif // LL_LLRENDERSTATEBACKEND_H
