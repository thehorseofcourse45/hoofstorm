/**
 * @file llrenderstatebackend_gl.cpp
 * @brief OpenGL backend for fixed pipeline state
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

#include "llrenderstatebackend.h"
#include "llglheaders.h"

// Stateless: every call maps to the GL call the viewer made directly before this layer existed.
class LLGLRenderStateBackend final : public LLRenderStateBackend
{
public:
    void setCapability(U32 cap, bool enabled) override
    {
        if (enabled)
        {
            glEnable(cap);
        }
        else
        {
            glDisable(cap);
        }
    }

    bool isCapabilityEnabled(U32 cap) const override { return glIsEnabled(cap) == GL_TRUE; }

    void setDepthTest(bool enabled) override { setCapability(GL_DEPTH_TEST, enabled); }

    void setDepthFunc(U32 func) override { glDepthFunc(func); }

    void setDepthMask(bool writeEnabled) override { glDepthMask(writeEnabled ? GL_TRUE : GL_FALSE); }

    void queryDepthState(bool& testEnabled, U32& func, bool& writeEnabled) const override
    {
        GLint gl_func = 0;
        GLboolean mask = GL_FALSE;

        glGetIntegerv(GL_DEPTH_FUNC, &gl_func);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);

        testEnabled = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
        func = (U32) gl_func;
        writeEnabled = mask == GL_TRUE;
    }
};

LLRenderStateBackend* createGLRenderStateBackend()
{
    return new LLGLRenderStateBackend();
}

LLRenderStateBackend& getRenderStateBackend()
{
    static LLRenderStateBackend* sBackend = createGLRenderStateBackend();
    return *sBackend;
}
