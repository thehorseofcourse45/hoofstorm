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

    void setMagFilter(U32 target, S32 magFilter) override
    {
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
    }

    void setCompare(U32 target, bool enabled, U32 func) override
    {
        if (enabled)
        {
            glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
            glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, func);
        }
        else
        {
            glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
    }

    void setLevelRange(U32 target, S32 baseLevel, S32 maxLevel) override
    {
        glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, baseLevel);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, maxLevel);
    }

    void setSwizzle(U32 target, const S32* rgba) override
    {
        glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, rgba);
    }

    void setLegacyMipmapGeneration(U32 target, bool enabled) override
    {
        glTexParameteri(target, GL_GENERATE_MIPMAP, enabled ? GL_TRUE : GL_FALSE);
    }

    void generate(S32 count, U32* names) override
    {
        glGenTextures(count, names);
    }

    void destroy(S32 count, const U32* names) override
    {
        glDeleteTextures(count, names);
    }

    void setUnpackRowLength(S32 pixels) override
    {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pixels);
    }

    void setUnpackSwapBytes(bool swap) override
    {
        glPixelStorei(GL_UNPACK_SWAP_BYTES, swap ? 1 : 0);
    }

    void image2D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, U32 format, U32 type, const void* pixels) override
    {
        glTexImage2D(target, level, internalFormat, width, height, 0, format, type, pixels);
    }

    void compressedImage2D(U32 target, S32 level, U32 internalFormat, S32 width, S32 height, S32 imageSize, const void* data) override
    {
        glCompressedTexImage2D(target, level, internalFormat, width, height, 0, imageSize, data);
    }

    void subImage2D(U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels) override
    {
        glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
    }

    void image3D(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, S32 depth, U32 format, U32 type,
                 const void* pixels) override
    {
        glTexImage3D(target, level, internalFormat, width, height, depth, 0, format, type, pixels);
    }

    void subImage3D(U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format, U32 type,
                    const void* pixels) override
    {
        glTexSubImage3D(target, level, x, y, z, width, height, depth, format, type, pixels);
    }

    void generateMipmaps(U32 target) override
    {
        glGenerateMipmap(target);
    }

    void copyFromFramebuffer2D(U32 target, S32 level, S32 xoffset, S32 yoffset, S32 x, S32 y, S32 width, S32 height) override
    {
        glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    }

    void copyFromFramebuffer3D(U32 target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width,
                               S32 height) override
    {
        glCopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y, width, height);
    }

    void copyImageFromFramebuffer2D(U32 target, S32 level, S32 internalFormat, S32 x, S32 y, S32 width, S32 height) override
    {
        glCopyTexImage2D(target, level, internalFormat, x, y, width, height, 0);
    }

    void replaceWithMipLevel(U32 target, S32 mip, S32 internalFormat, S32 width, S32 height, U32 format, U32 type, U64 bytes) override
    {
        if (mScratchPBO == 0)
        {
            glGenBuffers(1, &mScratchPBO);
            mScratchPBOSize = 0;
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, mScratchPBO);

        if (bytes > mScratchPBOSize)
        {
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_COPY);
            mScratchPBOSize = (U32)bytes;
        }

        glGetTexImage(target, mip, format, type, nullptr);

        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, mScratchPBO);
        glTexImage2D(target, 0, internalFormat, width, height, 0, format, type, nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    S32 getLevelParameter(U32 target, S32 level, U32 pname) override
    {
        GLint value = 0;
        glGetTexLevelParameteriv(target, level, pname, &value);
        return value;
    }

    void readImage(U32 target, S32 level, U32 format, U32 type, void* out) override
    {
        glGetTexImage(target, level, format, type, out);
    }

    void readCompressedImage(U32 target, S32 level, void* out) override
    {
        glGetCompressedTexImage(target, level, out);
    }

    bool isResident(U32 texture) override
    {
        GLboolean resident = GL_FALSE;
        glAreTexturesResident(1, &texture, &resident);
        return resident != GL_FALSE;
    }

    void shutdown() override
    {
        if (mScratchPBO != 0)
        {
            glDeleteBuffers(1, &mScratchPBO);
            mScratchPBO = 0;
            mScratchPBOSize = 0;
        }
    }

private:
    U32 mScratchPBO = 0;        // pixel buffer used by replaceWithMipLevel()
    U32 mScratchPBOSize = 0;
};

} // namespace

LLTextureBackend* createGLTextureBackend()
{
    return new LLGLTextureBackend();
}
