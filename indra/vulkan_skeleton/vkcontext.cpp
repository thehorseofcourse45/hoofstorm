#include "vkcontext.h"

#include <algorithm>
#include <cstring>
#include <string>

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void* user)
{
    VkContext* ctx = (VkContext*) user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        ++ctx->validationErrors;
        fprintf(stderr, "VALIDATION ERROR: %s\n", data->pMessage);
    }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        ++ctx->validationWarnings;
        fprintf(stderr, "validation warning: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

uint32_t VkContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    fprintf(stderr, "FATAL: no suitable memory type\n");
    abort();
}

VkShaderModule VkContext::createShaderModule(const uint32_t* code, size_t bytes) const
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &module));
    return module;
}

uint32_t VkContext::uploadRingAlloc(const void* data, uint32_t bytes)
{
    const VkDeviceSize at = (ringOffset + uboAlignment - 1) / uboAlignment * uboAlignment;
    if (at + bytes > ringSize)
    {
        fprintf(stderr, "FATAL: uniform ring buffer full\n");
        abort();
    }
    memcpy(ringMapped + at, data, bytes);
    ringOffset = at + bytes;
    return (uint32_t) at;
}

VkShaderModule VkContext::loadShader(const char* path) const
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "FATAL: cannot open shader %s\n", path);
        abort();
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> code((size + 3) / 4);
    fread(code.data(), 1, size, f);
    fclose(f);

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = code.data();
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &module));
    return module;
}

