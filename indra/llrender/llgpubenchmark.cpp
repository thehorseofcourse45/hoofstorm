/**
 * @file llgpubenchmark.cpp
 * @brief GL functionality access
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

/**
 * Contains ALL methods which directly access GL functionality
 * except for core rendering engine functionality.
 */

#include "linden_common.h"
#include "llgpubenchmark.h"
#include "llrenderbackend.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llimagegl.h"
#include "llgl.h"
#include "llframebufferbackend.h"
#include <memory>
#include "lltimer.h"
#include "llrand.h"
#include <cmath>

namespace
{
struct ShaderProfileHelper
{
    ShaderProfileHelper()
    {
        LLGLSLShader::initProfile();
    }
    ~ShaderProfileHelper()
    {
        LLGLSLShader::finishProfile();
    }
};

// This helper class is used to ensure that each generateTextures() call
// is matched by a corresponding deleteTextures() call. It also handles
// the bindManual() calls using those textures.
class TextureHolder
{
public:
    TextureHolder(U32 unit, U32 size) :
        texUnit(gGL.getTexUnit(unit)),
        source(size)            // preallocate vector
    {
        // takes (count, pointer)
        // &vector[0] gets pointer to contiguous array
        LLImageGL::generateTextures(static_cast<S32>(source.size()), &source[0]);
    }

    ~TextureHolder()
    {
        // unbind
        if (texUnit)
        {
                texUnit->unbind(LLTexUnit::TT_TEXTURE);
        }
        // ensure that we delete these textures regardless of how we exit
        LLImageGL::deleteTextures(static_cast<S32>(source.size()), &source[0]);
    }

    bool bind(U32 index)
    {
        if (texUnit) // should always be there with dummy (-1), but just in case
        {
            return texUnit->bindManual(LLTexUnit::TT_TEXTURE, source[index]);
        }
        return false;
    }

private:
    // capture which LLTexUnit we're going to use
    LLTexUnit* texUnit;

    // use std::vector for implicit resource management
    std::vector<U32> source;
};

F32 shader_timer_benchmark(std::vector<LLRenderTarget> & dest, TextureHolder & texHolder, U32 textures_count, LLVertexBuffer * buff, LLGLSLShader& shader, const std::vector<U8>& pixels, F32 &seconds)
{
    // run GPU timer benchmark

    //number of samples to take
    const S32 samples = 64;

    const bool native=isVulkanRenderBackendActive();
    const U64 expected=U64(1024)*1024*textures_count*samples;
    {
        shader.clearStats();
        std::unique_ptr<ShaderProfileHelper> profile;
        if (!native) profile=std::make_unique<ShaderProfileHelper>();
        struct Timer
        {
            U32 id=0; bool active=false;
            ~Timer() { if (active) endRenderQuery(GL_TIME_ELAPSED); if (id) deleteRenderQueries(1,&id); }
        } timer;
        dest[0].bindTarget();
        shader.bind();
        if (native)
        {
            generateRenderQueries(1,&timer.id); beginRenderQuery(GL_TIME_ELAPSED,timer.id);timer.active=true;
        }
        for (S32 c = 0; c < samples; ++c)
        {
            for (U32 i = 0; i < textures_count; ++i)
            {
                texHolder.bind(i);
                buff->setBuffer();
                buff->drawArrays(LLRender::TRIANGLES, 0, 3);
            }
        }
        if (native) { endRenderQuery(GL_TIME_ELAPSED);timer.active=false; }
        shader.unbind();
        if (native)
        {
            unsigned long long ns=0;getRenderQuery64(timer.id,GL_QUERY_RESULT,&ns);
            shader.mTimeElapsed=ns;
            // Fixed full-target draws define the byte budget; verify the output before accepting a timestamp rate.
            shader.mSamplesDrawn=expected;
        }
        const S32 points[][2]={{0,0},{1023,0},{0,1023},{1023,1023},{512,512}};
        bool correct=true;
        for (const auto& point:points)
        {
            U8 actual[4]{};getFramebufferBackend().readPixels(point[0],point[1],1,1,GL_RGBA,GL_UNSIGNED_BYTE,actual);
            correct &= !memcmp(actual,pixels.data()+4*(point[1]*1024+point[0]),4);
        }
        dest[0].flush();
        if (!correct) return -1.f;
    }

    F32 ms = shader.mTimeElapsed / 1000000.f;
    seconds = ms / 1000.f;

    F64 samples_drawn = (F64)shader.mSamplesDrawn;
    F64 gpixels_drawn = samples_drawn / 1000000000.0;
    if (!std::isfinite(seconds) || seconds<=0 || shader.mSamplesDrawn!=expected) return -1.f;
    F32 samples_sec = (F32)(gpixels_drawn / seconds);
    return samples_sec * 4;  // 4 bytes per sample
}

}

