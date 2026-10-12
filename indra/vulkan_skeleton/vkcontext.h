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

    // the "current shader". The GL interfaces have no such concept (it is global GL state, owned by
    // LLGLSLShader); a Vulkan pipeline needs it. This is a finding of the skeleton: see the report.
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;

    // per frame
    uint32_t imageIndex = 0;

    // read back buffer (test mode)
    VkBuffer readBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readMemory = VK_NULL_HANDLE;
    uint8_t* readMapped = nullptr;

    uint32_t validationErrors = 0;
    uint32_t validationWarnings = 0;

    bool init(HINSTANCE hinst, HWND hwnd, bool wantValidation);
    void shutdown();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkShaderModule loadShader(const char* path) const;

    // Frame: beginFrame acquires an image and starts recording with dynamic rendering into it (cleared).
    // endFrame optionally copies the image to readBuffer, then submits and presents.
    bool beginFrame(const float clear[4]);
    void endFrame(bool readBack);
};
