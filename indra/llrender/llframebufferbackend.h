/**
 * @file llframebufferbackend.h
 * @brief Graphics API backend interface for framebuffers
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

#ifndef LL_LLFRAMEBUFFERBACKEND_H
#define LL_LLFRAMEBUFFERBACKEND_H

#include "stdtypes.h"

// GL enums are the viewer's vocabulary. Framebuffer 0 belongs to the window backend.
// Calls run on the graphics-context thread; binding draw and read together preserves GL_FRAMEBUFFER semantics.
class LLFramebufferBackend
{
public:
    virtual ~LLFramebufferBackend() = default;
    virtual U32 createFramebuffer() = 0;
    virtual void destroyFramebuffer(U32 framebuffer) = 0;
    virtual void bind(U32 framebuffer) = 0;
    virtual void bindRead(U32 framebuffer) = 0;
    virtual U32 getBound() const = 0;
    // The framebuffer must be bound. Keeping the bind/attach/check/restore order preserves GL diagnostics.
    virtual void attach(U32 framebuffer, U32 attachment, U32 textureTarget, U32 texture, S32 level) = 0;
    virtual U32 checkStatus() = 0;
    // Select the first count color attachments (the viewer uses at most four).
    virtual void setDrawBuffers(U32 count) = 0;
    virtual void setDrawBufferNone() = 0;
    virtual void setDrawBufferBack() = 0;
    virtual void setReadBuffer(U32 buffer) = 0;
    virtual void setClearColor(F32 r, F32 g, F32 b, F32 a) = 0;
    virtual void setClearDepth(F64 depth) = 0;
    virtual void clear(U32 mask) = 0;
    virtual void readPixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* data) = 0;
};

LLFramebufferBackend* createGLFramebufferBackend();
LLFramebufferBackend& getFramebufferBackend();

#endif // LL_LLFRAMEBUFFERBACKEND_H
