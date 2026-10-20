// VulkanFsrUpscaler on a validated headless device: a synthetic 640x360 HDR scene (dark background, bright square moving 2 px a frame,
// constant depth, zero motion) is upscaled to 960x540 for 8 jittered frames and one reset frame, in the FP16 variant when the device
// has it and in FP32. Every frame's output is copied to a host buffer and checked: finite everywhere, the square bright in the centre,
// the background dark in the corner. Also: the failFsr2Create hook, refused inverted depth, resize, VRAM accounting, zero validation
// messages including teardown.
#include "vkfsr2.h"
#include <cmath>
#include <cstring>

static float halfToFloat(uint16_t h)
{
    const int e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), e - 25);
    return (h & 0x8000) ? -v : v;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    VkContext ctx;
    unsigned checks = 0;
    auto check = [&](bool ok, const char* label) { if (!ok) throw std::runtime_error(label); ++checks; printf("PASS %s\n", label); };
    try
    {
        check(ctx.init(GetModuleHandle(nullptr), nullptr, true) && ctx.validation, "validated GPU initialized");
        const uint32_t rw = 640, rh = 360, dw = 960, dh = 540;
        VulkanFsrUpscaler fsr;

        ctx.failFsr2Create = true;
        check(!fsr.create(ctx, dw, dh, rw, rh) && fsr.memoryBytes() == 0 && ctx.upscalerMemBytes == 0, "failFsr2Create: create fails, no memory");
        ctx.failFsr2Create = false;

        std::vector<std::pair<VkImage, VkDeviceMemory>> owned;
        auto makeImage = [&](VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
            VkImage image; VK_CHECK(vkCreateImage(ctx.device, &ci, nullptr, &image));
            VkMemoryRequirements req; vkGetImageMemoryRequirements(ctx.device, image, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size; ai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkDeviceMemory memory; VK_CHECK(vkAllocateMemory(ctx.device, &ai, nullptr, &memory));
            VK_CHECK(vkBindImageMemory(ctx.device, image, memory, 0));
            owned.push_back({image, memory});
            return Fsr2Image{image, format, w, h, VK_IMAGE_LAYOUT_UNDEFINED};
        };
        auto makeBuffer = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = usage;
            VK_CHECK(vkCreateBuffer(ctx.device, &bi, nullptr, &buffer));
            VkMemoryRequirements req; vkGetBufferMemoryRequirements(ctx.device, buffer, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkAllocateMemory(ctx.device, &ai, nullptr, &memory));
            VK_CHECK(vkBindBufferMemory(ctx.device, buffer, memory, 0));
            void* p; VK_CHECK(vkMapMemory(ctx.device, memory, 0, VK_WHOLE_SIZE, 0, &p));
            return p;
        };

        Fsr2Inputs in;
        in.color = makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        in.depth = makeImage(VK_FORMAT_D32_SFLOAT, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        in.motion = makeImage(VK_FORMAT_R16G16_SFLOAT, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Fsr2Image out = makeImage(VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkBuffer staging, readback; VkDeviceMemory stagingMemory, readbackMemory;
        float* scene = (float*) makeBuffer(VkDeviceSize(rw) * rh * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, stagingMemory);
        const uint16_t* result = (const uint16_t*) makeBuffer(VkDeviceSize(dw) * dh * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, readbackMemory);

        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = ctx.queueFamily;
        VkCommandPool pool; VK_CHECK(vkCreateCommandPool(ctx.device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer cmd; VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cai, &cmd));

        auto barrier = [&](Fsr2Image& image, VkImageLayout to, VkAccessFlags dstAccess, VkPipelineStageFlags dstStage) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = dstAccess;
            b.oldLayout = image.layout; b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image.image;
            b.subresourceRange = {VkImageAspectFlags(image.format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
            image.layout = to;
        };
        bool inputsCleared = false;
        // One frame: new scene upload, FSR dispatch, output copied to the host. Returns the dispatch result.
        auto frame = [&](int index, bool reset) {
            const int phases = ffxFsr3UpscalerGetJitterPhaseCount(rw, dw);
            ffxFsr3UpscalerGetJitterOffset(&in.jitterX, &in.jitterY, index % phases, phases);
            in.reset = reset;
            in.frameTimeMs = 16.7f;
            const uint32_t x0 = rw / 2 - 40 + 2 * index, y0 = rh / 2 - 40 + 2 * index;
            for (uint32_t y = 0; y < rh; ++y)
                for (uint32_t x = 0; x < rw; ++x)
                {
                    const float v = (x >= x0 && x < x0 + 80 && y >= y0 && y < y0 + 80) ? 1.0f : 0.05f;
                    float* p = scene + (size_t(y) * rw + x) * 4;
                    p[0] = p[1] = p[2] = v; p[3] = 1.0f;
                }
            VK_CHECK(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            if (!inputsCleared)
            {
                inputsCleared = true;
                barrier(in.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                barrier(in.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkClearDepthStencilValue depth{0.5f, 0};
                const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                vkCmdClearDepthStencilImage(cmd, in.depth.image, in.depth.layout, &depth, 1, &depthRange);
                const VkClearColorValue zero{};
                const VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, in.motion.image, in.motion.layout, &zero, 1, &colorRange);
            }
            // the inputs stay in their transfer layout: the wrapper must make the transfer writes visible to its compute passes
            barrier(in.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {rw, rh, 1};
            vkCmdCopyBufferToImage(cmd, staging, in.color.image, in.color.layout, 1, &copy);

            const bool ok = fsr.dispatch(cmd, in, out);
            in.color.layout = in.depth.layout = in.motion.layout = out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            barrier(out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            copy.imageExtent = {dw, dh, 1};
            vkCmdCopyImageToBuffer(cmd, out.image, out.layout, readback, 1, &copy);
            VK_CHECK(vkEndCommandBuffer(cmd));
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
            VK_CHECK(vkQueueWaitIdle(ctx.queue));
            return ok;
        };
        auto luma = [&](uint32_t x, uint32_t y) { return halfToFloat(result[(size_t(y) * dw + x) * 4 + 1]); };
        auto outputLooksRight = [&]() {
            for (size_t i = 0; i < size_t(dw) * dh * 4; ++i)
                if (!std::isfinite(halfToFloat(result[i]))) return false;
            const float centre = luma(dw / 2, dh / 2), corner = luma(8, 8);
            printf("  centre %.3f corner %.3f\n", centre, corner);
            return centre > 0.5f && centre < 2.0f && corner >= 0.0f && corner < 0.2f;
        };

        const bool deviceFp16 = ctx.fp16Math;
        for (int fp16 = deviceFp16 ? 1 : 0; fp16 >= 0; --fp16)
        {
            ctx.fp16Math = fp16 != 0;       // the wrapper picks the FP16 shaders from this
            printf("-- %s\n", fp16 ? "FP16" : "FP32");
            check(fsr.create(ctx, dw, dh, rw, rh), "upscaler created");
            check(fsr.memoryBytes() > 0 && ctx.upscalerMemBytes == fsr.memoryBytes(), "upscaler VRAM counted in upscalerMemBytes");
            printf("  upscaler VRAM %.1f MB\n", fsr.memoryBytes() / 1048576.0);
            bool all = true;
            for (int i = 0; i < 8; ++i) all = frame(i, i == 0) && all;
            check(all, "8 jittered dispatches recorded");
            check(outputLooksRight(), "display-size output: finite, square in the centre, background in the corner");
            check(frame(8, true), "reset dispatch recorded");
            check(outputLooksRight(), "reset frame output: finite, square in the centre, background in the corner");
            Fsr2Inputs inverted = in;
            inverted.depthInverted = true;
            check(!fsr.dispatch(cmd, inverted, out), "inverted depth is refused");
            check(fsr.resize(ctx, dw, dh, 480, 270) && fsr.memoryBytes() > 0 && ctx.upscalerMemBytes == fsr.memoryBytes(), "resize recreates the context");
            fsr.destroy();
            check(fsr.memoryBytes() == 0 && ctx.upscalerMemBytes == 0, "destroy releases the counted VRAM");
        }
        ctx.fp16Math = deviceFp16;

        vkDestroyCommandPool(ctx.device, pool, nullptr);
        vkDestroyBuffer(ctx.device, staging, nullptr); vkFreeMemory(ctx.device, stagingMemory, nullptr);
        vkDestroyBuffer(ctx.device, readback, nullptr); vkFreeMemory(ctx.device, readbackMemory, nullptr);
        for (auto& [image, memory] : owned) { vkDestroyImage(ctx.device, image, nullptr); vkFreeMemory(ctx.device, memory, nullptr); }
        ctx.shutdown();
        check(ctx.validationErrors == 0 && ctx.validationWarnings == 0, "zero validation messages including teardown");
        printf("FSR2_WRAPPER PASS (%u checks)\n", checks);
        return 0;
    }
    catch (const std::exception& e)
    {
        printf("FAIL: %s (validation %u errors, %u warnings)\n", e.what(), ctx.validationErrors, ctx.validationWarnings);
        return 1;
    }
}
