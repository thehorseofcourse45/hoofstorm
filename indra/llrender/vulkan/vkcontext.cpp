#include "vkcontext.h"
#include "vkframebufferbackend.h"
#include "vkquerybackend.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
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

std::atomic<bool> VkContext::sVsync{true};

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

uint32_t VkContext::uploadRingAlloc(const void* data, uint32_t bytes, Stall reason, VkDeviceSize alignment)
{
    const VkDeviceSize align = alignment ? alignment : uboAlignment;
    const VkDeviceSize at = (ringOffset + align - 1) / align * align;
    if (at + bytes > slot().ringBase + ringSize)
    {
        if (bytes > ringSize) throw std::runtime_error("Uniform upload exceeds ring capacity");
        flushCommands(reason);
        ringOffset = slot().ringBase;
        ++uniformGeneration;
        return uploadRingAlloc(data, bytes, reason, alignment);
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
    // ---- frames in flight
    unsigned frames = requestedFramesInFlight;
    if (!frames)
    {
        const char* env = std::getenv("FS_VK_FRAMES_IN_FLIGHT");
        frames = env ? (unsigned) std::atoi(env) : 1;
    }
    slotCount = std::clamp<size_t>(frames, 1, kMaxFramesInFlight);

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
    window = hwnd;
    headless = hwnd == nullptr;
    if (!headless)
    {
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = hinst;
        sci.hwnd = hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface));
    }

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
        if (!f13.dynamicRendering || !f13.shaderDemoteToHelperInvocation)
        {
            continue;
        }

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qprops.data());
        for (uint32_t q = 0; q < qn; ++q)
        {
            VkBool32 present = headless ? VK_TRUE : VK_FALSE;
            if (!headless) VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(d, q, surface, &present));
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                phys = d;
                queueFamily = q;
                timestampBits = qprops[q].timestampValidBits;
                timestampPeriod = props.limits.timestampPeriod;
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
    enable13.shaderDemoteToHelperInvocation = VK_TRUE;      // glslang turns GLSL discard into OpDemoteToHelperInvocation

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
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &extCount, exts.data());
        const char* off = std::getenv("FS_VK_PUSH_DESCRIPTORS");     // set to 0 to use allocated sampler sets
        const bool wanted = !(off && std::string(off) == "0");
        for (const auto& e : exts)
            if (wanted && std::string(e.extensionName) == VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)
            {
                deviceExt.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
                pushDescriptors = true;
            }
    }
    // core features the viewer's shaders use: samplerCubeArray (reflection probes) needs imageCubeArray
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 supported2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported2.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(phys, &supported2);
    VkPhysicalDeviceFeatures2 enable2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enable2.features.occlusionQueryPrecise = supported2.features.occlusionQueryPrecise;
    preciseOcclusion = supported2.features.occlusionQueryPrecise != VK_FALSE;
    enable2.features.pipelineStatisticsQuery = supported2.features.pipelineStatisticsQuery;
    pipelineStatistics = supported2.features.pipelineStatisticsQuery != VK_FALSE;
    enable2.features.depthClamp = supported2.features.depthClamp;
    depthClamp = supported2.features.depthClamp != VK_FALSE;
    enable2.features.geometryShader = supported2.features.geometryShader;
    geometryShader = supported2.features.geometryShader != VK_FALSE;
    enable2.features.shaderClipDistance = supported2.features.shaderClipDistance;
    enable2.features.shaderCullDistance = supported2.features.shaderCullDistance;
    enable2.features.shaderTessellationAndGeometryPointSize = supported2.features.shaderTessellationAndGeometryPointSize;
    enable2.features.imageCubeArray = supported2.features.imageCubeArray;
    enable2.features.fillModeNonSolid = supported2.features.fillModeNonSolid;
    enable2.features.largePoints = supported2.features.largePoints;
    enable2.features.textureCompressionBC = supported2.features.textureCompressionBC;      // S3TC / DXT textures
    enable2.features.samplerAnisotropy = supported2.features.samplerAnisotropy;
    samplerAnisotropy = supported2.features.samplerAnisotropy != VK_FALSE;
    {
        VkPhysicalDeviceProperties dp;
        vkGetPhysicalDeviceProperties(phys, &dp);
        maxAnisotropy = dp.limits.maxSamplerAnisotropy;
    }
    if (!supported2.features.imageCubeArray)
    {
        fprintf(stderr, "warning: imageCubeArray is not supported; shaders with samplerCubeArray will not be valid\n");
    }
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    hostQueryReset = supported12.hostQueryReset != VK_FALSE;
    enable12.hostQueryReset = supported12.hostQueryReset;
    enable12.pNext = &enable13;
    enable2.pNext = &enable12;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &enable2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t) deviceExt.size();
    dci.ppEnabledExtensionNames = deviceExt.data();
    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if (pushDescriptors)
    {
        cmdPushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR) vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
        VkPhysicalDevicePushDescriptorPropertiesKHR pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &pp;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        maxPushDescriptors = pp.maxPushDescriptors;
        if (!cmdPushDescriptorSet) pushDescriptors = false;
    }
    {
        const char* nb = std::getenv("FS_VK_NARROW_BARRIERS");
        g_narrowBarriers = !(nb && std::string(nb) == "0");
    }
    if (geometryShader) g_shaderStages |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VK_CHECK(vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache));

    if (!createWindowResources()) return false;

    // ---- commands and sync
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (size_t i = 0; i < slotCount; ++i)
    {
        VK_CHECK(vkAllocateCommandBuffers(device, &cai, &slots[i].cmd));
        VK_CHECK(vkCreateFence(device, &fci, nullptr, &slots[i].fence));
        VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &slots[i].imageAvailable));
    }
    // ---- uniform ring buffer and descriptor pools
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys, &props);
        uboAlignment = props.limits.minUniformBufferOffsetAlignment;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ringSize = ringTotal / slotCount;
        for (size_t i = 0; i < slotCount; ++i) slots[i].ringBase = i * ringSize;
        bci.size = ringTotal;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &ringBuffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, ringBuffer, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &ringMemory));
        VK_CHECK(vkBindBufferMemory(device, ringBuffer, ringMemory, 0));
        VK_CHECK(vkMapMemory(device, ringMemory, 0, VK_WHOLE_SIZE, 0, (void**) &ringMapped));

        VkPhysicalDeviceProperties uniformProps{};
        vkGetPhysicalDeviceProperties(phys, &uniformProps);
        uniformBlockLimit = std::min(65536u, uniformProps.limits.maxUniformBufferRange);
        uniformBuffers[0].data.assign(uniformBlockLimit, 0);
        VkDescriptorPoolSize staticSizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16384}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;      // a program frees its set when it is deleted
        dpi.maxSets = 8192;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = staticSizes;
        VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &staticPool));

        VkDescriptorPoolSize frameSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 262144}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 262144}};
        dpi.flags = 0;
        dpi.maxSets = 65536;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = frameSizes;
        for (size_t i = 0; i < slotCount; ++i) VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &slots[i].samplerPool));
    }
    return true;
}

