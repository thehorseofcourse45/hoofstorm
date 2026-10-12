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
#include "llrender.h"

// indexed by LLRender::eBlendFactor
static const GLenum sGLBlendFactor[] =
{
    GL_ONE,
    GL_ZERO,
    GL_DST_COLOR,
    GL_SRC_COLOR,
    GL_ONE_MINUS_DST_COLOR,
    GL_ONE_MINUS_SRC_COLOR,
    GL_DST_ALPHA,
    GL_SRC_ALPHA,
    GL_ONE_MINUS_DST_ALPHA,
    GL_ONE_MINUS_SRC_ALPHA,

    GL_ZERO // 'BF_UNDEF'
};

static_assert(sizeof(sGLBlendFactor) / sizeof(sGLBlendFactor[0]) == LLRender::BF_UNDEF + 1,
              "sGLBlendFactor must have one entry per LLRender::eBlendFactor");

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

    void setBlendFunc(U32 colorSrc, U32 colorDst, U32 alphaSrc, U32 alphaDst) override
    {
        llassert(colorSrc < LLRender::BF_UNDEF);
        llassert(colorDst < LLRender::BF_UNDEF);
        llassert(alphaSrc < LLRender::BF_UNDEF);
        llassert(alphaDst < LLRender::BF_UNDEF);

        if (colorSrc == alphaSrc && colorDst == alphaDst)
        {
            // same blend state as the separate call, using the call the viewer used for this case
            glBlendFunc(sGLBlendFactor[colorSrc], sGLBlendFactor[colorDst]);
        }
        else
        {
            glBlendFuncSeparate(sGLBlendFactor[colorSrc], sGLBlendFactor[colorDst],
                                sGLBlendFactor[alphaSrc], sGLBlendFactor[alphaDst]);
        }
    }

    void setColorMask(bool r, bool g, bool b, bool a) override
    {
        glColorMask(r ? GL_TRUE : GL_FALSE,
                    g ? GL_TRUE : GL_FALSE,
                    b ? GL_TRUE : GL_FALSE,
                    a ? GL_TRUE : GL_FALSE);
    }

    void setLineWidth(F32 width) override { glLineWidth(width); }

    void setViewport(S32 x, S32 y, S32 width, S32 height) override { glViewport(x, y, width, height); }

    void setScissor(S32 x, S32 y, S32 width, S32 height) override { glScissor(x, y, width, height); }

    void setCullFace(U32 face) override { glCullFace(face); }

    void setPolygonMode(U32 face, U32 mode) override { glPolygonMode(face, mode); }

    void setPolygonOffset(F32 factor, F32 units) override { glPolygonOffset(factor, units); }

    void setPointSize(F32 size) override { glPointSize(size); }

    void setStencilFunc(U32 func, S32 ref, U32 mask) override { glStencilFunc(func, ref, mask); }

    void setStencilOp(U32 sfail, U32 dpfail, U32 dppass) override { glStencilOp(sfail, dpfail, dppass); }

    void setStencilMask(U32 mask) override { glStencilMask(mask); }
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
