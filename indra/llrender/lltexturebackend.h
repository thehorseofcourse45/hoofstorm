/**
 * @file lltexturebackend.h
 * @brief Graphics API backend interface for textures
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

#ifndef LL_LLTEXTUREBACKEND_H
#define LL_LLTEXTUREBACKEND_H

#include "stdtypes.h"

// Everything the viewer's texture classes need from the graphics API. See doc/vulkan/texture-layer-design.md.
//
// Texture names are plain U32 handles (0 = none), as everywhere in the viewer. Targets, wrap modes and filters use the GL
// constants (GL_TEXTURE_2D, GL_REPEAT, GL_LINEAR_MIPMAP_LINEAR, ...) as the viewer's vocabulary for now; other backends
// translate them. The GL "bound texture of the active unit" model is kept on purpose: a call that edits a texture acts on
// the texture currently bound to the active unit, and LLTexUnit tracks (and caches) which that is.
//
// This header grows with the refactor: LLTexUnit's calls first (unit selection, binding, sampler state of the bound
// texture), then the upload and readback calls of LLImageGL and the other users.
//
// Threading: call from the thread that owns the graphics context.
class LLTextureBackend
{
public:
    virtual ~LLTextureBackend() = default;

    // one-time setup of the context's texture state (pixel transfer alignment)
    virtual void initContextState() = 0;

    // --- units ---------------------------------------------------------------------------------------------------------
    // Make texture unit 'unit' (0 .. LL_NUM_TEXTURE_LAYERS-1) the active one.
    virtual void activateUnit(U32 unit) = 0;
    // Bind 'texture' (0 = none) to the active unit for 'target'.
    virtual void bind(U32 target, U32 texture) = 0;

    // --- sampler state of the texture bound to the active unit -----------------------------------------------------------
    // wrapR < 0: leave the R coordinate alone (only cube maps, cube map arrays and 3D textures have one).
    virtual void setWrap(U32 target, S32 wrapS, S32 wrapT, S32 wrapR) = 0;
    virtual void setFilter(U32 target, S32 magFilter, S32 minFilter) = 0;
    virtual void setMaxAnisotropy(U32 target, F32 maxAnisotropy) = 0;
};

// OpenGL implementation, in lltexturebackend_gl.cpp
LLTextureBackend* createGLTextureBackend();

// The active backend. Created on first use as the GL backend.
LLTextureBackend& getTextureBackend();

#endif // LL_LLTEXTUREBACKEND_H