F32 measureGpuBandwidth(LLGLSLShader& shader)
{
    if (!LLGLSLShader::sCanProfile || LLGLSLShader::sProfileEnabled || !renderQuerySupported(GL_TIME_ELAPSED) ||
        (!isVulkanRenderBackendActive() && (!renderQuerySupported(GL_SAMPLES_PASSED) ||
        !renderQuerySupported(GL_PRIMITIVES_GENERATED)))) return -1.f;
    const bool ownFrame=isVulkanRenderBackendActive() && !isRenderBackendFrameActive();
    const float clear[4]={0,0,0,1};
    if (ownFrame && !beginRenderBackendFrame(clear)) return -1.f;
    struct FrameEnd { bool own; ~FrameEnd() { if (own) endRenderBackendFrame(); } } frameEnd{ownFrame};
    pushSyncPipelines();   // timing passes must draw every time
    struct SyncScope { ~SyncScope() { popSyncPipelines(); } } syncScope;
    gGL.flush();
    LLGLDisable blend(GL_BLEND), scissor(GL_SCISSOR_TEST), cull(GL_CULL_FACE);

    //measure memory bandwidth by:
    // - allocating a batch of textures and render targets
    // - rendering those textures to those render targets
    // - recording time taken
    // - taking the median time for a given number of samples

    //resolution of textures/render targets
    const U32 res = 1024;

    //number of textures
    const U32 count = 32;

    //time limit, allocation operations shouldn't take longer then 30 seconds, same for actual benchmark.
    const F32 time_limit = 30.f;

    std::vector<LLRenderTarget> dest(count);
    TextureHolder texHolder(0, count);

    //build a random texture
    std::vector<U8> pixels(res*res*4);

    for (U32 i = 0; i < res*res*4; ++i)
    {
        pixels[i] = (U8) ll_rand(255);
    }

    gGL.setColorMask(true, true);
    LLGLDepthTest depth(GL_FALSE);

    LLTimer alloc_timer;
    alloc_timer.start();
    for (U32 i = 0; i < count; ++i)
    {
        //allocate render targets and textures
        if (!dest[i].allocate(res, res, GL_RGBA))
        {
            LL_WARNS("Benchmark") << "Failed to allocate render target." << LL_ENDL;
            // abandon the benchmark test
            return -1.f;
        }
        dest[i].bindTarget();
        dest[i].clear();
        dest[i].flush();

        if (!texHolder.bind(i))
        {
            // can use a dummy value mDummyTexUnit = new LLTexUnit(-1);
            LL_WARNS("Benchmark") << "Failed to bind tex unit." << LL_ENDL;
            // abandon the benchmark test
            return -1.f;
        }
        LLImageGL::setManualImage(GL_TEXTURE_2D, 0, GL_RGBA, res,res,GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        // disable mipmaps and use point filtering to cause cache misses
        gGL.getTexUnit(0)->setHasMipMaps(false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);

        if (alloc_timer.getElapsedTimeF32() > time_limit)
        {
            // abandon the benchmark test
            LL_WARNS("Benchmark") << "Allocation operation took longer then 30 seconds, stopping." << LL_ENDL;
            return -1.f;
        }
    }


    //make a dummy triangle to draw with
    LLPointer<LLVertexBuffer> buff = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);

    if (!buff->allocateBuffer(3, 0))
    {
        LL_WARNS("Benchmark") << "Failed to allocate buffer during benchmark." << LL_ENDL;
        // abandon the benchmark test
        return -1.f;
    }

    LLStrider<LLVector3> v;

    if (! buff->getVertexStrider(v))
    {
        LL_WARNS("Benchmark") << "GL LLVertexBuffer::getVertexStrider() returned false, "
                   << "buff->getMappedData() is"
                   << (buff->getMappedData()? " not" : "")
                   << " NULL" << LL_ENDL;
        // abandon the benchmark test
        return -1.f;
    }

    // generate dummy triangle
    v[0].set(-1, 1, 0);
    v[1].set(-1, -3, 0);
    v[2].set(3, 1, 0);

    buff->unmapBuffer();

    LLGLSLShader::unbind();

    // Complete allocation clears/uploads before starting the measured workload.
    waitRenderBackendIdle();

    // run GPU timer benchmark twice
    F32 seconds = 0;
    F32 gbps = shader_timer_benchmark(dest, texHolder, count, buff.get(), shader, pixels, seconds);

    LL_INFOS("Benchmark") << "Memory bandwidth, 1st run is " << llformat("%.3f", gbps) << " GB/sec according to GPU timestamp queries, total time " << seconds << " seconds" << LL_ENDL;

    if (gbps<=0 || alloc_timer.getElapsedTimeF32()>time_limit) return -1.f;
    gbps = shader_timer_benchmark(dest, texHolder, count, buff.get(), shader, pixels, seconds);

    LL_INFOS("Benchmark") << "Memory bandwidth, final run is " << llformat("%.3f", gbps) << " GB/sec according to GPU timestamp queries, total time " << seconds << " seconds" << LL_ENDL;

    return std::isfinite(gbps) && gbps>0 ? gbps : -1.f;
}
