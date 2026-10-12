/**
 * @file llvertexbuffer_gl.cpp
 * @brief OpenGL backend for LLVertexBuffer
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

// All the OpenGL calls that LLVertexBuffer makes live here. The pool, the bound-buffer cache and the
// vertex attribute setup were moved verbatim out of llvertexbuffer.cpp.

#include "linden_common.h"

#include "llvertexbufferbackend.h"
#include "llfasttimer.h"
#include "llsys.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llimagegl.h"
#include "llmemory.h"

#include <chrono>
#include <list>
#include <unordered_map>
#include <vector>

// defined in llvertexbuffer.cpp
U32 nhpo2(U32 v);

// currently bound buffers and enabled attribute arrays (GL state cache)
static U32 sGLRenderBuffer = 0;
static U32 sGLRenderIndices = 0;
static U32 sLastMask = 0;

//============================================================================
// Pool of reusable VertexBuffer state

// batch calls to glGenBuffers
static GLuint gen_buffer()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

    GLuint ret = 0;
    constexpr U32 pool_size = 4096;

    thread_local static GLuint sNamePool[pool_size];
    thread_local static U32 sIndex = 0;

    if (sIndex == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("gen buffer");
        sIndex = pool_size;
#if !LL_DARWIN
        if (!gGLManager.mIsAMD)
        {
            glGenBuffers(pool_size, sNamePool);
        }
        else
#endif
        { // work around for AMD driver bug
            for (U32 i = 0; i < pool_size; ++i)
            {
                glGenBuffers(1, sNamePool + i);
            }
        }
    }

    ret = sNamePool[--sIndex];
    return ret;
}

static void delete_buffers(S32 count, GLuint* buffers)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // wait a few frames before actually deleting the buffers to avoid
    // synchronization issues with the GPU
    static std::vector<GLuint> sFreeList[4];

    if (gGLManager.mInited)
    {
        U32 idx = LLImageGL::sFrameCount % 4;

        for (S32 i = 0; i < count; ++i)
        {
            sFreeList[idx].push_back(buffers[i]);
        }

        idx = (LLImageGL::sFrameCount + 3) % 4;

        if (!sFreeList[idx].empty())
        {
            glDeleteBuffers((GLsizei)sFreeList[idx].size(), sFreeList[idx].data());
            sFreeList[idx].resize(0);
        }
    }
}


#define ANALYZE_VBO_POOL 0

static GLenum gl_buffer_target(BufferKind kind)
{
    return kind == BufferKind::Index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
}

static GLenum gl_index_type(IndexType type)
{
    return type == IndexType::U32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
}

// VBO Pool interface
class LLVBOPool
{
    public:
    virtual ~LLVBOPool() = default;
    virtual void allocate(BufferKind type, U32 size, GLuint& name, U8*& data) = 0;
    virtual void free(BufferKind type, U32 size, GLuint name, U8* data) = 0;
    virtual U64 getVramBytesUsed() = 0;
};

// VBO Pool for Apple GPUs (as in M1/M2 etc, not Intel macs)
// Effectively disables VBO pooling
class LLAppleVBOPool final: public LLVBOPool
{
public:
    U64 mAllocated = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated;
    }

    void allocate(BufferKind type, U32 size, GLuint& name, U8*& data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        STOP_GLERROR;
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(data == nullptr);  // non null data indicates a buffer that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mAllocated += size;

        { //allocate a new buffer
            LL_PROFILE_GPU_ZONE("vbo alloc");
            // ON OS X, we don't allocate a VBO until the last possible moment
            // in unmapBuffer
            data = (U8*) ll_aligned_malloc_16(size);
            STOP_GLERROR;
        }
    }

    void free(BufferKind type, U32 size, GLuint name, U8* data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(size >= 2);

        if (data)
        {
            ll_aligned_free_16(data);
        }

        mAllocated -= size;
        STOP_GLERROR;
        if (name)
        {
            delete_buffers(1, &name);
        }
        STOP_GLERROR;
    }
};

// VBO Pool for GPUs that benefit from VBO pooling
class LLDefaultVBOPool final : public LLVBOPool
{
public:
    typedef std::chrono::steady_clock::time_point Time;
    struct Entry
    {
        U8* mData;
        GLuint mGLName;
        Time mAge;
    };

    ~LLDefaultVBOPool() override
    {
        clear();
    }

    typedef std::unordered_map<U32, std::list<Entry>> Pool;

    Pool mVBOPool;
    Pool mIBOPool;

    U32 mTouchCount = 0;

    U64 mDistributed = 0;
    U64 mAllocated = 0;
    U64 mReserved = 0;
    U32 mMisses = 0;
    U32 mHits = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated + mReserved;
    }

    // increase the size to some common value (e.g. a power of two) to increase hit rate
    void adjustSize(U32& size)
    {
        // size = nhpo2(size);  // (193/303)/580 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 66 percent. Hit rate: 77 percent

        //(245/276)/385 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 57 percent. Hit rate: 69 percent
        //(187/209)/397 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 112 percent. Hit rate: 76 percent
        U32 block_size = llmax(nhpo2(size) / 8, (U32) 16);
        size += block_size - (size % block_size);
    }

    void allocate(BufferKind type, U32 size, GLuint& name, U8*& data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(data == nullptr);  // non null data indicates a buffer that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mDistributed += size;
        adjustSize(size);
        mAllocated += size;

        auto& pool = type == BufferKind::Index ? mIBOPool : mVBOPool;

        Pool::iterator iter = pool.find(size);
        if (iter == pool.end())
        { // cache miss, allocate a new buffer
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo pool miss");
            LL_PROFILE_GPU_ZONE("vbo alloc");

            mMisses++;
            name = gen_buffer();
            glBindBuffer(gl_buffer_target(type), name);
            glBufferData(gl_buffer_target(type), size, nullptr, GL_DYNAMIC_DRAW);
            if (type == BufferKind::Index)
            {
                sGLRenderIndices = name;
            }
            else
            {
                sGLRenderBuffer = name;
            }

            data = (U8*)ll_aligned_malloc_16(size);
        }
        else
        {
            mHits++;
            llassert(mReserved >= size); // assert if accounting gets messed up
            mReserved -= size;

            std::list<Entry>& entries = iter->second;
            Entry& entry = entries.back();
            name = entry.mGLName;
            data = entry.mData;

            entries.pop_back();
            if (entries.empty())
            {
                pool.erase(iter);
            }
        }

        clean();
    }

    void free(BufferKind type, U32 size, GLuint name, U8* data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(size >= 2);
        llassert(name != 0);
        llassert(data != nullptr);

        clean();

        llassert(mDistributed >= size);
        mDistributed -= size;
        adjustSize(size);
        llassert(mAllocated >= size);
        mAllocated -= size;
        mReserved += size;

        auto& pool = type == BufferKind::Index ? mIBOPool : mVBOPool;

        Pool::iterator iter = pool.find(size);

        if (iter == pool.end())
        {
            std::list<Entry> newlist;
            newlist.push_front({ data, name, std::chrono::steady_clock::now() });
            pool[size] = newlist;
        }
        else
        {
            iter->second.push_front({ data, name, std::chrono::steady_clock::now() });
        }

    }

    // clean periodically (clean gets called for every alloc/free)
    void clean()
    {
        mTouchCount++;
        if (mTouchCount < 1024) // clean every 1k touches
        {
            return;
        }
        mTouchCount = 0;

        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

        std::unordered_map<U32, std::list<Entry>>* pools[] = { &mVBOPool, &mIBOPool };

        using namespace std::chrono_literals;

        Time cutoff = std::chrono::steady_clock::now() - 5s;

        for (auto* pool : pools)
        {
            for (Pool::iterator iter = pool->begin(); iter != pool->end(); )
            {
                auto& entries = iter->second;

                while (!entries.empty() && entries.back().mAge < cutoff)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo cache timeout");
                    auto& entry = entries.back();
                    ll_aligned_free_16(entry.mData);
                    delete_buffers(1, &entry.mGLName);
                    llassert(mReserved >= iter->first);
                    mReserved -= iter->first;
                    entries.pop_back();

                }

                if (entries.empty())
                {
                    iter = pool->erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }

#if 0
        LL_INFOS() << llformat("(%d/%d)/%d MB (distributed/allocated)/total in VBO Pool. Overhead: %d percent. Hit rate: %d percent",
            mDistributed / 1000000,
            mAllocated / 1000000,
            (mAllocated + mReserved) / 1000000, // total bytes
            ((mAllocated+mReserved-mDistributed)*100)/llmax(mDistributed, (U64) 1), // overhead percent
            (mHits*100)/llmax(mMisses+mHits, (U32)1)) // hit rate percent
            << LL_ENDL;
#endif
    }

    void clear()
    {
        for (auto& entries : mIBOPool)
        {
            for (auto& entry : entries.second)
            {
                ll_aligned_free_16(entry.mData);
                delete_buffers(1, &entry.mGLName);
            }
        }

        for (auto& entries : mVBOPool)
        {
            for (auto& entry : entries.second)
            {
                ll_aligned_free_16(entry.mData);
                delete_buffers(1, &entry.mGLName);
            }
        }

        mReserved = 0;

        mIBOPool.clear();
        mVBOPool.clear();
    }
};

static LLVBOPool* sVBOPool = nullptr;

static const U32 sGLMode[LLRender::NUM_MODES] =
{
    GL_TRIANGLES,
    GL_TRIANGLE_STRIP,
    GL_TRIANGLE_FAN,
    GL_POINTS,
    GL_LINES,
    GL_LINE_STRIP,
    GL_LINE_LOOP,
};

// Point each attribute the bound shader reads at its offset inside the bound vertex buffer.
static void setup_vertex_attributes(U32 data_mask, const U32* offsets)
{
    STOP_GLERROR;
    U8* base = nullptr;

    if (data_mask & LLVertexBuffer::MAP_NORMAL)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_NORMAL;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_NORMAL]);
        glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_NORMAL], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_TEXCOORD3)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_TEXCOORD3;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_TEXCOORD3]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_TEXCOORD3], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_TEXCOORD2)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_TEXCOORD2;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_TEXCOORD2]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_TEXCOORD2], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_TEXCOORD1)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_TEXCOORD1;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_TEXCOORD1]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_TEXCOORD1], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_TANGENT)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_TANGENT;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_TANGENT]);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_TANGENT], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_TEXCOORD0)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_TEXCOORD0;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_TEXCOORD0]);
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_TEXCOORD0], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_COLOR)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_COLOR;
        //bind emissive instead of color pointer if emissive is present
        void* ptr = (data_mask & LLVertexBuffer::MAP_EMISSIVE) ? (void*)(base + offsets[LLVertexBuffer::TYPE_EMISSIVE]) : (void*)(base + offsets[LLVertexBuffer::TYPE_COLOR]);
        glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_COLOR], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_EMISSIVE)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_EMISSIVE;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_EMISSIVE]);
        glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_EMISSIVE], ptr);

        if (!(data_mask & LLVertexBuffer::MAP_COLOR))
        { //map emissive to color channel when color is not also being bound to avoid unnecessary shader swaps
            loc = LLVertexBuffer::TYPE_COLOR;
            glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_EMISSIVE], ptr);
        }
    }
    if (data_mask & LLVertexBuffer::MAP_WEIGHT)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_WEIGHT;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_WEIGHT]);
        glVertexAttribPointer(loc, 1, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_WEIGHT], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_WEIGHT4)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_WEIGHT4;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_WEIGHT4]);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_WEIGHT4], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_JOINT)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_JOINT;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_JOINT]);
        glVertexAttribIPointer(loc, 4, GL_UNSIGNED_SHORT, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_JOINT], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_CLOTHWEIGHT)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_CLOTHWEIGHT;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_CLOTHWEIGHT]);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_TRUE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_CLOTHWEIGHT], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_TEXTURE_INDEX)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_TEXTURE_INDEX;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_VERTEX] + 12);
        glVertexAttribIPointer(loc, 1, GL_UNSIGNED_INT, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_VERTEX], ptr);
    }
    if (data_mask & LLVertexBuffer::MAP_VERTEX)
    {
        LLVertexBuffer::AttributeType loc = LLVertexBuffer::TYPE_VERTEX;
        void* ptr = (void*)(base + offsets[LLVertexBuffer::TYPE_VERTEX]);
        glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_VERTEX], ptr);
    }
    STOP_GLERROR;
}

static void set_enabled_attribute_arrays(U32 data_mask)
{
    if (sLastMask != data_mask)
    {
        for (U32 i = 0; i < LLVertexBuffer::TYPE_MAX; ++i)
        {
            S32 loc = i;

            U32 mask = 1 << i;

            if (sLastMask & (1 << i))
            { //was enabled
                if (!(data_mask & mask))
                { //needs to be disabled
                    glDisableVertexAttribArray(loc);
                }
            }
            else
            {   //was disabled
                if (data_mask & mask)
                { //needs to be enabled
                    glEnableVertexAttribArray(loc);
                }
            }
        }
    }

    sLastMask = data_mask;
}

//============================================================================
// OpenGL implementation of the backend interface

class LLGLVertexBufferBackend final : public LLVertexBufferBackend
{
public:
    LLGLVertexBufferBackend()
    {
        if (gGLManager.mIsApple)
        {
            LL_INFOS() << "VBO Pooling Disabled" << LL_ENDL;
            mPool = new LLAppleVBOPool();
        }
        else
        {
            LL_INFOS() << "VBO Pooling Enabled" << LL_ENDL;
            mPool = new LLDefaultVBOPool();
        }
    }

    ~LLGLVertexBufferBackend() override
    {
        unbind();
        delete mPool;
    }

    bool usesDeferredUpload() const override { return gGLManager.mIsApple; }

    void allocate(BufferKind kind, U32 size, BufferHandle& handle, U8*& data) override
    {
        GLuint name = (GLuint) handle.id;
        mPool->allocate(kind, size, name, data);
        handle.id = name;
    }

    void free(BufferKind kind, U32 size, BufferHandle handle, U8* data) override
    {
        mPool->free(kind, size, (GLuint) handle.id, data);
    }

    U64 getVramBytesUsed() override { return mPool->getVramBytesUsed(); }

    void createStatic(BufferKind kind, BufferHandle& handle, U32 size, const void* data) override
    {
        if (handle)
        {
            GLuint old = (GLuint) handle.id;
            delete_buffers(1, &old);
        }
        handle.id = gen_buffer();
        glBindBuffer(gl_buffer_target(kind), (GLuint) handle.id);
        cached(kind) = (U32) handle.id;
        glBufferData(gl_buffer_target(kind), size, data, GL_STATIC_DRAW);
    }

    void bind(BufferKind kind, BufferHandle handle) override
    {
        U32& current = cached(kind);
        if (current != handle.id)
        {
            glBindBuffer(gl_buffer_target(kind), (GLuint) handle.id);
            current = (U32) handle.id;
        }
    }

    bool isBound(BufferKind kind, BufferHandle handle) const override
    {
        return (kind == BufferKind::Vertex ? sGLRenderBuffer : sGLRenderIndices) == handle.id;
    }

    void unbind() override
    {
        STOP_GLERROR;
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        STOP_GLERROR;
        sGLRenderBuffer = 0;
        sGLRenderIndices = 0;
    }

    void invalidateBufferBinding() override { sGLRenderBuffer = 0; }

    void upload(BufferKind kind, BufferHandle handle, U32 start, U32 end, const void* data) override
    {
        llassert(isBound(kind, handle));

        // stream to GPU via glBufferSubData
        if (end != 0)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("glBufferSubData");
            LL_PROFILE_ZONE_NUM(start);
            LL_PROFILE_ZONE_NUM(end);
            LL_PROFILE_ZONE_NUM(end-start);

            constexpr U32 block_size = 65536;

            for (U32 i = start; i <= end; i += block_size)
            {
                U32 tend = llmin(i + block_size, end);
                U32 size = tend - i + 1;
                glBufferSubData(gl_buffer_target(kind), i, size, (U8*) data + (i-start));
            }
        }
    }

    void setBuffers(BufferHandle vb, BufferHandle ib, U32 attribMask, const U32* offsets) override
    {
        STOP_GLERROR;

        if (sGLRenderBuffer != vb.id)
        {
            glBindBuffer(GL_ARRAY_BUFFER, (GLuint) vb.id);
            sGLRenderBuffer = (U32) vb.id;

            setup_vertex_attributes(attribMask, offsets);
        }
        else if (sLastMask != attribMask)
        {
            setup_vertex_attributes(attribMask, offsets);
            sLastMask = attribMask;
        }

        if (sGLRenderIndices != ib.id)
        {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint) ib.id);
            sGLRenderIndices = (U32) ib.id;
        }

        STOP_GLERROR;
    }

    void setEnabledAttributes(U32 mask) override { set_enabled_attribute_arrays(mask); }

    void drawIndexed(U32 mode, U32 start, U32 end, U32 count, IndexType type, U32 indexByteOffset) override
    {
        STOP_GLERROR;
        glDrawRangeElements(sGLMode[mode], start, end, count, gl_index_type(type), (GLvoid*) (size_t) indexByteOffset);
        STOP_GLERROR;
    }

    void drawArrays(U32 mode, U32 first, U32 count) override
    {
        STOP_GLERROR;
        glDrawArrays(sGLMode[mode], first, count);
        STOP_GLERROR;
    }

    void initContextState() override
    {
        // bind a dummy vertex array object so we're core profile compliant
        U32 ret;
        glGenVertexArrays(1, &ret);
        glBindVertexArray(ret);
    }

    void drawArraysNoInput(U32 mode, U32 first, U32 count) override
    {
        glDrawArrays(sGLMode[mode], first, count);
    }

    void setLabel(BufferHandle handle, const char* label) override
    {
        LL_LABEL_OBJECT_GL(GL_BUFFER, (GLuint) handle.id, strlen(label), label);
    }

private:
    static U32& cached(BufferKind kind) { return kind == BufferKind::Vertex ? sGLRenderBuffer : sGLRenderIndices; }

    LLVBOPool* mPool = nullptr;
};

LLVertexBufferBackend* createGLVertexBufferBackend()
{
    return new LLGLVertexBufferBackend();
}