bool VkContext::init(HINSTANCE hinst, HWND hwnd, bool wantValidation, bool wantDepthClipControl)
{
    // ---- instance
    std::vector<const char*> layers;
    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

    if (wantValidation)
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props)
        {
            if (strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                validation = true;
            }
        }
        if (!validation)
        {
            fprintf(stderr, "warning: validation layer not found (set VK_LAYER_PATH to the SDK Bin folder); running without it\n");
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vulkan_skeleton";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = (uint32_t) layers.size();
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = (uint32_t) extensions.size();
    ici.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    if (validation)
    {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debug_callback;
        mci.pUserData = this;
        VK_CHECK(create(instance, &mci, nullptr, &messenger));
    }

    // ---- surface
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = hinst;
    sci.hwnd = hwnd;
    VK_CHECK(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface));

    // ---- physical device: needs Vulkan 1.3 dynamic rendering, a graphics+present queue and swapchain support
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (VkPhysicalDevice d : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(d, &f2);
        if (!f13.dynamicRendering)
        {
            continue;
        }

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qprops.data());
        for (uint32_t q = 0; q < qn; ++q)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, q, surface, &present);
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                phys = d;
                queueFamily = q;
                break;
            }
        }
        if (phys)
        {
            fprintf(stdout, "GPU: %s (Vulkan %u.%u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                    VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
            break;
        }
    }
    if (!phys)
    {
        fprintf(stderr, "FATAL: no Vulkan 1.3 GPU with dynamic rendering and presentation support\n");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    // ---- logical device
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enable13.dynamicRendering = VK_TRUE;

    // VK_EXT_depth_clip_control: ask whether the device can use GL's -1..1 clip depth range
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, exts.data());
        bool listed = false;
        for (auto& e : exts)
        {
            listed |= strcmp(e.extensionName, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME) == 0;
        }
        VkPhysicalDeviceDepthClipControlFeaturesEXT dcf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &dcf;
        vkGetPhysicalDeviceFeatures2(phys, &f2);
        const bool supported = listed && dcf.depthClipControl;
        depthClipControl = wantDepthClipControl && supported;
        fprintf(stdout, "VK_EXT_depth_clip_control: %s, %s\n", supported ? "supported" : "NOT supported",
                depthClipControl ? "enabled" : "not used");
    }
    VkPhysicalDeviceDepthClipControlFeaturesEXT enableClip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    enableClip.depthClipControl = VK_TRUE;
    if (depthClipControl)
    {
        enable13.pNext = &enableClip;
    }

    std::vector<const char*> deviceExt = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (depthClipControl)
    {
        deviceExt.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &enable13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t) deviceExt.size();
    dci.ppEnabledExtensionNames = deviceExt.data();
    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    // ---- swapchain
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    extent = caps.currentExtent;

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = f;
        }
    }
    swapFormat = chosen.format;

    canReadBack = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface;
    sc.minImageCount = std::min(caps.minImageCount + 1, caps.maxImageCount ? caps.maxImageCount : 0xFFFFFFFFu);
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (canReadBack ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device, &sc, nullptr, &swapchain));

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    images.resize(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
    views.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &views[i]));
    }

    // ---- commands and sync
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &frameFence));
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &imageAvailable));
    renderDone.resize(imageCount);
    for (auto& s : renderDone)
    {
        VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &s));
    }

    // ---- read back buffer
    if (canReadBack)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = (VkDeviceSize) extent.width * extent.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &readBuffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, readBuffer, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &readMemory));
        VK_CHECK(vkBindBufferMemory(device, readBuffer, readMemory, 0));
        VK_CHECK(vkMapMemory(device, readMemory, 0, VK_WHOLE_SIZE, 0, (void**) &readMapped));
    }

    // ---- depth attachment
    {
        VkImageCreateInfo dci2{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        dci2.imageType = VK_IMAGE_TYPE_2D;
        dci2.format = depthFormat;
        dci2.extent = {extent.width, extent.height, 1};
        dci2.mipLevels = 1;
        dci2.arrayLayers = 1;
        dci2.samples = VK_SAMPLE_COUNT_1_BIT;
        dci2.tiling = VK_IMAGE_TILING_OPTIMAL;
        dci2.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        dci2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &dci2, nullptr, &depthImage));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, depthImage, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &depthMemory));
        VK_CHECK(vkBindImageMemory(device, depthImage, depthMemory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depthImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = depthFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &depthView));
    }

    // ---- uniform ring buffer and descriptor pools
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys, &props);
        uboAlignment = props.limits.minUniformBufferOffsetAlignment;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = ringSize;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &ringBuffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, ringBuffer, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &ringMemory));
        VK_CHECK(vkBindBufferMemory(device, ringBuffer, ringMemory, 0));
        VK_CHECK(vkMapMemory(device, ringMemory, 0, VK_WHOLE_SIZE, 0, (void**) &ringMapped));

        VkDescriptorPoolSize staticSizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 64;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = staticSizes;
        VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &staticPool));

        VkDescriptorPoolSize frameSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
        dpi.maxSets = 1024;
        dpi.pPoolSizes = frameSizes;
        VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &framePool));
    }
    return true;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                          VkPipelineStageFlags dstStage, VkAccessFlags dstAccess,
                          VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool VkContext::beginFrame(const float clear[4])
{
    VK_CHECK(vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX));
    VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return false; // the skeleton does not support resizing
    }
    VK_CHECK(r);
    VK_CHECK(vkResetFences(device, 1, &frameFence));
    ++frameCounter;
    ringOffset = 0;
    VK_CHECK(vkResetDescriptorPool(device, framePool, 0));

    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    image_barrier(cmd, images[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    image_barrier(cmd, depthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = views[imageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{clear[0], clear[1], clear[2], clear[3]}};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    return true;
}

void VkContext::endFrame(bool readBack)
{
    vkCmdEndRendering(cmd);

    if (readBack && canReadBack)
    {
        image_barrier(cmd, images[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuffer, 1, &region);
        image_barrier(cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    else
    {
        image_barrier(cmd, images[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone[imageIndex];
    VK_CHECK(vkQueueSubmit(queue, 1, &si, frameFence));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult r = vkQueuePresentKHR(queue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR)
    {
        VK_CHECK(r);
    }
}

void VkContext::shutdown()
{
    if (!device)
    {
        return;
    }
    vkDeviceWaitIdle(device);
    vkDestroyDescriptorPool(device, framePool, nullptr);
    vkDestroyDescriptorPool(device, staticPool, nullptr);
    vkUnmapMemory(device, ringMemory);
    vkDestroyBuffer(device, ringBuffer, nullptr);
    vkFreeMemory(device, ringMemory, nullptr);
    vkDestroyImageView(device, depthView, nullptr);
    vkDestroyImage(device, depthImage, nullptr);
    vkFreeMemory(device, depthMemory, nullptr);
    if (readBuffer)
    {
        vkUnmapMemory(device, readMemory);
        vkDestroyBuffer(device, readBuffer, nullptr);
        vkFreeMemory(device, readMemory, nullptr);
    }
    for (auto s : renderDone)
    {
        vkDestroySemaphore(device, s, nullptr);
    }
    vkDestroySemaphore(device, imageAvailable, nullptr);
    vkDestroyFence(device, frameFence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    for (auto v : views)
    {
        vkDestroyImageView(device, v, nullptr);
    }
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    if (messenger)
    {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy(instance, messenger, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
}
