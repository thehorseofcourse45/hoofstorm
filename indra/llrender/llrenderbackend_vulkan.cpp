/**
 * @file llrenderbackend.cpp
 * @brief Viewer startup backend selection
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
#include "llrenderbackend.h"
#include "llerrorcontrol.h"
#include <io.h>
#include <memory>
#include <exception>
#include "llgl.h"
#include <algorithm>
#include <mutex>
#include <set>

#ifdef LL_VULKAN_RUNTIME_AVAILABLE
#include "vulkan/vkcontext.h"
#include "vulkan/vkbackends.h"
#include "vulkan/vkshaderbackend.h"
#include "vulkan/vktexturebackend.h"
#include "vulkan/vkframebufferbackend.h"
#include "vulkan/vkquerybackend.h"
#include "llrenderbackend_threaded.h"
namespace
{
std::uint32_t sErrors = 0, sWarnings = 0;
bool sDeferPresentation = false;
struct NativeRenderer
{
    VkContext context;
    // Threaded renderer (on unless FS_VK_RENDER_THREAD=0, doc/vulkan/render-thread-design.md): the viewer's thread talks to proxies and
    // the render thread owns every real backend call. All of these stay empty when it is switched off.
    std::unique_ptr<rt::Thread> thread;
    rt::Shared shared;
    std::unique_ptr<rt::StateProxy> stateProxy;
    std::unique_ptr<rt::ShaderProxy> shaderProxy;
    std::unique_ptr<rt::TextureProxy> textureProxy;
    std::unique_ptr<rt::FramebufferProxy> framebufferProxy;
    bool frameActive = false;                // main thread's view of context.recording while threaded
    std::mutex queryMutex;                   // queries whose begin() is queued but not yet run: their old result must not be reported
    std::set<std::uint32_t> queryBeginPending;
    std::uint32_t nextUniformBuffer = 0;     // uniform buffer ids handed out by the main thread while threaded
    rt::Thread::Stats previousThreadStats;
    bool beginThreadedFrame(const float clear[4])
    {
        if (frameActive) return true;
        if (!context.headless)
        {
            // the same checks VkContext::beginFrame makes before it touches the swapchain
            RECT client{};
            if (!IsWindow(context.window) || !GetClientRect(context.window, &client) || IsIconic(context.window) ||
                client.right <= client.left || client.bottom <= client.top) return false;
        }
        frameActive = true;
        const std::array<float, 4> color{clear[0], clear[1], clear[2], clear[3]};
        thread->post([ctx = &context, sh = &shared, color] {
            sh->frameFailed = false;
            if (!ctx->recording && !ctx->beginFrame(color.data())) sh->frameFailed = true;     // draws of this frame are dropped
        });
        return true;
    }
    void endThreadedFrame()
    {
        if (!frameActive || sDeferPresentation) return;
        frameActive = false;
        thread->post([this, t = thread.get()] {
            if (context.recording)
            {
                context.endFrame(false);
                logFramePacing();
            }
            t->frameDone();
        });
        thread->frameQueued();
        thread->throttle(1);     // at most one finished frame waiting for the render thread
    }
    uint32_t pacingFrames = 0;
    double recordingTotal = 0, recordingMax = 0, fenceTotal = 0, acquireTotal = 0, presentTotal = 0;
    uint64_t previousDraws = 0, previousPasses = 0, previousBarriers = 0, previousSamplerSets = 0;
    uint64_t previousBlocking = 0, previousRenames = 0, previousPipelines = 0, previousSkipped = 0;
    double previousPipelineMs = 0, gpuTotal = 0, gpuMax = 0;
    std::array<uint64_t,VkContext::stallKinds> previousStalls{};
    std::array<double,VkContext::stallKinds> previousStallMs{};
    uint64_t previousTextureUploadAllocations=0, previousTextureRingUploads=0;
    uint64_t previousCacheHits=0, previousBufferAllocations=0, previousBufferPoolHits=0, previousBufferPoolEvictions=0;
    std::array<std::array<double,300>,5> samples{};
    void logFramePacing()
    {
        samples[0][pacingFrames]=context.lastRecordingMs;
        samples[1][pacingFrames]=context.lastFenceWaitMs;
        samples[2][pacingFrames]=context.lastAcquireWaitMs;
        samples[3][pacingFrames]=context.lastPresentMs;
        samples[4][pacingFrames]=context.lastGpuMs;
        ++pacingFrames;
        gpuTotal += context.lastGpuMs; gpuMax = std::max(gpuMax, context.lastGpuMs);
        recordingTotal += context.lastRecordingMs;
        recordingMax = std::max(recordingMax, context.lastRecordingMs);
        fenceTotal += context.lastFenceWaitMs;
        acquireTotal += context.lastAcquireWaitMs;
        presentTotal += context.lastPresentMs;
        if (pacingFrames != 300) return;
        for (auto& values:samples) std::sort(values.begin(),values.end());
        LL_INFOS("VulkanPacing") << "300 frames; recording avg/max ms=" << recordingTotal/300 << "/" << recordingMax
            << "; fence/acquire/present avg ms=" << fenceTotal/300 << "/" << acquireTotal/300 << "/" << presentTotal/300
            << "; GPU avg/max ms=" << gpuTotal/300 << "/" << gpuMax
            << "; blocking submits=" << context.blockingSubmissions-previousBlocking
            << "; buffer versions=" << context.bufferRenames-previousRenames
            << "; VRAM buffer copies made/dropped (total)=" << context.bufferPromotions << "/" << context.bufferDemotions
            << " holding MB=" << context.bufferVramBytes / 1048576.0
            << "; pipelines=" << context.pipelinesCreated-previousPipelines
            << "; draws skipped for pending pipelines=" << context.pipelineDrawsSkipped-previousSkipped
            << "; feedback substitutions=" << context.feedbackSubstitutions
            << "; frames in flight=" << context.slotCount
            << "; pipeline build ms=" << context.pipelineCreationMs-previousPipelineMs
            << "; cache hits=" << context.pipelineCacheHits-previousCacheHits
            << "; recording/fence/acquire/present/GPU p95 ms=" << samples[0][284] << "/" << samples[1][284]
            << "/" << samples[2][284] << "/" << samples[3][284] << "/" << samples[4][284]
            << "; fence/acquire/present max ms=" << samples[1][299] << "/" << samples[2][299] << "/" << samples[3][299] << LL_ENDL;
        static const char* names[]={"readback","query","uniform-ring","buffer-reuse","upload","explicit"};
        for (size_t i=0;i<VkContext::stallKinds;++i)
        {
            LL_INFOS("VulkanPacing") << "blocking " << names[i] << "; calls=" << context.stallCounts[i]-previousStalls[i]
                << "; total ms=" << context.stallMs[i]-previousStallMs[i] << LL_ENDL;
        }
        LL_INFOS("VulkanPacing") << "per frame: draws=" << (context.statDraws-previousDraws)/300.0 << "; render passes=" << (context.statRenderPasses-previousPasses)/300.0
            << "; texture barriers=" << (g_barrierCount-previousBarriers)/300.0 << "; push descriptors=" << context.pushDescriptors << "; narrow barriers=" << g_narrowBarriers << "; new sampler sets=" << (context.statSamplerSets-previousSamplerSets)/300.0 << LL_ENDL;
        {
            std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::array<uint32_t, 4>>> byDraws;
            for (const auto& e : context.statPassDraws) byDraws.emplace_back(e.second, e.first);
            std::sort(byDraws.rbegin(), byDraws.rend());
            for (size_t i = 0; i < std::min<size_t>(byDraws.size(), 14); ++i)
                LL_INFOS("VulkanPacing") << "target fb=" << byDraws[i].second[0] << " " << byDraws[i].second[1] << "x" << byDraws[i].second[2]
                    << " colors=" << byDraws[i].second[3] << ": draws/frame=" << byDraws[i].first.first / 300.0
                    << " passes/frame=" << byDraws[i].first.second / 300.0 << LL_ENDL;
            context.statPassDraws.clear();
        }
        if (context.queries && !context.queries->passTimes.empty())     // on unless FS_VK_PASS_TIMING=0
        {
            std::vector<std::pair<double, std::pair<std::array<uint32_t, 4>, uint64_t>>> byTime;
            std::map<std::array<uint32_t, 4>, double> gaps;
            double total = 0, totalGap = 0;
            for (const auto& e : context.queries->passTimes)
            {
                byTime.push_back({e.second.ms, {e.first, e.second.count}}); total += e.second.ms;
                gaps[e.first] = e.second.gapMs; totalGap += e.second.gapMs;
            }
            std::sort(byTime.rbegin(), byTime.rend());
            LL_INFOS("VulkanPacing") << "pass GPU ms/frame total=" << total / 300.0 << " (sum of render pass bodies); gap before passes="
                << totalGap / 300.0 << " (barriers, transitions, idle between passes)" << LL_ENDL;
            for (size_t i = 0; i < std::min<size_t>(byTime.size(), 14); ++i)
            {
                const auto& key = byTime[i].second.first;
                LL_INFOS("VulkanPacing") << "pass GPU fb=" << key[0] << " " << key[1] << "x" << key[2] << " colors=" << key[3]
                    << ": ms/frame=" << byTime[i].first / 300.0 << " gap before ms/frame=" << gaps[key] / 300.0
                    << " passes/frame=" << byTime[i].second.second / 300.0 << LL_ENDL;
            }
            context.queries->passTimes.clear();
            if (!context.queries->programTimes.empty())
            {
                std::vector<std::pair<double, uint32_t>> byProgram;
                for (const auto& e : context.queries->programTimes) byProgram.push_back({e.second.ms, e.first});
                std::sort(byProgram.rbegin(), byProgram.rend());
                LL_INFOS("VulkanPacing") << "G-buffer pass by shader: setup before first draw ms/frame=" << context.queries->programSetupMs / 300.0 << LL_ENDL;
                for (size_t i = 0; i < std::min<size_t>(byProgram.size(), 14); ++i)
                {
                    const auto& t = context.queries->programTimes[byProgram[i].second];
                    LL_INFOS("VulkanPacing") << "G-buffer shader " << getShaderDebugName(byProgram[i].second) << " #" << byProgram[i].second
                        << ": ms/frame=" << t.ms / 300.0 << " draws/frame=" << t.draws / 300.0 << " stretches/frame=" << t.stretches / 300.0
                        << " Kverts/frame=" << t.vertices / 300e3 << " Kvs/frame=" << t.vsInvocations / 300e3
                        << " Kfrags/frame=" << t.fragments / 300e3 << LL_ENDL;
                }
                context.queries->programTimes.clear();
                context.queries->programSetupMs = 0;
                std::vector<std::pair<double, uint32_t>> byDraw;
                for (const auto& e : context.queries->drawTimes) byDraw.push_back({e.second, e.first});
                std::sort(byDraw.rbegin(), byDraw.rend());
                for (size_t i = 0; i < std::min<size_t>(byDraw.size(), 12); ++i)
                    LL_INFOS("VulkanPacing") << "G-buffer draw #" << byDraw[i].second << " of studied shader: ms/frame=" << byDraw[i].first / 300.0 << LL_ENDL;
                context.queries->drawTimes.clear();
            }
            std::vector<std::pair<double, std::pair<uint32_t, uint32_t>>> byScreen;
            for (const auto& e : context.queries->screenProgramTimes) byScreen.push_back({e.second.ms, e.first});
            std::sort(byScreen.rbegin(), byScreen.rend());
            for (size_t i = 0; i < std::min<size_t>(byScreen.size(), 16); ++i)
            {
                const auto& t = context.queries->screenProgramTimes[byScreen[i].second];
                LL_INFOS("VulkanPacing") << "screen pass fb=" << byScreen[i].second.first << " shader " << getShaderDebugName(byScreen[i].second.second)
                    << " #" << byScreen[i].second.second << ": ms/frame=" << t.ms / 300.0 << " draws/frame=" << t.draws / 300.0
                    << " stretches/frame=" << t.stretches / 300.0 << LL_ENDL;
            }
            context.queries->screenProgramTimes.clear();
        }
        if (thread)
        {
            const auto& now = thread->stats();     // written by the main thread; a torn read only skews one log line
            LL_INFOS("VulkanPacing") << "render thread per frame: records=" << (now.posts - previousThreadStats.posts) / 300.0
                << "; KiB=" << (now.bytes - previousThreadStats.bytes) / 1024.0 / 300.0
                << "; syncs=" << (now.syncs - previousThreadStats.syncs) / 300.0
                << "; sync wait ms=" << (now.syncMs - previousThreadStats.syncMs) / 300.0
                << "; throttle wait ms=" << (now.throttleMs - previousThreadStats.throttleMs) / 300.0 << LL_ENDL;
            previousThreadStats = now;
        }
        previousDraws=context.statDraws; previousPasses=context.statRenderPasses; previousBarriers=g_barrierCount; previousSamplerSets=context.statSamplerSets;
        LL_INFOS("VulkanPacing") << "buffer storage alloc/reuse/evict=" << context.bufferAllocations-previousBufferAllocations
            << "/" << context.bufferPoolHits-previousBufferPoolHits << "/" << context.bufferPoolEvictions-previousBufferPoolEvictions << LL_ENDL;
        previousBufferAllocations=context.bufferAllocations; previousBufferPoolHits=context.bufferPoolHits;
        previousBufferPoolEvictions=context.bufferPoolEvictions;
        LL_INFOS("VulkanPacing") << "texture staging alloc/ring=" << context.textureUploadAllocations-previousTextureUploadAllocations
            << "/" << context.textureRingUploads-previousTextureRingUploads << LL_ENDL;
        previousTextureUploadAllocations=context.textureUploadAllocations; previousTextureRingUploads=context.textureRingUploads;
        previousStalls=context.stallCounts; previousStallMs=context.stallMs;
        previousCacheHits=context.pipelineCacheHits;
        previousBlocking = context.blockingSubmissions;
        previousRenames = context.bufferRenames;
        previousPipelines = context.pipelinesCreated;
        previousSkipped = context.pipelineDrawsSkipped;
        previousPipelineMs = context.pipelineCreationMs;
        pacingFrames = 0;
        recordingTotal = recordingMax = fenceTotal = acquireTotal = presentTotal = gpuTotal = gpuMax = 0;
    }
    std::unique_ptr<VulkanRenderStateBackend> state;
    std::unique_ptr<VulkanShaderBackend> shaders;
    std::unique_ptr<VulkanTextureBackend> textures;
    std::unique_ptr<VulkanFramebufferBackend> framebuffers;
    std::unique_ptr<VulkanQueryBackend> queries;
    std::unique_ptr<VulkanVertexBufferBackend> vertices;
    ~NativeRenderer()
    {
        thread.reset();     // runs everything still queued, then joins
        if (context.device) vkDeviceWaitIdle(context.device);
        vertices.reset();
        queries.reset();
        framebuffers.reset();
        textures.reset();
        shaders.reset();
        state.reset();
        context.shutdown();
        sErrors = context.validationErrors;
        sWarnings = context.validationWarnings;
    }
};
NativeRenderer* sNative = nullptr;
}
#endif

bool activateVulkanRenderBackend(void* instance, void* window, bool validation, std::string& error)
{
    error.clear();
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative)
    {
        if (sNative->context.window == window) return true;
        error = "A Vulkan renderer already owns another window";
        return false;
    }
    if (renderBackendObjectsCreated())
    {
        error = "Backend objects already exist; startup API cannot be changed";
        return false;
    }
    if (wglGetCurrentContext())
    {
        error = "An OpenGL context is already current; startup API cannot be changed";
        return false;
    }
    if (!instance || !window || !IsWindow((HWND)window))
    {
        error = "Vulkan activation requires a valid native instance and window";
        return false;
    }
    // The GUI viewer has no stderr, so the backend's FATAL abort messages would vanish; keep them in the log folder.
    if (_fileno(stderr) < 0)
    {
        const char* appdata = std::getenv("APPDATA");
        std::string path = std::string(appdata ? appdata : ".") + "/Firestorm_x64/logs/vulkan_fatal.log";
        if (freopen(path.c_str(), "w", stderr)) LLError::stopLoggingToStderr();     // Firestorm.log already has the viewer log
    }
    try
    {
        auto candidate = std::make_unique<NativeRenderer>();
        {
            // two frames in flight unless FS_VK_FRAMES_IN_FLIGHT says otherwise
            const char* frames = std::getenv("FS_VK_FRAMES_IN_FLIGHT");
            candidate->context.requestedFramesInFlight = frames ? (unsigned) std::atoi(frames) : 2u;
        }
        if (!candidate->context.init((HINSTANCE)instance, (HWND)window, validation))
        {
            error = "Vulkan context initialization failed";
            return false;
        }
        if (validation && !candidate->context.validation)
        {
            error = "Requested Vulkan validation layer is unavailable";
            return false;
        }
        candidate->state = std::make_unique<VulkanRenderStateBackend>();
        candidate->shaders = std::make_unique<VulkanShaderBackend>(&candidate->context);
        candidate->textures = std::make_unique<VulkanTextureBackend>(&candidate->context);
        candidate->framebuffers = std::make_unique<VulkanFramebufferBackend>(&candidate->context, candidate->state.get());
        candidate->queries = std::make_unique<VulkanQueryBackend>(&candidate->context);
        candidate->vertices = std::make_unique<VulkanVertexBufferBackend>(&candidate->context, candidate->state.get());
        {
            const char* threaded = std::getenv("FS_VK_RENDER_THREAD");     // set to 0 to run backend calls on the viewer thread instead
            if (!(threaded && std::string(threaded) == "0"))
            {
                candidate->thread = std::make_unique<rt::Thread>();
                candidate->stateProxy = std::make_unique<rt::StateProxy>(*candidate->thread, candidate->state.get());
                candidate->shaderProxy = std::make_unique<rt::ShaderProxy>(*candidate->thread, candidate->shaders.get());
                candidate->textureProxy = std::make_unique<rt::TextureProxy>(*candidate->thread, candidate->textures.get());
                candidate->framebufferProxy = std::make_unique<rt::FramebufferProxy>(*candidate->thread, candidate->framebuffers.get());
                LL_INFOS("VulkanPacing") << "render thread enabled" << LL_ENDL;
            }
        }
        LL_INFOS("VulkanQueries") << "GPU timing=" << candidate->queries->supported(GL_TIME_ELAPSED)
            << "; occlusion=" << candidate->queries->supported(GL_ANY_SAMPLES_PASSED)
            << "; precise samples=" << candidate->queries->supported(GL_SAMPLES_PASSED)
            << "; primitive statistics=" << candidate->queries->supported(GL_PRIMITIVES_GENERATED) << LL_ENDL;
        // Publish only after the context and the complete backend set exist.
        {
            const char* strict = std::getenv("FS_VK_STRICT_FEEDBACK");   // set to 1 to abort on render-target feedback
            candidate->context.tolerateFeedback = !(strict && std::string(strict) == "1");
            const char* env = std::getenv("FS_VK_ASYNC_PIPELINES");   // set to 0 to build pipelines synchronously
            candidate->context.asyncPipelines = !(env && std::string(env) == "0");
        }
        sNative = candidate.release();
        sErrors = sWarnings = 0;
        return true;
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        return false;
    }
#else
    (void)instance; (void)window; (void)validation;
    error = "Vulkan runtime was not built";
    return false;
#endif
}

void setRenderBackendVsync(bool enable)
{
    VkContext::sVsync = enable;
}

bool isVulkanRenderBackendActive()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    return sNative != nullptr;
#else
    return false;
#endif
}
void deferRenderBackendPresentation(bool defer) { sDeferPresentation = defer; }
bool beginRenderBackendFrame(const float clear[4])
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread) return sNative->beginThreadedFrame(clear);
    if (sNative) return sNative->context.recording || sNative->context.beginFrame(clear);
#endif
    (void)clear;
    return true;
}
bool isRenderBackendFrameActive()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread) return sNative->frameActive;
    if (sNative) return sNative->context.recording;
#endif
    return true;
}
void endRenderBackendFrame()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread) { sNative->endThreadedFrame(); return; }
    if (sNative && sNative->context.recording && !sDeferPresentation)
    {
        sNative->context.endFrame(false);
        sNative->logFramePacing();
    }
#endif
}
void shutdownRenderBackend()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (!sNative) return;
    auto* old = sNative;
    sNative = nullptr;
    delete old;

#endif
}
void getRenderBackendValidationCounts(std::uint32_t& errors, std::uint32_t& warnings)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    errors = sNative ? sNative->context.validationErrors : sErrors;
    warnings = sNative ? sNative->context.validationWarnings : sWarnings;
#else
    errors = warnings = 0;
#endif
}
LLRenderStateBackend* activeVulkanRenderStateBackend()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative) return sNative->stateProxy ? static_cast<LLRenderStateBackend*>(sNative->stateProxy.get()) : sNative->state.get();
#endif
    return nullptr;
}
LLShaderBackend* activeVulkanShaderBackend()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative) return sNative->shaderProxy ? static_cast<LLShaderBackend*>(sNative->shaderProxy.get()) : sNative->shaders.get();
#endif
    return nullptr;
}
LLTextureBackend* activeVulkanTextureBackend()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative) return sNative->textureProxy ? static_cast<LLTextureBackend*>(sNative->textureProxy.get()) : sNative->textures.get();
#endif
    return nullptr;
}
LLFramebufferBackend* activeVulkanFramebufferBackend()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative) return sNative->framebufferProxy ? static_cast<LLFramebufferBackend*>(sNative->framebufferProxy.get()) : sNative->framebuffers.get();
#endif
    return nullptr;
}
LLVertexBufferBackend* createActiveVulkanVertexBufferBackend()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative)
    {
        VulkanVertexBufferBackend* real = sNative->vertices ? sNative->vertices.release()
            : new VulkanVertexBufferBackend(&sNative->context, sNative->state.get());
        if (sNative->thread) return new rt::VertexProxy(*sNative->thread, real, &sNative->shared);     // the proxy owns the real backend
        return real;
    }
#endif
    return nullptr;
}

void configureRenderPipelineCache(const std::string& file, bool writable)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative)
    {
        if (sNative->thread) sNative->thread->sync();
        sNative->context.configurePipelineCache(file, writable);
        LL_INFOS("VulkanPipelineCache") << "Loaded bytes=" << sNative->context.pipelineCacheLoadedBytes
            << "; writable=" << writable << "; file=" << file << LL_ENDL;
    }
#endif
}

void pushSyncPipelines()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread) { sNative->thread->post([ctx = &sNative->context] { ++ctx->syncPipelineScopes; }); return; }
    if (sNative) ++sNative->context.syncPipelineScopes;
#endif
}
void popSyncPipelines()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread) { sNative->thread->post([ctx = &sNative->context] { if (ctx->syncPipelineScopes > 0) --ctx->syncPipelineScopes; }); return; }
    if (sNative && sNative->context.syncPipelineScopes > 0) --sNative->context.syncPipelineScopes;
#endif
}

void waitRenderBackendIdle()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative)
    {
        if (sNative->thread) sNative->thread->sync();     // the render thread is idle from here, so this thread may drive the context
        if (sNative->context.recording) sNative->context.flushCommands(VkContext::Stall::Explicit);
        else
        {
            const auto start=std::chrono::steady_clock::now();
            VK_CHECK(vkDeviceWaitIdle(sNative->context.device));
            sNative->context.recordStall(VkContext::Stall::Explicit,start);
        }
        return;
    }
#endif
    glFinish();
}
bool initializeVulkanCapabilities(LLGLManager& m)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (!sNative) return false;
    auto& ctx = sNative->context;
    VkPhysicalDeviceProperties p{};
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceProperties(ctx.phys, &p);
    vkGetPhysicalDeviceFeatures(ctx.phys, &features);
    m.mIsAMD = p.vendorID == 0x1002;
    m.mIsNVIDIA = p.vendorID == 0x10de;
    m.mIsIntel = p.vendorID == 0x8086;
    m.mGLVendor = m.mGLVendorShort = m.mIsAMD ? "AMD" : m.mIsNVIDIA ? "NVIDIA" : m.mIsIntel ? "INTEL" : "MISC";
    m.mGLRenderer = p.deviceName;
    m.mGLVersionString = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(p.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_MINOR(p.apiVersion)) + "." + std::to_string(VK_API_VERSION_PATCH(p.apiVersion));
    m.mDriverVersionVendorString = std::to_string(p.driverVersion);
    // GLSL source compatibility for the existing viewer source assembler, not a claimed OpenGL context.
    m.mGLVersion = 4.5f; m.mGLSLVersionMajor = 4; m.mGLSLVersionMinor = 50;
    m.mDriverVersionMajor = VK_API_VERSION_MAJOR(p.apiVersion);
    m.mDriverVersionMinor = VK_API_VERSION_MINOR(p.apiVersion);
    m.mDriverVersionRelease = VK_API_VERSION_PATCH(p.apiVersion);
    m.mNumTextureImageUnits = std::min(32u, p.limits.maxPerStageDescriptorSamplers);
    m.mGLMaxTextureSize = p.limits.maxImageDimension2D;
    m.mGLMaxVertexRange = INT_MAX; m.mGLMaxIndexRange = INT_MAX;
    m.mMaxUniformBlockSize = std::min(65536u, p.limits.maxUniformBufferRange);
    m.mMaxVaryingVectors = p.limits.maxVertexOutputComponents / 4;
    // The viewer renders into single-sample targets; multisample images are not implemented by this backend.
    m.mMaxSamples = m.mMaxColorTextureSamples = m.mMaxDepthTextureSamples = m.mMaxIntegerSamples = 1;
    m.mMaxSampleMaskWords = 1;
    m.mHasCubeMapArray = features.imageCubeArray != 0;
    m.mHasAnisotropic = ctx.samplerAnisotropy; m.mMaxAnisotropy = ctx.maxAnisotropy;
    m.mHasDebugOutput = m.mHasTransformFeedback = false;
    m.mHasAMDAssociations = m.mHasNVXGpuMemoryInfo = m.mHasATIMemInfo = false;
    std::uint64_t bytes = 0;
    for (uint32_t i=0;i<ctx.memProps.memoryHeapCount;++i)
        if (ctx.memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) bytes += ctx.memProps.memoryHeaps[i].size;
    m.mVRAM = m.mVRAMDetected = static_cast<uint32_t>(bytes / (1024*1024));
    m.mInited = m.mHasRequirements = true;
    return true;
#else
    (void)m; return false;
#endif
}
std::uint32_t createRenderUniformBuffer()
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread)
    {
        const auto id=++sNative->nextUniformBuffer;
        sNative->thread->post([ctx=&sNative->context,id] { ctx->nextUniformBuffer=id; ctx->uniformBuffers.emplace(id, VkContext::UniformBuffer{}); });
        return id;
    }
    if (sNative)
    {
        auto& ctx=sNative->context;
        const auto id=++ctx.nextUniformBuffer;
        ctx.uniformBuffers.emplace(id, VkContext::UniformBuffer{});
        return id;
    }
#endif
    GLuint id=0; glGenBuffers(1,&id); return id;
}
void deleteRenderUniformBuffer(std::uint32_t buffer)
{
    if (!buffer) return;
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread)
    {
        sNative->thread->post([ctx=&sNative->context,buffer] {
            ctx->uniformBuffers.erase(buffer);
            for (auto& id:ctx->uniformBindings) if(id==buffer) id=0;
        });
        return;
    }
    if (sNative)
    {
        auto& ctx=sNative->context;
        ctx.uniformBuffers.erase(buffer);
        for (auto& id:ctx.uniformBindings) if(id==buffer) id=0;
        return;
    }
#endif
    glDeleteBuffers(1,&buffer);
}
void uploadRenderUniformBuffer(std::uint32_t buffer,const void* data,std::size_t bytes)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread)
    {
        sNative->thread->postData(data,bytes,[ctx=&sNative->context,buffer,bytes](const void* copy) {
            auto it=ctx->uniformBuffers.find(buffer);
            if(it==ctx->uniformBuffers.end() || bytes>ctx->uniformBlockLimit)
                throw std::runtime_error("Invalid Vulkan uniform buffer upload");
            auto& u=it->second;
            u.data.assign(ctx->uniformBlockLimit,0);
            if(bytes) memcpy(u.data.data(),copy,bytes);
            u.frame=0;
        });
        return;
    }
    if (sNative)
    {
        auto& ctx=sNative->context;
        auto it=ctx.uniformBuffers.find(buffer);
        if(it==ctx.uniformBuffers.end() || bytes>ctx.uniformBlockLimit || (bytes && !data))
            throw std::runtime_error("Invalid Vulkan uniform buffer upload");
        auto& u=it->second;
        u.data.assign(ctx.uniformBlockLimit,0);
        if(bytes) memcpy(u.data.data(),data,bytes);
        u.frame=0;
        return;
    }
#endif
    glBindBuffer(GL_UNIFORM_BUFFER,buffer);
    glBufferData(GL_UNIFORM_BUFFER,bytes,data,GL_STREAM_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER,0);
}
void bindRenderUniformBuffer(std::uint32_t binding,std::uint32_t buffer)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread)
    {
        sNative->thread->post([ctx=&sNative->context,binding,buffer] {
            if(binding>=4 || (buffer && ctx->uniformBuffers.find(buffer)==ctx->uniformBuffers.end()))
                throw std::runtime_error("Invalid Vulkan uniform buffer binding");
            ctx->uniformBindings[binding]=buffer;
        });
        return;
    }
    if (sNative)
    {
        auto& ctx=sNative->context;
        if(binding>=4 || (buffer && ctx.uniformBuffers.find(buffer)==ctx.uniformBuffers.end()))
            throw std::runtime_error("Invalid Vulkan uniform buffer binding");
        ctx.uniformBindings[binding]=buffer;
        return;
    }
#endif
    glBindBufferBase(GL_UNIFORM_BUFFER,binding,buffer);
}

bool renderQuerySupported(std::uint32_t target)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative) return sNative->queries->supported(target);
#endif
    return true;
}
void generateRenderQueries(int count, std::uint32_t* ids)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative) { sNative->queries->generate(count,ids); return; }
#endif
    glGenQueries(count,ids);
}
void deleteRenderQueries(int count, const std::uint32_t* ids)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread)
    {
        if (count>0 && ids) sNative->thread->postData(ids,size_t(count)*sizeof(std::uint32_t),[q=sNative->queries.get(),count](const void* copy) { q->destroy(count,static_cast<const std::uint32_t*>(copy)); });
        return;
    }
    if (sNative) { sNative->queries->destroy(count,ids); return; }
#endif
    glDeleteQueries(count,ids);
}
void beginRenderQuery(std::uint32_t target, std::uint32_t id)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread)
    {
        { std::lock_guard<std::mutex> lock(sNative->queryMutex); sNative->queryBeginPending.insert(id); }
        sNative->thread->post([n=sNative,target,id] {
            n->queries->begin(target,id);
            std::lock_guard<std::mutex> lock(n->queryMutex);
            n->queryBeginPending.erase(id);
        });
        return;
    }
    if (sNative) { sNative->queries->begin(target,id); return; }
#endif
    glBeginQuery(target,id);
}
void endRenderQuery(std::uint32_t target)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative && sNative->thread) { sNative->thread->post([q=sNative->queries.get(),target] { q->end(target); }); return; }
    if (sNative) { sNative->queries->end(target); return; }
#endif
    glEndQuery(target);
}
void getRenderQuery64(std::uint32_t id, std::uint32_t parameter, unsigned long long* value)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative)
    {
        if (parameter==GL_QUERY_RESULT_AVAILABLE)
        {
            bool pending=false;
            if (sNative->thread)
            {
                std::lock_guard<std::mutex> lock(sNative->queryMutex);
                pending=sNative->queryBeginPending.count(id)!=0;     // the finished query it still holds is last frame's
            }
            *value=!pending && sNative->queries->available(id);     // locked inside, safe next to the render thread
        }
        else if (parameter==GL_QUERY_RESULT)
        {
            // a ready result is read as it is; otherwise the frame must have been recorded first, and the read may wait for the GPU
            if (sNative->thread && !sNative->queries->available(id)) sNative->thread->sync();
            *value=sNative->queries->result(id);
        }
        else throw std::runtime_error("Invalid query result parameter");
        return;
    }
#endif
    glGetQueryObjectui64v(id,parameter,value);
}
void getRenderQuery32(std::uint32_t id, std::uint32_t parameter, std::uint32_t* value)
{
#ifdef LL_VULKAN_RUNTIME_AVAILABLE
    if (sNative)
    {
        unsigned long long wide=0; getRenderQuery64(id,parameter,&wide);
        *value=static_cast<std::uint32_t>(std::min(wide,static_cast<unsigned long long>(UINT32_MAX))); return;
    }
#endif
    glGetQueryObjectuiv(id,parameter,value);
}
