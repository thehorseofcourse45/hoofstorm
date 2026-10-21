// VulkanFsrUpscaler on a validated headless device: a synthetic 640x360 HDR scene (dark background, bright square moving 2 px a frame,
// constant depth, zero motion) is upscaled to 960x540 for 8 jittered frames and one reset frame, in the FP16 variant when the device
// has it and in FP32. Every frame's output is copied to a host buffer and checked: finite everywhere, the square bright in the centre,
// the background dark in the corner. Also: the failFsr2Create hook, refused inverted depth, resize, VRAM accounting, zero validation
// messages including teardown. Then the failure rule and the reset carry through VulkanFramebufferBackend::dispatchFsr2.
#include "vkfsr2.h"
#include "vkbackends.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
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
            const int64_t createdBytes = fsr.memoryBytes();
            check(fsr.resizesInPlace(dw, dh, 480, 270) && fsr.resize(ctx, dw, dh, 480, 270) && fsr.memoryBytes() == createdBytes
                      && ctx.upscalerMemBytes == createdBytes,
                  "a smaller render size keeps the context");
            check(!fsr.resizesInPlace(dw, dh, dw, dh) && fsr.resize(ctx, dw, dh, dw, dh) && fsr.memoryBytes() > createdBytes
                      && ctx.upscalerMemBytes == fsr.memoryBytes() && fsr.resizesInPlace(dw, dh, rw, rh),
                  "a larger render size recreates the context for the display size, so later steps stay in place");
            check(!fsr.resizesInPlace(dw + 2, dh, 480, 270) && fsr.resize(ctx, dw + 2, dh, 480, 270) && ctx.upscalerMemBytes == fsr.memoryBytes(),
                  "a new display size recreates the context");
            fsr.destroy();
            check(fsr.memoryBytes() == 0 && ctx.upscalerMemBytes == 0, "destroy releases the counted VRAM");
        }
        ctx.fp16Math = deviceFp16;

        // Failure rule through the backend entry the viewer uses (dispatchRenderBackendFsr2 forwards to
        // VulkanFramebufferBackend::dispatchFsr2; the global entry needs the native renderer, so the backend is driven here).
        {
            VulkanRenderStateBackend state;
            VulkanTextureBackend textures(&ctx);
            VulkanFramebufferBackend fb(&ctx, &state);
            constexpr U32 RW = 320, RH = 240, DW = 640, DH = 480;
            auto makeTexture = [&](U32 internal, U32 format, U32 w, U32 h) {
                U32 name; textures.generate(1, &name);
                textures.setStorageImage(name);
                textures.bind(glc::TEXTURE_2D, name);
                textures.image2D(glc::TEXTURE_2D, 0, internal, w, h, format, glc::FLOAT, nullptr);
                return name;
            };
            const U32 color = makeTexture(glc::RGBA16F, glc::RGBA, RW, RH);
            const U32 depth = makeTexture(glc::DEPTH_COMPONENT32F, glc::DEPTH_COMPONENT, RW, RH);
            const U32 motion = makeTexture(glc::RG16F, glc::RG, RW, RH);
            const U32 output = makeTexture(glc::RGBA16F, glc::RGBA, DW, DH);
            const U32 srgb = makeTexture(glc::SRGB8_ALPHA8, glc::RGBA, DW, DH);
            RenderBackendFsr2Params p;
            p.colorTex = color; p.depthTex = depth; p.motionTex = motion; p.outputTex = output;
            p.renderW = RW; p.renderH = RH; p.displayW = DW; p.displayH = DH;
            p.frameTimeMs = 16.7f;
            const float clear[4] = {0, 0, 0, 1};
            check(ctx.beginFrame(clear), "backend: frame acquired");

            // create fails three times: the rule trips on the third, and from then on nothing is created or dispatched
            ctx.failFsr2Create = true;
            const uint64_t creates = ctx.fsr2CreateCalls, dispatches = ctx.fsr2DispatchCalls;
            const bool first = !fb.dispatchFsr2(p) && !fb.fsr2Failed() && fb.fsr2LastFailed();
            const bool second = !fb.dispatchFsr2(p) && !fb.fsr2Failed();
            check(first && second, "backend: two create failures answer false, the rule has not tripped");
            check(!fb.dispatchFsr2(p) && fb.fsr2Failed() && ctx.fsr2CreateCalls == creates + 3, "backend: the third create failure trips the rule");
            ctx.failFsr2Create = false;
            const bool refused = !fb.dispatchFsr2(p) && !fb.dispatchFsr2(p);
            check(refused && ctx.fsr2CreateCalls == creates + 3 && ctx.fsr2DispatchCalls == dispatches && ctx.upscalerMemBytes == 0,
                  "backend: after the trip no create or dispatch is attempted");
            fb.releaseFsr2();
            check(!fb.fsr2Failed() && !fb.fsr2LastFailed(), "backend: release clears the failure state");
            ctx.endFrame(false);
            // one upscale per frame, as the viewer does (the SDK recycles its per-dispatch views after a few dispatches)
            auto inFrame = [&](const RenderBackendFsr2Params& params) {
                check(ctx.beginFrame(clear), "backend: frame acquired");
                const bool ok = fb.dispatchFsr2(params);
                ctx.endFrame(false);
                return ok;
            };
            check(inFrame(p) && !fb.fsr2LastFailed() && ctx.fsr2DispatchCalls == dispatches + 1, "backend: the next dispatch creates and records");

            // a reset is consumed only by a dispatch the SDK accepted
            RenderBackendFsr2Params resetRefused = p; resetRefused.reset = true; resetRefused.outputTex = srgb;
            check(!inFrame(resetRefused) && fb.fsr2LastFailed(), "backend: a refused dispatch asking for a reset counts a failure");
            check(inFrame(p) && fb.fsr2LastReset() && !fb.fsr2LastFailed(), "backend: the reset a refused dispatch asked for is applied by the next accepted one");
            check(inFrame(p) && !fb.fsr2LastReset(), "backend: the carried reset is applied once");
            RenderBackendFsr2Params resetSdk = p; resetSdk.reset = true;
            ctx.failFsr2Dispatch = true;
            check(!inFrame(resetSdk) && fb.fsr2LastFailed() && ctx.fsr2DispatchCalls == dispatches + 4, "backend: an SDK refusal asking for a reset counts a failure");
            ctx.failFsr2Dispatch = false;
            check(inFrame(p) && fb.fsr2LastReset(), "backend: the reset an SDK refusal swallowed is applied by the next accepted dispatch");
            fb.keepFsr2Reset(true);     // the render thread dropped a dispatch with its failed frame
            check(inFrame(p) && fb.fsr2LastReset(), "backend: a reset handed on from a dropped dispatch is applied");
            // a recreated swapchain resets the history: a refused dispatch does not use that up either
            ++ctx.swapchainGeneration;
            ctx.failFsr2Dispatch = true;
            check(!inFrame(p), "backend: SDK refusal after a swapchain recreation");
            ctx.failFsr2Dispatch = false;
            check(inFrame(p) && fb.fsr2LastReset(), "backend: the swapchain reset survives the refused dispatch");
            check(inFrame(p) && !fb.fsr2LastReset(), "backend: and is applied once");
            // a render size step keeps the context but not the history rendered at the other size
            RenderBackendFsr2Params smaller = p; smaller.renderW = p.renderW - 4; smaller.renderH = p.renderH - 4;
            const uint64_t stepCreates = ctx.fsr2CreateCalls;
            check(inFrame(smaller) && !fb.fsr2LastFailed() && fb.fsr2LastReset() && ctx.upscalerMemBytes > 0, "backend: a smaller render size resets the history");
            check(inFrame(smaller) && !fb.fsr2LastReset(), "backend: the same render size does not");
            check(inFrame(p) && fb.fsr2LastReset() && ctx.fsr2CreateCalls == stepCreates + 2, "backend: stepping back up within the created size resets again, in place");
            check(inFrame(p) && !fb.fsr2LastReset(), "backend: and only once");
            fb.releaseFsr2();
            check(ctx.upscalerMemBytes == 0, "backend: release frees the upscaler");
            const U32 names[] = {color, depth, motion, output, srgb};
            textures.destroy(5, names);
            VK_CHECK(vkDeviceWaitIdle(ctx.device));
        }

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
