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

#ifndef LL_LLRENDERBACKEND_H
#define LL_LLRENDERBACKEND_H
#include <string>
#include <cstdint>

// Experimental native viewer renderer; selected before any graphics objects exist.
constexpr unsigned LL_WINDOW_VULKAN = 1u;
bool activateVulkanRenderBackend(void* instance, void* window, bool validation, std::string& error);
bool isVulkanRenderBackendActive();
void setRenderBackendVsync(bool enable); // Vulkan presentation mode: FIFO when on, MAILBOX/IMMEDIATE when off
bool beginRenderBackendFrame(const float clear[4]);
bool isRenderBackendFrameActive();
void endRenderBackendFrame();
void shutdownRenderBackend(); // caller releases LLRender/LLVertexBuffer resources before closing the native window
void setRenderBackendExiting(); // process exit: skip freeing GPU buffers one by one (not with validation on)
void getRenderBackendValidationCounts(std::uint32_t& errors, std::uint32_t& warnings);
bool renderBackendObjectsCreated();
class LLGLManager;
bool initializeVulkanCapabilities(LLGLManager& manager);
void waitRenderBackendIdle();
// Real device-local memory use and OS budget in MB (VK_EXT_memory_budget); false when not Vulkan or unsupported.
bool getRenderBackendVRAMBudget(std::uint32_t& usedMB, std::uint32_t& budgetMB);
// Draws whose pipeline is still compiling are skipped unless a caller needs every draw (benchmarks): hold a scope around that work.
void pushSyncPipelines();
void popSyncPipelines();
void configureRenderPipelineCache(const std::string& file, bool writable);
void deferRenderBackendPresentation(bool defer);
std::uint32_t createRenderUniformBuffer();
void deleteRenderUniformBuffer(std::uint32_t buffer);
void uploadRenderUniformBuffer(std::uint32_t buffer, const void* data, std::size_t bytes);
void bindRenderUniformBuffer(std::uint32_t binding, std::uint32_t buffer);

// FSR 2 (FidelityFX FSR 3.1) temporal upscale, Vulkan only. Textures are GL names. color: HDR, render size. depth: D32/D16,
// 0 near 1 far. motion: RG16F/RG32F, current-to-previous motion in UV units. reactive: optional (0), R8. output: display size,
// made with setRenderBackendStorageTexture and not sRGB. Every image has one mip level and one layer (setRenderBackendStorageTexture
// gives that). jitter in render pixels; camera near/far in metres, fovY in radians (the SDK linearises depth with them).
struct RenderBackendFsr2Params
{
    unsigned colorTex = 0, depthTex = 0, motionTex = 0, reactiveTex = 0, outputTex = 0;
    unsigned renderW = 0, renderH = 0, displayW = 0, displayH = 0;
    float jitterX = 0, jitterY = 0, frameTimeMs = 16.7f, sharpness = 0;
    bool reset = false;
    float cameraNear = 0.1f, cameraFar = 1000.0f, fovY = 1.0f;
};
// Records the upscale into the current frame; afterwards every image passed is ready for sampling. False when the Vulkan backend is
// not active, no frame is recording, a size is zero or render > display, an image is unsuitable, the upscaler cannot be created,
// or after isRenderBackendFsr2Failed(). The upscaler is created on first use and recreated when a size changes. With the render
// thread on, the work is queued and true means queued: a failure found there only counts toward isRenderBackendFsr2Failed().
bool dispatchRenderBackendFsr2(const RenderBackendFsr2Params& params);
void releaseRenderBackendFsr2();     // frees the upscaler and its history, VRAM count to 0, failure state cleared
bool isRenderBackendFsr2Failed();    // three dispatches in a row failed; nothing is attempted until releaseRenderBackendFsr2()
// The last dispatch that reached the backend was recorded and accepted by the SDK. With the render thread on, read it after
// waitRenderBackendIdle() (tests).
bool lastRenderBackendFsr2Succeeded();
// Call after generating the texture name and before defining its storage: one mip level, plus storage usage where the format
// allows it (for FSR 2 images). No effect with OpenGL.
void setRenderBackendStorageTexture(unsigned texture);
// Texture LOD bias for the temporal upscaler (fsr2MipBias), on sampled 2D textures that filter through mips; render targets,
// cube maps and shadow samplers keep theirs. 0 is the plain state. Applied in order with the draws, so the viewer sets it around
// the jittered main-scene passes only (LLPipeline::beginFsr2SceneTextures / endFsr2Frame). No effect with OpenGL.
void setRenderBackendTextureLodBias(float bias);

bool renderQuerySupported(std::uint32_t target);
void generateRenderQueries(int count, std::uint32_t* ids);
void deleteRenderQueries(int count, const std::uint32_t* ids);
void beginRenderQuery(std::uint32_t target, std::uint32_t id);
void endRenderQuery(std::uint32_t target);
void getRenderQuery64(std::uint32_t id, std::uint32_t parameter, unsigned long long* value);
void getRenderQuery32(std::uint32_t id, std::uint32_t parameter, std::uint32_t* value);

class LLRenderStateBackend;
class LLShaderBackend;
class LLTextureBackend;
class LLFramebufferBackend;
class LLVertexBufferBackend;
LLRenderStateBackend* activeVulkanRenderStateBackend();
LLShaderBackend* activeVulkanShaderBackend();
LLTextureBackend* activeVulkanTextureBackend();
LLFramebufferBackend* activeVulkanFramebufferBackend();
LLVertexBufferBackend* createActiveVulkanVertexBufferBackend();
#endif