bool VkContext::createWindowResources()
{
    uint32_t imageCount = 1;
    if (headless)
    {
        // A real GPU image substitutes for framebuffer 0 in tests; there is no acquire or presentation.
        extent = {800, 600};
        swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
        canReadBack = true;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = swapFormat;
        image.extent = {extent.width, extent.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        images.resize(1);
        VK_CHECK(vkCreateImage(device, &image, nullptr, images.data()));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, images[0], &req);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize = req.size;
        memory.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &memory, nullptr, &headlessMemory));
        VK_CHECK(vkBindImageMemory(device, images[0], headlessMemory, 0));
    }
    else
    {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    RECT client{};
    if (!GetClientRect(window, &client) || IsIconic(window) || client.right <= client.left || client.bottom <= client.top)
        return false;
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
        extent = {std::clamp((uint32_t) (client.right-client.left), caps.minImageExtent.width, caps.maxImageExtent.width),
                  std::clamp((uint32_t) (client.bottom-client.top), caps.minImageExtent.height, caps.maxImageExtent.height)};
    if (!extent.width || !extent.height) return false;

    uint32_t fn = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr));
    if (!fn) return false;
    std::vector<VkSurfaceFormatKHR> formats(fn);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, formats.data()));
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
    sc.minImageCount = std::min(std::max<uint32_t>(caps.minImageCount + 1, (uint32_t) slotCount + 1), caps.maxImageCount ? caps.maxImageCount : 0xFFFFFFFFu);
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (canReadBack ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = (VkCompositeAlphaFlagBitsKHR) (caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always supported
    activeVsync = sVsync;
    if (!activeVsync)
    {
        uint32_t pn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, nullptr);
        std::vector<VkPresentModeKHR> modes(pn);
        if (pn) vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, modes.data());
        bool mailbox = false, immediate = false;
        for (auto m : modes) { mailbox |= m == VK_PRESENT_MODE_MAILBOX_KHR; immediate |= m == VK_PRESENT_MODE_IMMEDIATE_KHR; }
        if (mailbox) sc.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (immediate) sc.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    sc.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device, &sc, nullptr, &swapchain));

    imageCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr));
    images.resize(imageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data()));
    }
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

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderDone.resize(imageCount);
    for (auto& semaphore : renderDone) VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &semaphore));
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
        dci2.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

    ++swapchainGeneration;
    swapchainDirty = false;
    return true;
}

