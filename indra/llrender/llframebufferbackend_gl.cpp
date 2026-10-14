/**
 * @file llframebufferbackend_gl.cpp
 * @brief OpenGL implementation of LLFramebufferBackend
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
#include "llframebufferbackend.h"
#include "llglheaders.h"

namespace
{
class LLGLFramebufferBackend final : public LLFramebufferBackend
{
public:
    U32 createFramebuffer() override
    {
        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        return framebuffer;
    }
    void destroyFramebuffer(U32 framebuffer) override
    {
        glDeleteFramebuffers(1, &framebuffer);
        if (mDraw == framebuffer) mDraw = 0;
    }
    void bind(U32 framebuffer) override
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        mDraw = framebuffer;
    }
    void bindRead(U32 framebuffer) override { glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer); }
    U32 getBound() const override { return mDraw; }
    void attach(U32 framebuffer, U32 attachment, U32 target, U32 texture, S32 level) override
    {
        llassert(mDraw == framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, target, texture, level);
    }
    U32 checkStatus() override { return glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER); }
    void setDrawBuffers(U32 count) override
    {
        const GLenum buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        llassert(count <= 4);
        if (count <= 4) glDrawBuffers(count, buffers);
    }
    void setDrawBufferNone() override { glDrawBuffer(GL_NONE); }
    void setDrawBufferBack() override { glDrawBuffer(GL_BACK); }
    void setReadBuffer(U32 buffer) override { glReadBuffer(buffer); }
    void setClearColor(F32 r, F32 g, F32 b, F32 a) override { glClearColor(r, g, b, a); }
    void setClearDepth(F64 depth) override { glClearDepth(depth); }
    void clear(U32 mask) override { glClear(mask); }
    void readPixels(S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* data) override
    {
        glReadPixels(x, y, width, height, format, type, data);
    }
private:
    U32 mDraw = 0;
};
} // namespace

LLFramebufferBackend* createGLFramebufferBackend() { return new LLGLFramebufferBackend(); }
