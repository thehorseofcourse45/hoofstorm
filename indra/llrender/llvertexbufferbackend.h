/**
 * @file llvertexbufferbackend.h
 * @brief Graphics API backend interface for LLVertexBuffer
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

#ifndef LL_LLVERTEXBUFFERBACKEND_H
#define LL_LLVERTEXBUFFERBACKEND_H

#include "llvertexbuffer.h"

// Everything LLVertexBuffer needs from the graphics API. LLVertexBuffer keeps the CPU-side shadow copy of
// the data and the dirty-region bookkeeping; the backend owns GPU storage, pooling, binding state,
// uploads, vertex input setup and draw calls.
//
// Threading: all methods must be called from the thread that owns the graphics context.
class LLVertexBufferBackend
{
public:
    virtual ~LLVertexBufferBackend() = default;

    // True if writes only go to the CPU shadow copy and the GPU buffer is (re)created at unmap time
    // (the Apple GPU path in the GL backend). False if dirty ranges are streamed with upload().
    virtual bool usesDeferredUpload() const = 0;

    // --- storage ---------------------------------------------------------------------------------------
    // Allocate GPU storage of 'size' bytes plus a 16-byte aligned CPU shadow copy returned in 'data'.
    // 'handle' and 'data' must be empty on entry.
    virtual void allocate(BufferKind kind, U32 size, BufferHandle& handle, U8*& data) = 0;
    // Release what allocate() returned. 'data' may be null if the CPU copy was already released.
    virtual void free(BufferKind kind, U32 size, BufferHandle handle, U8* data) = 0;
    virtual U64 getVramBytesUsed() = 0;
    // Deferred-upload path: replace 'handle' (releasing the old buffer if set) with a new GPU buffer
    // initialised from 'data', and make it the current buffer of its kind.
    virtual void createStatic(BufferKind kind, BufferHandle& handle, U32 size, const void* data) = 0;

    // --- binding ---------------------------------------------------------------------------------------
    // Make 'handle' the current buffer of its kind. No-op if it already is.
    virtual void bind(BufferKind kind, BufferHandle handle) = 0;
    virtual bool isBound(BufferKind kind, BufferHandle handle) const = 0;
    virtual void unbind() = 0;
    // Forget the cached vertex buffer so the next setBuffers() rebinds it and re-sets the vertex input
    // (see https://github.com/secondlife/viewer/issues/912)
    virtual void invalidateBufferBinding() = 0;

    // --- upload ----------------------------------------------------------------------------------------
    // Copy bytes [start, end] (inclusive) of 'data' into the current buffer 'handle' at the same offsets.
    // 'handle' must be the bound buffer of its kind.
    virtual void upload(BufferKind kind, BufferHandle handle, U32 start, U32 end, const void* data) = 0;

    // --- vertex input / draw ---------------------------------------------------------------------------
    // Make 'vb' and 'ib' current for the next draw. 'attribMask' is the set of LLVertexBuffer::MAP_* attributes
    // the bound shader reads; 'offsets' are the byte offsets of each attribute inside the vertex buffer.
    virtual void setBuffers(BufferHandle vb, BufferHandle ib, U32 attribMask, const U32* offsets) = 0;
    // Enable exactly the vertex attribute arrays in 'mask' (MAP_* bits).
    virtual void setEnabledAttributes(U32 mask) = 0;
    // 'mode' is an LLRender::eGeomModes value. 'indexByteOffset' is the offset into the index buffer.
    virtual void drawIndexed(U32 mode, U32 start, U32 end, U32 count, IndexType type, U32 indexByteOffset) = 0;
    virtual void drawArrays(U32 mode, U32 first, U32 count) = 0;

    // Create the per-context vertex input state the API needs before anything can be drawn (a dummy vertex
    // array object in core-profile GL). Call once per graphics context, after the context is current.
    virtual void initContextState() = 0;
    // Draw 'count' vertices starting at 'first' with no vertex buffers: the vertex shader generates the
    // positions (for example a fullscreen triangle from gl_VertexID). 'mode' is an LLRender::eGeomModes value.
    virtual void drawArraysNoInput(U32 mode, U32 first, U32 count) = 0;

    // Debug label for graphics debuggers (RenderDoc); may be a no-op.
    virtual void setLabel(BufferHandle handle, const char* label) = 0;
};

// OpenGL implementation, in llvertexbuffer_gl.cpp
LLVertexBufferBackend* createGLVertexBufferBackend();

#endif // LL_LLVERTEXBUFFERBACKEND_H