void VkContext::destroyWindowResources()
{
    vkDestroyImageView(device, depthView, nullptr);
    vkDestroyImage(device, depthImage, nullptr);
    vkFreeMemory(device, depthMemory, nullptr);
    depthView = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    depthMemory = VK_NULL_HANDLE;
    if (readBuffer)
    {
        if (readMapped) vkUnmapMemory(device, readMemory);
        vkDestroyBuffer(device, readBuffer, nullptr);
        vkFreeMemory(device, readMemory, nullptr);
        readBuffer = VK_NULL_HANDLE;
        readMemory = VK_NULL_HANDLE;
        readMapped = nullptr;
    }
    for (auto semaphore : renderDone) vkDestroySemaphore(device, semaphore, nullptr);
    renderDone.clear();
    for (auto view : views) vkDestroyImageView(device, view, nullptr);
    views.clear();
    if (headless && !images.empty())
    {
        vkDestroyImage(device, images[0], nullptr);
        vkFreeMemory(device, headlessMemory, nullptr);
        headlessMemory = VK_NULL_HANDLE;
    }
    else vkDestroySwapchainKHR(device, swapchain, nullptr);
    images.clear();
    swapchain = VK_NULL_HANDLE;
}

bool VkContext::recreateSwapchain()
{
    if (recording || headless) return false;
    RECT client{};
    if (!GetClientRect(window, &client) || IsIconic(window) || client.right <= client.left || client.bottom <= client.top)
        return false;
    VK_CHECK(vkDeviceWaitIdle(device));
    collectReleasedResources();
    // shortcut: recreation waits for the device; use presentation fences when asynchronous recreation is needed.
    destroyWindowResources();
    return createWindowResources();
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
    // Only the slot about to be reused must be finished; the previous frame may still be on the GPU.
    const size_t next = (frameCounter + 1) % slotCount;
    FrameSlot& ns = slots[next];
    const auto fenceStart = std::chrono::steady_clock::now();
    VK_CHECK(vkWaitForFences(device, 1, &ns.fence, VK_TRUE, UINT64_MAX));
    completedFrame = std::max(completedFrame, ns.frame);
    lastFenceWaitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-fenceStart).count();
    const auto collectStart = std::chrono::steady_clock::now();
    lastCollectCount = slots[next].release.size();
    collectFencedBatches(false);
    collectSlot(next);
    lastCollectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-collectStart).count();
    if (!headless)
    {
        RECT client{};
        if (!IsWindow(window) || !GetClientRect(window, &client)) return false;
        if (IsIconic(window) || client.right <= client.left || client.bottom <= client.top) return false;
        if (!swapchain || swapchainDirty || activeVsync != sVsync || (uint32_t) (client.right-client.left) != extent.width ||
            (uint32_t) (client.bottom-client.top) != extent.height)
            if (!recreateSwapchain()) return false;
    }
    const auto acquireStart = std::chrono::steady_clock::now();
    VkResult r = VK_SUCCESS;
    if (!headless) r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, ns.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    else imageIndex = 0;
    lastAcquireWaitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-acquireStart).count();
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        swapchainDirty = true;
        recreateSwapchain();
        return false; // retry after pumping window events; the fence has not been reset
    }
    if (r == VK_SUBOPTIMAL_KHR) swapchainDirty = true;
    else VK_CHECK(r);
    currentSlot = next;
    VK_CHECK(vkResetFences(device, 1, &ns.fence));
    ++frameCounter;
    ringOffset = slot().ringBase;
    VK_CHECK(vkResetDescriptorPool(device, slot().samplerPool, 0));

    VK_CHECK(vkResetCommandBuffer(slot().cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(slot().cmd, &bi));
    ++cmdSerial;

    recording = true;
    recordingStarted = std::chrono::steady_clock::now();
    acquireConsumed = headless;
    windowColorLayout = windowDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const auto tailStart = std::chrono::steady_clock::now();
    framebuffers->beginFrame(clear);
    const auto queriesStart = std::chrono::steady_clock::now();
    if (queries) { queries->beginFrame(); queries->beginRecording(); }
    const auto end = std::chrono::steady_clock::now();
    lastFramebufferBeginMs = std::chrono::duration<double, std::milli>(queriesStart-tailStart).count();
    lastQueryBeginMs = std::chrono::duration<double, std::milli>(end-queriesStart).count();
    lastBeginMs = std::chrono::duration<double, std::milli>(end-fenceStart).count();
    return true;
}

