/**
 * @file lltexturebackend_gl.cpp
 * @brief OpenGL implementation of LLTextureBackend
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

#include "lltexturebackend.h"
#include "llglheaders.h"

namespace
{

class LLGLTextureBackend final : public LLTextureBackend
{
public:
    void initContextState() override
    {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }

    void activateUnit(U32 unit) override
    {
        glActiveTexture(GL_TEXTURE0 + unit);
    }

    void bind(U32 target, U32 texture) override
    {
        glBindTexture(target, texture);
    }

    void setWrap(U32 target, S32 wrapS, S32 wrapT, S32 wrapR) override
    {
        glTexParameteri(target, GL_TEXTURE_WRAP_S, wrapS);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, wrapT);
        if (wrapR >= 0)
        {
            glTexParameteri(target, GL_TEXTURE_WRAP_R, wrapR);
        }
    }

    void setFilter(U32 target, S32 magFilter, S32 minFilter) override
    {
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
    }

    void setMaxAnisotropy(U32 target, F32 maxAnisotropy) override
    {
        glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, maxAnisotropy);
    }
};

} // namespace

LLTextureBackend* createGLTextureBackend()
{
    return new LLGLTextureBackend();
}

LLTextureBackend& getTextureBackend()
{
    static LLTextureBackend* sBackend = createGLTextureBackend();
    return *sBackend;
}
