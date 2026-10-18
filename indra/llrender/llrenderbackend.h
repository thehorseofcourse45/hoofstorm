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
