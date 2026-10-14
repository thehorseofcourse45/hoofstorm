#include "vkcontext.h"
#include "vkframebufferbackend.h"
#include <cstring>

bool test_window_lifecycle(VkContext& ctx)
{
    if (ctx.headless) { printf("window lifecycle: skipped in headless mode\n"); return true; }
    bool passed = true;
    unsigned checks = 0;
    auto expect = [&](const char* name, bool ok) {
        printf("  [%s] window: %s\n", ok ? "PASS" : "FAIL", name);
        passed &= ok;
        ++checks;
    };
    auto pump = [] {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    };
    auto frame = [&](const float color[4]) {
        for (int attempt=0; attempt<30; ++attempt)
        {
            pump();
            if (!ctx.beginFrame(color)) { Sleep(10); continue; }
            ctx.framebuffers->beginRendering();
            ctx.endFrame(true);
            VK_CHECK(vkWaitForFences(ctx.device,1,&ctx.slot().fence,VK_TRUE,UINT64_MAX));
            return ctx.canReadBack && ctx.readMapped &&
                   ctx.readMapped[0] == (uint8_t) (color[2]*255) &&
                   ctx.readMapped[1] == (uint8_t) (color[1]*255) &&
                   ctx.readMapped[2] == (uint8_t) (color[0]*255);
        }
        return false;
    };
    const float blue[4] = {0,0,1,1}, green[4] = {0,1,0,1};
    expect("initial frame presents and reads back",frame(blue));
    unsigned generation=ctx.swapchainGeneration;
    const int sizes[][2]={{320,240},{1024,768},{513,377},{800,600}};
    for (const auto& size : sizes)
    {
        RECT rect{0,0,size[0],size[1]};
        AdjustWindowRect(&rect,(DWORD) GetWindowLongPtr(ctx.window,GWL_STYLE),FALSE);
        SetWindowPos(ctx.window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        pump();
        expect("resize renders the new extent",frame(green) && ctx.extent.width == (U32) size[0] && ctx.extent.height == (U32) size[1]);
        expect("size-dependent resources were recreated",ctx.swapchainGeneration > generation);
        generation=ctx.swapchainGeneration;
    }
    ShowWindow(ctx.window,SW_MINIMIZE);
    pump();
    expect("minimized window skips acquisition",!ctx.beginFrame(blue));
    expect("skipped frame leaves fence signaled",vkGetFenceStatus(ctx.device,ctx.slot().fence)==VK_SUCCESS);
    ShowWindow(ctx.window,SW_RESTORE);
    pump();
    expect("restored window resumes presentation",frame(blue));
    ctx.swapchainDirty=true;
    generation=ctx.swapchainGeneration;
    expect("dirty swapchain recreates and renders",frame(green) && ctx.swapchainGeneration > generation);
    // Record a real buffer operation, then request destruction before the recording is submitted.
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size=16;
    bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(ctx.device,&bi,nullptr,&buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(ctx.device,buffer,&requirements);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=requirements.size;
    ai.memoryTypeIndex=ctx.findMemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device,&ai,nullptr,&memory));
    VK_CHECK(vkBindBufferMemory(ctx.device,buffer,memory,0));
    bool released=false;
    if (!ctx.beginFrame(blue)) return false;
    vkCmdFillBuffer(ctx.slot().cmd,buffer,0,16,0xFFFFFFFF);
    const auto device=ctx.device;
    ctx.deferRelease([device,buffer,memory,&released] {
        vkDestroyBuffer(device,buffer,nullptr);
        vkFreeMemory(device,memory,nullptr);
        released=true;
    });
    expect("recorded resource stays alive until frame completion",!released);
    ctx.framebuffers->beginRendering();
    ctx.endFrame(false);
    expect("deletion remains queued until fence collection",!released);
    // A release is collected when its slot is reused, so with several slots it runs that many frames later.
    bool framesOk=true;
    for (size_t i=0;i<ctx.slotCount;++i) framesOk=frame(green) && framesOk;
    expect("reused frame slot collects completed resource",framesOk && released);
    {
        VkContext partial;
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance.pApplicationInfo=&application;
        VK_CHECK(vkCreateInstance(&instance,nullptr,&partial.instance));
        partial.shutdown();
        expect("partial initialization releases the instance",partial.instance==VK_NULL_HANDLE && partial.device==VK_NULL_HANDLE);
        partial.shutdown();
        expect("shutdown is safe to repeat",partial.instance==VK_NULL_HANDLE);
    }
    {
        VkContext probe;
        if (!probe.init(GetModuleHandle(nullptr),nullptr,ctx.validation)) return false;
        // Reproduce shutdown before a readback allocation has a live host mapping.
        vkUnmapMemory(probe.device,probe.readMemory);
        probe.readMapped=nullptr;
        probe.shutdown();
        expect("unmapped readback allocation cleans up without validation errors",probe.validationErrors==0 && probe.validationWarnings==0);
    }
    bool completedRelease=false;
    VK_CHECK(vkResetFences(ctx.device,1,&ctx.slot().fence));
    ctx.deferRelease([&completedRelease] { completedRelease=true; });
    expect("unsignaled fence keeps deletion queued outside recording",!completedRelease);
    VK_CHECK(vkQueueSubmit(ctx.queue,0,nullptr,ctx.slot().fence));
    framesOk=true;
    for (size_t i=0;i<ctx.slotCount;++i) framesOk=frame(green) && framesOk;
    expect("completed fence permits queued deletion",framesOk && completedRelease);
    printf("window lifecycle checks: %u, result %s\n",checks,passed?"PASS":"FAIL");
    return passed;
}
