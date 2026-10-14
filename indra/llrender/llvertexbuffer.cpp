/**
 * @file llvertexbuffer.cpp
 * @brief LLVertexBuffer implementation
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

#include "linden_common.h"

#include "llfasttimer.h"
#include "llsys.h"
#include "llvertexbuffer.h"
#include "llvertexbufferbackend.h"
// #include "llrender.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llvector4a.h"
#include "llshadermgr.h"
#include "llglslshader.h"
#include "llmemory.h"
#include <glm/gtc/type_ptr.hpp>

//Next Highest Power Of Two
//helper function, returns first number > v that is a power of 2, or v if v is already a power of 2
U32 nhpo2(U32 v)
{
    U32 r = 1;
    while (r < v) {
        r *= 2;
    }
    return r;
}

//which power of 2 is i?
//assumes i is a power of 2 > 0
U32 wpo2(U32 i)
{
    llassert(i > 0);
    llassert(nhpo2(i) == i);

    U32 r = 0;

    while (i >>= 1) ++r;

    return r;
}

struct CompareMappedRegion
{
    bool operator()(const LLVertexBuffer::MappedRegion& lhs, const LLVertexBuffer::MappedRegion& rhs)
    {
        return lhs.mStart < rhs.mStart;
    }
};


static LLVertexBufferBackend* sBackend = nullptr;

void LLVertexBufferData::drawWithMatrix()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    if (mTexName)
    {
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexName);
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mModelView));
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mProjection));
    gGL.matrixMode(LLRender::MM_TEXTURE0);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mTexture0));

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);

    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

void LLVertexBufferData::draw()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    if (mTexName)
    {
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexName);
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);
}

//============================================================================

//static
U64 LLVertexBuffer::getBytesAllocated()
{
    return sBackend ? sBackend->getVramBytesUsed() : 0;
}

//============================================================================
//
//static
U32 LLVertexBuffer::sVertexCount = 0;


//NOTE: each component must be AT LEAST 4 bytes in size to avoid a performance penalty on AMD hardware
const U32 LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_MAX] =
{
    sizeof(LLVector4), // TYPE_VERTEX,
    sizeof(LLVector4), // TYPE_NORMAL,
    sizeof(LLVector2), // TYPE_TEXCOORD0,
    sizeof(LLVector2), // TYPE_TEXCOORD1,
    sizeof(LLVector2), // TYPE_TEXCOORD2,
    sizeof(LLVector2), // TYPE_TEXCOORD3,
    sizeof(LLColor4U), // TYPE_COLOR,
    sizeof(LLColor4U), // TYPE_EMISSIVE, only alpha is used currently
    sizeof(LLVector4), // TYPE_TANGENT,
    sizeof(F32),       // TYPE_WEIGHT,
    sizeof(LLVector4), // TYPE_WEIGHT4,
    sizeof(LLVector4), // TYPE_CLOTHWEIGHT,
    sizeof(U64),       // TYPE_JOINT,
    sizeof(LLVector4), // TYPE_TEXTURE_INDEX (actually exists as position.w), no extra data, but stride is 16 bytes
};

static const std::string vb_type_name[] =
{
    "TYPE_VERTEX",
    "TYPE_NORMAL",
    "TYPE_TEXCOORD0",
    "TYPE_TEXCOORD1",
    "TYPE_TEXCOORD2",
    "TYPE_TEXCOORD3",
    "TYPE_COLOR",
    "TYPE_EMISSIVE",
    "TYPE_TANGENT",
    "TYPE_WEIGHT",
    "TYPE_WEIGHT4",
    "TYPE_CLOTHWEIGHT",
    "TYPE_JOINT"
    "TYPE_TEXTURE_INDEX",
    "TYPE_MAX",
    "TYPE_INDEX",
};


//static

//static
void LLVertexBuffer::drawArrays(U32 mode, const std::vector<LLVector3>& pos)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    gGL.begin(mode);
    for (auto& v : pos)
    {
        gGL.vertex3fv(v.mV);
    }
    gGL.end();
    gGL.flush();
}

//static
void LLVertexBuffer::drawElements(U32 mode, const LLVector4a* pos, const LLVector2* tc, U32 num_indices, const U16* indicesp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(LLGLSLShader::sCurBoundShaderPtr != NULL);

    // <FS:Beq> FIRE-29679 trap empty calls that cause crashes when rezzing in OpenSim.
    if(pos == nullptr || indicesp == nullptr )
    {
        LL_WARNS() << "Called drawElements with null pos or null indices" << LL_ENDL;
        return;
    }
    // </FS:Beq>

    // <FS:Ansariel> Crash fix due to invalid calls to drawElements by Drake Arconis
    if (num_indices == 0)
    {
        LL_WARNS() << "Called drawElements with 0 indices" << LL_ENDL;
        return;
    }
    // </FS:Ansariel>

    STOP_GLERROR;

    gGL.syncMatrices();

    unbind();

    gGL.begin(mode);

    if (tc != nullptr)
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gGL.texCoord2fv(tc[idx].mV);
            gGL.vertex3fv(pos[idx].getF32ptr());
        }
    }
    else
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gGL.vertex3fv(pos[idx].getF32ptr());
        }
    }
    gGL.end();
    gGL.flush();
}

bool LLVertexBuffer::validateRange(U32 start, U32 end, U32 count, U32 indices_offset) const
{
    if (!gDebugGL)
    {
        return true;
    }

    llassert(start < mNumVerts);
    llassert(end < mNumVerts);

    if (start >= mNumVerts ||
        end >= mNumVerts)
    {
        LL_ERRS() << "Bad vertex buffer draw range: [" << start << ", " << end << "] vs " << mNumVerts << LL_ENDL;
    }

    if (indices_offset >= mNumIndices ||
        indices_offset + count > mNumIndices)
    {
        LL_ERRS() << "Bad index buffer draw range: [" << indices_offset << ", " << indices_offset+count << "]" << LL_ENDL;
    }

    {
#if 0  // not a reliable test for VBOs that are not backed by a CPU buffer
        U16* idx = (U16*) mMappedIndexData+indices_offset;
        for (U32 i = 0; i < count; ++i)
        {
            llassert(idx[i] >= start);
            llassert(idx[i] <= end);

            if (idx[i] < start || idx[i] > end)
            {
                LL_ERRS() << "Index out of range: " << idx[i] << " not in [" << start << ", " << end << "]" << LL_ENDL;
            }
        }

        LLVector4a* v = (LLVector4a*)mMappedData;

        for (U32 i = start; i <= end; ++i)
        {
            if (!v[i].isFinite3())
            {
                LL_ERRS() << "Non-finite vertex position data detected." << LL_ENDL;
            }
        }

        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;

        if (shader && shader->mFeatures.mIndexedTextureChannels > 1)
        {
            LLVector4a* v = (LLVector4a*) mMappedData;

            for (U32 i = start; i < end; i++)
            {
                U32 idx = (U32) (v[i][3]+0.25f);
                if (idx >= (U32)shader->mFeatures.mIndexedTextureChannels)
                {
                    LL_ERRS() << "Bad texture index found in vertex data stream." << LL_ENDL;
                }
            }
        }
#endif
    }

    return true;
}

#if LL_PROFILER_ENABLE_RENDER_DOC
void LLVertexBuffer::setLabel(const char* label) {
    sBackend->setLabel(mBuffer, label);
}
#endif

void LLVertexBuffer::clone(LLVertexBuffer& target) const
{
    target.mTypeMask = mTypeMask;
    target.mIndexType = mIndexType;
    target.mIndicesStride = mIndicesStride;
    if (target.getNumVerts() != getNumVerts() ||
        target.getNumIndices() != getNumIndices())
    {
        target.allocateBuffer(getNumVerts(), getNumIndices());
    }
}

void LLVertexBuffer::drawRange(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    llassert(validateRange(start, end, count, indices_offset));
    llassert(sBackend->isBound(BufferKind::Vertex, mBuffer));
    llassert(sBackend->isBound(BufferKind::Index, mIndices));
    gGL.syncMatrices();
    sBackend->drawIndexed(mode, start, end, count, mIndexType, indices_offset * mIndicesStride);
}

void LLVertexBuffer::drawRangeFast(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    sBackend->drawIndexed(mode, start, end, count, mIndexType, indices_offset * mIndicesStride);
}


void LLVertexBuffer::draw(U32 mode, U32 count, U32 indices_offset) const
{
    drawRange(mode, 0, mNumVerts-1, count, indices_offset);
}


void LLVertexBuffer::drawArrays(U32 mode, U32 first, U32 count) const
{
    llassert(first + count <= mNumVerts);
    llassert(sBackend->isBound(BufferKind::Vertex, mBuffer));
    llassert(sBackend->isBound(BufferKind::Index, mIndices));

    gGL.syncMatrices();
    sBackend->drawArrays(mode, first, count);
}

//static
void LLVertexBuffer::initClass(LLWindow* window)
{
    llassert(sBackend == nullptr);

    sBackend = createViewerVertexBufferBackend();
}

//static
void LLVertexBuffer::setupClientArrays(U32 data_mask)
{
    if (sBackend)
    {
        sBackend->setEnabledAttributes(data_mask);
    }
}

//static
void LLVertexBuffer::unbind()
{
    if (sBackend)
    {
        sBackend->unbind();
    }
}

//static
bool LLVertexBuffer::usesDeferredUpload()
{
    return sBackend && sBackend->usesDeferredUpload();
}

//static
void LLVertexBuffer::initContextVertexState()
{
    // fail loudly: without this state a core-profile context cannot draw anything
    llassert_always(sBackend);
    sBackend->initContextState();
}

//static
void LLVertexBuffer::drawArraysNoInput(U32 mode, U32 first, U32 count)
{
    llassert(sBackend);
    sBackend->drawArraysNoInput(mode, first, count);
}

//static
void LLVertexBuffer::invalidateBufferBinding()
{
    if (sBackend)
    {
        sBackend->invalidateBufferBinding();
    }
}

//static
void LLVertexBuffer::cleanupClass()
{
    unbind();

    delete sBackend;
    sBackend = nullptr;
}

//----------------------------------------------------------------------------

LLVertexBuffer::LLVertexBuffer(U32 typemask)
:   LLRefCount(),
    mTypeMask(typemask)
{
    //zero out offsets
    for (U32 i = 0; i < TYPE_MAX; i++)
    {
        mOffsets[i] = 0;
    }
}

// list of mapped buffers
// NOTE: must not be LLPointer<LLVertexBuffer> to avoid breaking non-ref-counted LLVertexBuffer instances
static std::vector<LLVertexBuffer*> sMappedBuffers;

//static
void LLVertexBuffer::flushBuffers()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // must only be called from main thread
    for (auto& buffer : sMappedBuffers)
    {
        buffer->_unmapBuffer();
        buffer->mMapped = false;
    }

    sMappedBuffers.resize(0);
}

//static
U32 LLVertexBuffer::calcOffsets(const U32& typemask, U32* offsets, U32 num_vertices)
{
    U32 offset = 0;
    for (U32 i=0; i<TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            if (offsets && LLVertexBuffer::sTypeSize[i])
            {
                offsets[i] = offset;
                offset += LLVertexBuffer::sTypeSize[i]*num_vertices;
                offset = (offset + 0xF) & ~0xF;
            }
        }
    }

    offsets[TYPE_TEXTURE_INDEX] = offsets[TYPE_VERTEX] + 12;

    return offset;
}

//static
U32 LLVertexBuffer::calcVertexSize(const U32& typemask)
{
    U32 size = 0;
    for (U32 i = 0; i < TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            size += LLVertexBuffer::sTypeSize[i];
        }
    }

    return size;
}

// protected, use unref()
//virtual
LLVertexBuffer::~LLVertexBuffer()
{
    if (mMapped)
    { // is on the mapped buffer list but doesn't need to be flushed
        mMapped = false;
        unmapBuffer();
    }

    destroyGLBuffer();
    destroyGLIndices();

    if (mMappedData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's vertices" << LL_ENDL;
    }
    if (mMappedIndexData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's indices" << LL_ENDL;
    }
};

//----------------------------------------------------------------------------

void LLVertexBuffer::genBuffer(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sBackend);

    if (sBackend)
    {
        llassert(mSize == 0);
        llassert(!mBuffer);
        llassert(mMappedData == nullptr);

        mSize = size;
        sBackend->allocate(BufferKind::Vertex, mSize, mBuffer, mMappedData);
    }
}

void LLVertexBuffer::genIndices(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sBackend);

    if (sBackend)
    {
        llassert(mIndicesSize == 0);
        llassert(!mIndices);
        llassert(mMappedIndexData == nullptr);
        mIndicesSize = size;
        sBackend->allocate(BufferKind::Index, mIndicesSize, mIndices, mMappedIndexData);
    }
}

bool LLVertexBuffer::createGLBuffer(U32 size)
{
    if (mBuffer || mMappedData)
    {
        destroyGLBuffer();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genBuffer(size);

    if (!mMappedData)
    {
        success = false;
    }
    return success;
}

bool LLVertexBuffer::createGLIndices(U32 size)
{
    if (mIndices)
    {
        destroyGLIndices();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genIndices(size);

    if (!mMappedIndexData)
    {
        success = false;
    }
    return success;
}

void LLVertexBuffer::destroyGLBuffer()
{
    if (mBuffer || mMappedData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sBackend);
        if (sBackend)
        {
            sBackend->free(BufferKind::Vertex, mSize, mBuffer, mMappedData);
        }

        mSize = 0;
        mBuffer = {};
        mMappedData = nullptr;
    }
}

void LLVertexBuffer::destroyGLIndices()
{
    if (mIndices || mMappedIndexData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sBackend);
        if (sBackend)
        {
            sBackend->free(BufferKind::Index, mIndicesSize, mIndices, mMappedIndexData);
        }

        mIndicesSize = 0;
        mIndices = {};
        mMappedIndexData = nullptr;
    }
}

bool LLVertexBuffer::updateNumVerts(U32 nverts)
{
    llassert(nverts >= 0);

    bool success = true;

    U32 needed_size = calcOffsets(mTypeMask, mOffsets, nverts);

    if (needed_size != mSize)
    {
        success &= createGLBuffer(needed_size);
    }

    llassert(mSize == needed_size);
    mNumVerts = nverts;
    return success;
}

bool LLVertexBuffer::updateNumIndices(U32 nindices)
{
    llassert(nindices >= 0);

    bool success = true;

    U32 needed_size = sizeof(U16) * nindices;

    if (needed_size != mIndicesSize)
    {
        success &= createGLIndices(needed_size);
    }

    llassert(mIndicesSize == needed_size);
    mNumIndices = nindices;
    return success;
}

bool LLVertexBuffer::allocateBuffer(U32 nverts, U32 nindices)
{
    if (nverts < 0 || nindices < 0)
    {
        LL_ERRS() << "Bad vertex buffer allocation: " << nverts << " : " << nindices << LL_ENDL;
    }

    bool success = true;

    success &= updateNumVerts(nverts);
    success &= updateNumIndices(nindices);

    return success;
}

//----------------------------------------------------------------------------

// if no gap between region and given range exists, expand region to cover given range and return true
// otherwise return false
bool expand_region(LLVertexBuffer::MappedRegion& region, U32 start, U32 end)
{

    if (end < region.mStart ||
        start > region.mEnd)
    { //gap exists, do not merge
        return false;
    }

    region.mStart = llmin(region.mStart, start);
    region.mEnd = llmax(region.mEnd, end);

    return true;
}


// Map for data access
U8* LLVertexBuffer::mapVertexBuffer(LLVertexBuffer::AttributeType type, U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();

    if (count == -1)
    {
        count = mNumVerts - index;
    }

    if (!sBackend->usesDeferredUpload())
    {
        U32 start = mOffsets[type] + sTypeSize[type] * index;
        U32 end = start + sTypeSize[type] * count-1;

        bool flagged = false;
        // flag region as mapped
        for (U32 i = 0; i < mMappedVertexRegions.size(); ++i)
        {
            MappedRegion& region = mMappedVertexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedVertexRegions.push_back({ start, end });
        }
    }
    return mMappedData+mOffsets[type]+sTypeSize[type]*index;
}


U8* LLVertexBuffer::mapIndexBuffer(U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();

    if (count == -1)
    {
        count = mNumIndices-index;
    }

    if (!sBackend->usesDeferredUpload())
    {
        U32 start = sizeof(U16) * index;
        U32 end = start + sizeof(U16) * count-1;

        bool flagged = false;
        // flag region as mapped
        for (U32 i = 0; i < mMappedIndexRegions.size(); ++i)
        {
            MappedRegion& region = mMappedIndexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedIndexRegions.push_back({ start, end });
        }
    }

    return mMappedIndexData + sizeof(U16)*index;
}

// flush the given byte range
//  kind -- which buffer (vertex or index) is being written
//  start -- first byte to copy
//  end -- last byte to copy (NOT last byte + 1)
//  data -- data to be flushed
//  dst -- mMappedData or mMappedIndexData
void LLVertexBuffer::flush_vbo(BufferKind kind, U32 start, U32 end, void* data, U8* dst)
{
    if (sBackend->usesDeferredUpload())
    {
        // the GPU buffer is created at unmap time, so flush_vbo doesn't actually write to it;
        // be sure to call _mapBuffer to tag the buffer for flushing
        _mapBuffer();
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb memcpy");
        // copy into mapped buffer
        memcpy(dst+start, data, end-start+1);
    }
    else
    {
        sBackend->upload(kind, kind == BufferKind::Vertex ? mBuffer : mIndices, start, end, data);
    }
}

void LLVertexBuffer::unmapBuffer()
{
    flushBuffers();
}

void LLVertexBuffer::_mapBuffer()
{
    if (!mMapped)
    {
        mMapped = true;
        sMappedBuffers.push_back(this);
    }
}

void LLVertexBuffer::_unmapBuffer()
{
    STOP_GLERROR;
    if (!mMapped)
    {
        return;
    }

    struct SortMappedRegion
    {
        bool operator()(const MappedRegion& lhs, const MappedRegion& rhs)
        {
            return lhs.mStart < rhs.mStart;
        }
    };

    if (sBackend->usesDeferredUpload())
    {
        if (mMappedData)
        {
            sBackend->createStatic(BufferKind::Vertex, mBuffer, mSize, mMappedData);
        }
        else
        {
            sBackend->bind(BufferKind::Vertex, mBuffer);
        }

        if (mMappedIndexData)
        {
            sBackend->createStatic(BufferKind::Index, mIndices, mIndicesSize, mMappedIndexData);
        }
        else
        {
            sBackend->bind(BufferKind::Index, mIndices);
        }
    }
    else
    {
        if (!mMappedVertexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - vertex");

            sBackend->bind(BufferKind::Vertex, mBuffer);

            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedVertexRegions.begin(), mMappedVertexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedVertexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedVertexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(BufferKind::Vertex, start, end, (U8*)mMappedData + start, mMappedData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(BufferKind::Vertex, start, end, (U8*)mMappedData + start, mMappedData);
            mMappedVertexRegions.clear();
        }

        if (!mMappedIndexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - index");

            sBackend->bind(BufferKind::Index, mIndices);
            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedIndexRegions.begin(), mMappedIndexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedIndexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedIndexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(BufferKind::Index, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(BufferKind::Index, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
            mMappedIndexRegions.clear();
        }
    }
}

//----------------------------------------------------------------------------

template <class T,LLVertexBuffer::AttributeType type> struct VertexBufferStrider
{
    typedef LLStrider<T> strider_t;
    static bool get(LLVertexBuffer& vbo,
                    strider_t& strider,
                    S32 index, S32 count)
    {
        if (type == LLVertexBuffer::TYPE_INDEX)
        {
            U8* ptr = vbo.mapIndexBuffer(index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapIndexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(0);
            return true;
        }
        else if (vbo.hasDataType(type))
        {
            U32 stride = LLVertexBuffer::sTypeSize[type];

            U8* ptr = vbo.mapVertexBuffer(type, index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapVertexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(stride);
            return true;
        }
        else
        {
            LL_ERRS() << "VertexBufferStrider could not find valid vertex data." << LL_ENDL;
        }
        return false;
    }
};

bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getIndexStrider(LLStrider<U16>& strider, U32 index, S32 count)
{
    llassert(mIndicesStride == 2); // cannot access 32-bit indices with U16 strider
    llassert(mIndexType == IndexType::U16);
    return VertexBufferStrider<U16,TYPE_INDEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord0Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD0>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord1Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD1>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord2Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD2>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a, TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getColorStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_COLOR>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getEmissiveStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_EMISSIVE>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getWeightStrider(LLStrider<F32>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<F32,TYPE_WEIGHT>::get(*this, strider, index, count);
}

// <FS:Ansariel> Vectorized Weight4Strider and ClothWeightStrider by Drake Arconis
//bool LLVertexBuffer::getWeight4Strider(LLStrider<LLVector4>& strider, U32 index, S32 count)
//{
//  return VertexBufferStrider<LLVector4,TYPE_WEIGHT4>::get(*this, strider, index, count);
//}
//
//bool LLVertexBuffer::getClothWeightStrider(LLStrider<LLVector4>& strider, U32 index, S32 count)
//{
//  return VertexBufferStrider<LLVector4,TYPE_CLOTHWEIGHT>::get(*this, strider, index, count);
//}
bool LLVertexBuffer::getWeight4Strider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_WEIGHT4>::get(*this, strider, index, count);
}

bool LLVertexBuffer::getClothWeightStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_CLOTHWEIGHT>::get(*this, strider, index, count);
}
// </FS:Ansariel>

//----------------------------------------------------------------------------


// Set for rendering
void LLVertexBuffer::setBuffer()
{
    STOP_GLERROR;

    if (mMapped)
    {
        LL_WARNS_ONCE() << "Missing call to unmapBuffer or flushBuffers" << LL_ENDL;
        _unmapBuffer();
    }

    // no data may be pending
    llassert(mMappedVertexRegions.empty());
    llassert(mMappedIndexRegions.empty());

    // a shader must be bound
    llassert(LLGLSLShader::sCurBoundShaderPtr);

    U32 data_mask = LLGLSLShader::sCurBoundShaderPtr->mAttributeMask;

    // this Vertex Buffer must provide all necessary attributes for currently bound shader
    llassert_msg((data_mask & mTypeMask) == data_mask,
        "Attribute mask mismatch! mTypeMask should be a superset of data_mask.  data_mask: 0x"
                << std::hex << data_mask << " mTypeMask: 0x" << mTypeMask << " Missing: 0x" << (data_mask & ~mTypeMask) <<  std::dec);

    sBackend->setBuffers(mBuffer, mIndices, data_mask, mOffsets);
}



void LLVertexBuffer::setPositionData(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, 0, sizeof(LLVector4a) * getNumVerts()-1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + sTypeSize[TYPE_TEXCOORD0] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + sTypeSize[TYPE_TEXCOORD1] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_COLOR], mOffsets[TYPE_COLOR] + sTypeSize[TYPE_COLOR] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + sTypeSize[TYPE_NORMAL] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + sTypeSize[TYPE_TANGENT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + sTypeSize[TYPE_WEIGHT4] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_JOINT], mOffsets[TYPE_JOINT] + sTypeSize[TYPE_JOINT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data)
{
    flush_vbo(BufferKind::Index, 0, sizeof(U16) * getNumIndices() - 1, (U8*) data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data)
{
    if (mIndexType != IndexType::U32)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndexType = IndexType::U32;
        mIndicesStride = 4;
        mNumIndices /= 2;
    }
    flush_vbo(BufferKind::Index, 0, sizeof(U32) * getNumIndices() - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setPositionData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, offset * sizeof(LLVector4a), (offset + count) * sizeof(LLVector4a) - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_NORMAL] + offset * sTypeSize[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + (offset + count) * sTypeSize[TYPE_NORMAL] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD0] + offset * sTypeSize[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + (offset + count) * sTypeSize[TYPE_TEXCOORD0] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD1] + offset * sTypeSize[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + (offset + count) * sTypeSize[TYPE_TEXCOORD1] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_COLOR] + offset * sTypeSize[TYPE_COLOR], mOffsets[TYPE_COLOR] + (offset + count) * sTypeSize[TYPE_COLOR] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TANGENT] + offset * sTypeSize[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + (offset + count) * sTypeSize[TYPE_TANGENT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_WEIGHT4] + offset * sTypeSize[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + (offset + count) * sTypeSize[TYPE_WEIGHT4] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_JOINT] + offset * sTypeSize[TYPE_JOINT], mOffsets[TYPE_JOINT] + (offset + count) * sTypeSize[TYPE_JOINT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Index, offset * sizeof(U16), (offset + count) * sizeof(U16) - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data, U32 offset, U32 count)
{
    if (mIndexType != IndexType::U32)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndexType = IndexType::U32;
        mIndicesStride = 4;
        mNumIndices /= 2;
    }
    flush_vbo(BufferKind::Index, offset * sizeof(U32), (offset + count) * sizeof(U32) - 1, (U8*)data, mMappedIndexData);
}