void VkContext::endFrame(bool readBack)
{
    lastRecordingMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-recordingStarted).count();
    lastPresentMs = 0;
    framebuffers->endRendering();
    if (queries) queries->endRecording();

    if (readBack && canReadBack)
    {
        transitionWindow(false, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(slot().cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuffer, 1, &region);
    }
    if (!headless) transitionWindow(false, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (queries) queries->endFrame();
    VK_CHECK(vkEndCommandBuffer(slot().cmd));

    recording = false;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = acquireConsumed ? 0 : 1;
    si.pWaitSemaphores = &slot().imageAvailable;
    si.pWaitDstStageMask = &waitStage;
    VkCommandBuffer buffers[2];
    si.commandBufferCount = 0;
    if (VkCommandBuffer pending = takeBatch()) buffers[si.commandBufferCount++] = pending;
    buffers[si.commandBufferCount++] = slot().cmd;
    si.pCommandBuffers = buffers;
    si.signalSemaphoreCount = headless ? 0 : 1;
    si.pSignalSemaphores = &renderDone[imageIndex];
    VK_CHECK(vkQueueSubmit(queue, 1, &si, slot().fence));
    ++submissionSerial;
    submittedFrame = slot().frame = frameCounter;

    if (headless) return;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;
    const auto presentStart = std::chrono::steady_clock::now();
    VkResult r = vkQueuePresentKHR(queue, &pi);
    lastPresentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-presentStart).count();
    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) swapchainDirty = true;
    else if (r != VK_SUCCESS)
    {
        VK_CHECK(r);
    }
}

void VkContext::transitionWindow(bool depth, VkImageLayout layout)
{
    auto& old = depth ? windowDepthLayout : windowColorLayout;
    // An acquired swapchain image is ordered by its semaphore; the depth image and the headless color image are reused across frames.
    const VkAccessFlags src = (old == VK_IMAGE_LAYOUT_UNDEFINED && !depth && !headless) ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    image_barrier(slot().cmd, depth ? depthImage : images[imageIndex], old, layout,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, src, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                  depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    old = layout;
}

void VkContext::recordStall(Stall reason, std::chrono::steady_clock::time_point start)
{
    ++stallCounts[size_t(reason)];
    stallMs[size_t(reason)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
}

void VkContext::flushCommands(Stall reason)
{
    if (!recording) return;
    const auto start = std::chrono::steady_clock::now();
    framebuffers->endRendering();
    if (queries) queries->endRecording();
    VK_CHECK(vkEndCommandBuffer(slot().cmd));
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = acquireConsumed ? 0 : 1;
    submit.pWaitSemaphores = &slot().imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    VkCommandBuffer buffers[2];
    submit.commandBufferCount = 0;
    if (VkCommandBuffer pending = takeBatch()) buffers[submit.commandBufferCount++] = pending;
    buffers[submit.commandBufferCount++] = slot().cmd;
    submit.pCommandBuffers = buffers;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    ++submissionSerial;
    VK_CHECK(vkQueueWaitIdle(queue));
    completedFrame = submittedFrame;
    ++blockingSubmissions;
    recordStall(reason, start);
    acquireConsumed = true;
    VK_CHECK(vkResetCommandBuffer(slot().cmd, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(slot().cmd, &begin));
    ++cmdSerial;
    if (queries) queries->beginRecording();
}

VkCommandBuffer VkContext::batchCommands()
{
    if (!batch.cmd)
    {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cai, &batch.cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(batch.cmd, &bi));
    }
    return batch.cmd;
}

uint8_t* VkContext::batchStage(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceSize& offset)
{
    for (auto& chunk : batch.chunks)
    {
        const VkDeviceSize at = (chunk.used + 15) & ~VkDeviceSize(15);
        if (at + bytes <= chunk.size)
        {
            chunk.used = at + bytes;
            buffer = chunk.buffer; offset = at; batch.bytes += bytes;
            return chunk.mapped + at;
        }
    }
    StageChunk chunk;
    for (size_t i = 0; i < freeChunks.size(); ++i)
        if (freeChunks[i].size >= bytes) { chunk = freeChunks[i]; freeChunks.erase(freeChunks.begin() + i); break; }
    if (!chunk.buffer)
    {
        chunk.size = std::max(bytes, kBatchChunk);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = chunk.size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &chunk.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, chunk.buffer, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &chunk.memory));
        VK_CHECK(vkBindBufferMemory(device, chunk.buffer, chunk.memory, 0));
        VK_CHECK(vkMapMemory(device, chunk.memory, 0, VK_WHOLE_SIZE, 0, (void**) &chunk.mapped));
        ++textureUploadAllocations;
    }
    chunk.used = bytes;
    batch.chunks.push_back(chunk);
    buffer = chunk.buffer; offset = 0; batch.bytes += bytes;
    return chunk.mapped;
}

