#pragma once
#include <cstdlib>
struct LLRenderStateBackend {};
struct LLShaderBackend {};
struct LLTextureBackend {};
struct LLFramebufferBackend {};
struct LLVertexBufferBackend {};
extern int allocations[5];
LLRenderStateBackend* createGLRenderStateBackend();
LLShaderBackend* createGLShaderBackend();
LLTextureBackend* createGLTextureBackend();
LLFramebufferBackend* createGLFramebufferBackend();
LLVertexBufferBackend* createGLVertexBufferBackend();
LLRenderStateBackend& getRenderStateBackend();
LLShaderBackend& getShaderBackend();
LLTextureBackend& getTextureBackend();
LLFramebufferBackend& getFramebufferBackend();
LLVertexBufferBackend* createViewerVertexBufferBackend();

inline LLRenderStateBackend* activeVulkanRenderStateBackend() { return nullptr; }
inline LLShaderBackend* activeVulkanShaderBackend() { return nullptr; }
inline LLTextureBackend* activeVulkanTextureBackend() { return nullptr; }
inline LLFramebufferBackend* activeVulkanFramebufferBackend() { return nullptr; }
inline LLVertexBufferBackend* createActiveVulkanVertexBufferBackend() { return nullptr; }
