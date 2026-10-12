/**
 * @file llvertexbuffertypes.h
 * @brief Small API-neutral types shared by LLVertexBuffer and its backends
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

#ifndef LL_LLVERTEXBUFFERTYPES_H
#define LL_LLVERTEXBUFFERTYPES_H

#include "stdtypes.h"

// Deliberately dependency-free (no GL headers, no viewer math types) so that a graphics backend can include
// llvertexbufferbackend.h without pulling in the rest of the renderer.

// Backend-neutral buffer description (no GL enums in the public interface)
enum class BufferKind : U8 { Vertex, Index };
enum class IndexType : U8 { U16, U32 };

// Opaque GPU buffer handle. 0 means "no buffer". The backend decides what the value means
// (a GL buffer name today).
struct BufferHandle
{
    U64 id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const BufferHandle& rhs) const { return id == rhs.id; }
    bool operator!=(const BufferHandle& rhs) const { return id != rhs.id; }
};

#endif // LL_LLVERTEXBUFFERTYPES_H