VkCommandBuffer VkContext::takeBatch(std::function<void()>* releaseOut)
{
    if (!batch.cmd) return VK_NULL_HANDLE;
    VK_CHECK(vkEndCommandBuffer(batch.cmd));
    auto taken = std::move(batch);
    batch = UploadBatch{};
    const VkCommandBuffer cmd = taken.cmd;
    // Everything the batch owns is freed once the carrying submission has finished; its frame slot is reused only after that.
    std::function<void()> release = [this, taken]() mutable {
        for (auto& release : taken.release) release();
        vkFreeCommandBuffers(device, pool, 1, &taken.cmd);
        for (auto& chunk : taken.chunks)
        {
            chunk.used = 0;
            if (chunk.size == kBatchChunk && freeChunks.size() < 4) { freeChunks.push_back(chunk); continue; }
            reap([device = device, chunk] {
                vkUnmapMemory(device, chunk.memory);
                vkDestroyBuffer(device, chunk.buffer, nullptr);
                vkFreeMemory(device, chunk.memory, nullptr);
            });
        }
    };
    if (releaseOut) *releaseOut = std::move(release);
    else slot().release.push_back(std::move(release));
    return cmd;
}

void VkContext::flushBatch()
{
    collectFencedBatches(false);
    if (recording || !batch.cmd) return;
    // Its own fence, so the staging memory is freed when this submission finishes, without waiting for it (that wait was a
    // 30 ms stall during loading) and without depending on a frame that may not come while minimized.
    FencedBatch fenced{};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fenced.fence));
    VkCommandBuffer cmd = takeBatch(&fenced.release);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fenced.fence));
    ++submissionSerial;
    fencedBatches.push_back(std::move(fenced));
}

void VkContext::collectFencedBatches(bool wait)
{
    // Submissions finish in order: stop at the first one still running.
    size_t done = 0;
    for (; done < fencedBatches.size(); ++done)
    {
        auto& fenced = fencedBatches[done];
        if (wait) VK_CHECK(vkWaitForFences(device, 1, &fenced.fence, VK_TRUE, UINT64_MAX));
        else if (vkGetFenceStatus(device, fenced.fence) != VK_SUCCESS) break;
        fenced.release();
        vkDestroyFence(device, fenced.fence, nullptr);
    }
    fencedBatches.erase(fencedBatches.begin(), fencedBatches.begin() + done);
}

void VkContext::discardBatch()
{
    if (!batch.cmd) return;
    vkEndCommandBuffer(batch.cmd);
    vkFreeCommandBuffers(device, pool, 1, &batch.cmd);
    for (auto& release : batch.release) release();
    for (auto& chunk : batch.chunks) freeChunks.push_back(chunk);
    batch = UploadBatch{};
}

void VkContext::refreshCompleted()
{
    for (;;)
    {
        FrameSlot* oldest = nullptr;
        for (size_t i = 0; i < slotCount; ++i)
            if (slots[i].frame > completedFrame && (!oldest || slots[i].frame < oldest->frame)) oldest = &slots[i];
        if (!oldest || vkGetFenceStatus(device, oldest->fence) != VK_SUCCESS) return;
        completedFrame = oldest->frame;   // the queue is in order, so earlier frames are finished too
    }
}

