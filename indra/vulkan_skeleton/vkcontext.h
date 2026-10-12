// Vulkan skeleton: device, swapchain and per-frame command recording.
// Standalone test program (not part of the viewer build). See doc/vulkan/PROJECT-NOTES.md.
#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define VK_CHECK(x)                                                                         \
    do {                                                                                    \
        VkResult r_ = (x);                                                                  \
        if (r_ != VK_SUCCESS) {                                                             \
            fprintf(stderr, "FATAL %s:%d: %s returned VkResult %d\n", __FILE__, __LINE__, #x, (int) r_); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

struct VkTexture;

struct VkContext
{
    // set up once
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    bool validation = false;

    // swapchain
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    bool canReadBack = false;

    // one frame in flight (a skeleton, not a frame scheduler)
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    std::vector<VkSemaphore> renderDone;

    // the "current program". The GL interfaces have no such concept (it is global GL state, owned by LLGLSLShader);
    // a Vulkan pipeline and its descriptor sets need it. In the viewer LLShaderBackend::use() sets it.
    struct VkProgram* program = nullptr;

    // Texture units, as the viewer's LLTexUnit::bind() would fill them. A program samples unit samplerUnit[binding].
    static constexpr int kTextureUnits = 32;
    const VkTexture* unitTextures[kTextureUnits] = {};

    // GL clip depth is -1..1, Vulkan's is 0..1. With VK_EXT_depth_clip_control the pipeline can ask for GL's range.
    bool depthClipControl = false;

    // depth attachment (one per swapchain size; contents are cleared every frame)
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    // Per-draw uniform data: every draw copies its program's uniform block shadow to the next free spot of this ring and
    // binds it with a dynamic offset. Reset every frame (one frame in flight).
    VkBuffer ringBuffer = VK_NULL_HANDLE;
    VkDeviceMemory ringMemory = VK_NULL_HANDLE;
    uint8_t* ringMapped = nullptr;
    VkDeviceSize ringSize = 1 << 20;
    VkDeviceSize ringOffset = 0;
    VkDeviceSize uboAlignment = 256;
    VkDescriptorPool staticPool = VK_NULL_HANDLE;   // descriptor sets that live as long as their program
    VkDescriptorPool framePool = VK_NULL_HANDLE;    // sampler sets, reset every frame
    uint64_t frameCounter = 0;

    // per frame
    uint32_t imageIndex = 0;

    // read back buffer (test mode)
    VkBuffer readBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readMemory = VK_NULL_HANDLE;
    uint8_t* readMapped = nullptr;

    uint32_t validationErrors = 0;
    uint32_t validationWarnings = 0;

    bool init(HINSTANCE hinst, HWND hwnd, bool wantValidation, bool wantDepthClipControl = true);
    void shutdown();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkShaderModule loadShader(const char* path) const;
    VkShaderModule createShaderModule(const uint32_t* code, size_t bytes) const;
    uint32_t uploadRingAlloc(const void* data, uint32_t bytes); // copy into the ring, returns the offset

    // Frame: beginFrame acquires an image and starts recording with dynamic rendering into it (cleared).
    // endFrame optionally copies the image to readBuffer, then submits and presents.
    bool beginFrame(const float clear[4]);
    void endFrame(bool readBack);
};