void VkContext::waitForFrame(uint64_t frame)
{
    while (frame > completedFrame)
    {
        FrameSlot* target = nullptr;
        for (size_t i = 0; i < slotCount; ++i)
            if (slots[i].frame >= frame && (!target || slots[i].frame < target->frame)) target = &slots[i];
        if (!target) return;   // that frame is still being recorded
        VK_CHECK(vkWaitForFences(device, 1, &target->fence, VK_TRUE, UINT64_MAX));
        completedFrame = std::max(completedFrame, target->frame);
    }
}

void VkContext::deferRelease(std::function<void()> release)
{
    if (batch.cmd) { batch.release.push_back(std::move(release)); return; }
    if (!recording)
    {
        // A flushed batch still on the GPU may use it, and was submitted after the last frame: free it after that batch.
        collectFencedBatches(false);
        if (!fencedBatches.empty())
        {
            auto& last = fencedBatches.back().release;
            last = [first = std::move(last), then = std::move(release)]() mutable { first(); then(); };
            return;
        }
    }
    refreshCompleted();
    if (!recording && completedFrame >= submittedFrame)
        release();
    else slot().release.push_back(std::move(release));
}

void VkContext::reap(std::function<void()> free)
{
    {
        std::lock_guard<std::mutex> lock(reaperMutex);
        if (!reaper.joinable())
        {
            reaperQuit = false;
            reaper = std::thread([this] {
                SetThreadDescription(GetCurrentThread(), L"VulkanReaper");
                std::unique_lock<std::mutex> lock(reaperMutex);
                for (;;)
                {
                    reaperWake.wait(lock, [this] { return reaperQuit || !reaperQueue.empty(); });
                    if (reaperQueue.empty()) return;
                    auto next = std::move(reaperQueue.front());
                    reaperQueue.pop_front();
                    lock.unlock();
                    next();
                    lock.lock();
                }
            });
        }
        reaperQueue.push_back(std::move(free));
    }
    reaperWake.notify_one();
}

void VkContext::stopReaper()
{
    {
        std::lock_guard<std::mutex> lock(reaperMutex);
        reaperQuit = true;
    }
    reaperWake.notify_one();
    if (reaper.joinable()) reaper.join();
}

void VkContext::collectSlot(size_t index)
{
    auto ready = std::move(slots[index].release);
    slots[index].release.clear();
    for (auto& release : ready) release();
}

void VkContext::collectReleasedResources()
{
    collectFencedBatches(true);
    for (size_t i = 0; i < slotCount; ++i) collectSlot(i);
}

void VkContext::shutdown()
{
    if (device)
    {
        vkDeviceWaitIdle(device);
        discardBatch();
        collectReleasedResources();
        stopReaper();
        for (auto& chunk : freeChunks)
        {
            vkUnmapMemory(device, chunk.memory);
            vkDestroyBuffer(device, chunk.buffer, nullptr);
            vkFreeMemory(device, chunk.memory, nullptr);
        }
        freeChunks.clear();
        for (size_t i = 0; i < slotCount; ++i) vkDestroyDescriptorPool(device, slots[i].samplerPool, nullptr);
        vkDestroyDescriptorPool(device, staticPool, nullptr);
        if (ringMapped) vkUnmapMemory(device, ringMemory);
        vkDestroyBuffer(device, ringBuffer, nullptr);
        vkFreeMemory(device, ringMemory, nullptr);
        destroyWindowResources();
        for (size_t i = 0; i < slotCount; ++i)
        {
            vkDestroySemaphore(device, slots[i].imageAvailable, nullptr);
            vkDestroyFence(device, slots[i].fence, nullptr);
        }
        vkDestroyCommandPool(device, pool, nullptr);
        savePipelineCache();
        vkDestroyPipelineCache(device, pipelineCache, nullptr);
        pipelineCache = VK_NULL_HANDLE;
        if (!exiting) vkDestroyDevice(device, nullptr);     // exiting: it still owns the buffers left to process teardown
        device = VK_NULL_HANDLE;
    }
    if (surface) { vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
    if (messenger)
    {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy(instance, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
    }
    if (instance && !exiting) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
    recording = false;
}
