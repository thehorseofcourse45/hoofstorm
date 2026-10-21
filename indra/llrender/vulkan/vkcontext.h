// Shared Windows Vulkan runtime: device, swapchain and per-frame command recording.
// The viewer supplies LLWindow::getPlatformWindow() as HWND when backend selection is integrated.
#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <functional>
#include <chrono>
#include <map>
#include <array>
#include <string>
#include <stdexcept>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#define VK_CHECK(x)                                                                         \
    do {                                                                                    \
        VkResult r_ = (x);                                                                  \
        if (r_ != VK_SUCCESS) {                                                             \
            fprintf(stderr, "FATAL %s:%d: %s returned VkResult %d\n", __FILE__, __LINE__, #x, (int) r_); \
            throw std::runtime_error("Vulkan call failed: " #x);                                                                        \
        }                                                                                   \
    } while (0)

class VulkanTextureBackend;
class VulkanFramebufferBackend;
class VulkanQueryBackend;

inline VkPipelineStageFlags g_shaderStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;   // stages that sample textures
inline bool g_narrowBarriers = true;     // FS_VK_NARROW_BARRIERS=0 restores ALL_COMMANDS on both sides
inline uint64_t g_barrierCount = 0;     // texture layout barriers recorded, read by the VulkanPacing log

struct VkContext
{
    VkContext() = default;
    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;
    ~VkContext() { shutdown(); }
    // set up once
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::string pipelineCacheFile;
    bool pipelineCacheWritable = false;
    size_t pipelineCacheLoadedBytes = 0;
    void configurePipelineCache(const std::string& file, bool writable);
    bool savePipelineCache() noexcept;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    bool memoryBudget = false;      // VK_EXT_memory_budget enabled
    // Process usage and OS budget of the device-local heaps, in bytes; false without the extension. Safe from any thread.
    bool deviceLocalBudget(uint64_t& usage, uint64_t& budget) const;
    bool validation = false;

    // swapchain
    HWND window = nullptr;
    bool headless = false;
    bool swapchainDirty = false;
    static std::atomic<bool> sVsync; // wanted presentation mode, set from the main thread
    bool activeVsync = true;         // mode the current swapchain was created with
    uint32_t swapchainGeneration = 0;
    bool createWindowResources();
    void destroyWindowResources();
    bool recreateSwapchain();
    VkDeviceMemory headlessMemory = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    bool canReadBack = false;

    // one frame in flight (a skeleton, not a frame scheduler)
    VkCommandPool pool = VK_NULL_HANDLE;
    struct FrameSlot
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkDescriptorPool samplerPool = VK_NULL_HANDLE;   // sampler sets, reset every frame the slot is reused
        uint64_t frame = 0;                              // frame number last submitted from this slot (0: never)
        VkDeviceSize ringBase = 0;                       // start of this slot's region of the shared ring
        std::vector<std::function<void()>> release;      // run once the slot's frame has finished
    };
    static constexpr size_t kMaxFramesInFlight = 2;
    FrameSlot slots[kMaxFramesInFlight];
    size_t slotCount = 1, currentSlot = 0;               // currentSlot is the slot of the frame being recorded, or the last submitted one
    unsigned requestedFramesInFlight = 0;                // set before init; 0 reads FS_VK_FRAMES_IN_FLIGHT (default 1)
    void collectSlot(size_t index);
    FrameSlot& slot() { return slots[currentSlot]; }
    std::vector<VkSemaphore> renderDone;

    // the "current program". The GL interfaces have no such concept (it is global GL state, owned by LLGLSLShader);
    // a Vulkan pipeline and its descriptor sets need it. In the viewer LLShaderBackend::use() sets it.
    struct VkProgram* program = nullptr;
    uint32_t programHandle = 0;                          // backend handle of "program", for the pass timing log

    // Texture units. A program samples unit samplerUnit[binding]; the texture backend knows what is bound to each unit
    // (LLTexUnit::bind) and turns it into an image view and a sampler when a draw needs it.
    static constexpr int kTextureUnits = 32;
    VulkanTextureBackend* textures = nullptr;
    VulkanFramebufferBackend* framebuffers = nullptr;
    VulkanQueryBackend* queries = nullptr;
    uint32_t timestampBits = 0;
    float timestampPeriod = 0;
    bool hostQueryReset = false;
    bool preciseOcclusion = false, pipelineStatistics = false, depthClamp = false;
    uint64_t submissionSerial = 0;
    double lastGpuMs = 0;
    bool recording = false, acquireConsumed = false;
    VkImageLayout windowColorLayout = VK_IMAGE_LAYOUT_UNDEFINED, windowDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    void transitionWindow(bool depth, VkImageLayout layout);
    // Frame completion. Frame numbers are frameCounter values. submittedFrame is the last frame handed to vkQueueSubmit with
    // frameFence; completedFrame is the highest frame known finished on the GPU. Resource reuse tests are "usedFrame <= completedFrame".
    uint64_t submittedFrame = 0, completedFrame = 0;
    void refreshCompleted();                  // polls the fence, never waits
    void waitForFrame(uint64_t frame);        // waits until `frame` has finished
    void deferRelease(std::function<void()> release);
    bool ringFits(VkDeviceSize bytes, VkDeviceSize align) const    // uploadRingAlloc would not have to flush
    {
        return (ringOffset + align - 1) / align * align + bytes <= slots[currentSlot].ringBase + ringSize;
    }
    // deferRelease for plain vkDestroy*/vkFreeMemory calls on objects nothing else references: once the frame is finished
    // they run on the reaper thread, because vkFreeMemory can take milliseconds and stalled the render thread.
    void deferFree(std::function<void()> free) { deferRelease([this, free = std::move(free)]() mutable { reap(std::move(free)); }); }
    void reap(std::function<void()> free);
    void stopReaper();                        // runs what is queued, then joins; before the device goes away
    void collectReleasedResources();
    // Process exit without validation: vertex/index buffer memory and the device are left to process teardown. Freeing
    // thousands of buffers one vkFreeMemory at a time took about 10 s of the viewer's exit.
    bool exiting = false;
    enum class Stall : size_t { Readback, Query, UniformRing, BufferReuse, Upload, Explicit, Count };
    static constexpr size_t stallKinds = size_t(Stall::Count);
    std::array<uint64_t, stallKinds> stallCounts{};
    std::array<double, stallKinds> stallMs{};
    void recordStall(Stall reason, std::chrono::steady_clock::time_point start);
    void flushCommands(Stall reason = Stall::Explicit);

    // Work recorded while no frame is recording (texture creation and uploads between frames) goes into one batch command buffer
    // with its own staging memory. It is submitted ahead of the next submission, so it costs no GPU wait. Resources released while a
    // batch is open are held until that submission finishes.
    struct StageChunk { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; uint8_t* mapped = nullptr; VkDeviceSize size = 0, used = 0; };
    struct UploadBatch
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        std::vector<StageChunk> chunks;
        std::vector<std::function<void()>> release;
        VkDeviceSize bytes = 0;
    } batch;
    std::vector<StageChunk> freeChunks;
    std::mutex reaperMutex;
    std::condition_variable reaperWake;
    std::deque<std::function<void()>> reaperQueue;
    std::thread reaper;
    bool reaperQuit = false;
    static constexpr VkDeviceSize kBatchChunk = 8 << 20;
    VkDeviceSize batchLimit = 128 << 20;                               // between-frames batch size that triggers flushBatch
    uint64_t batchedUploads = 0;
    VkCommandBuffer batchCommands();                                   // opens the batch on first use
    uint8_t* batchStage(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceSize& offset);   // 16-byte aligned staging inside the batch
    // Ends the batch for the caller's submit; null if none. Its resources are freed once the current frame slot's fence
    // signals, or, given `release`, handed to the caller to run when its own submission has finished.
    VkCommandBuffer takeBatch(std::function<void()>* release = nullptr);
    void flushBatch();                                                 // size limit or no frame coming: submit alone, no wait
    struct FencedBatch { VkFence fence; std::function<void()> release; };
    std::vector<FencedBatch> fencedBatches;                            // flushBatch submissions not yet known finished
    void collectFencedBatches(bool wait);
    void discardBatch();                                               // shutdown: never executed

    bool samplerAnisotropy = false;
    float maxAnisotropy = 1.0f;
    float maxSamplerLodBias = 2.0f;     // device limit (2 is the minimum Vulkan guarantees)
    // FidelityFX upscaler compute needs: storage-image writes without a format whenever supported; its FP16 shader variants only
    // with fp16Math (shaderFloat16, shaderInt16 and the 16-bit buffer access features all enabled).
    bool fp16Math = false;

    // GL clip depth is -1..1, Vulkan's is 0..1. With VK_EXT_depth_clip_control the pipeline can ask for GL's range.
    bool depthClipControl = false;
    bool geometryShader = false;
    // VK_KHR_push_descriptor: a program's sampler set is pushed into the command buffer instead of allocated, written and bound.
    bool pushDescriptors = false;
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet = nullptr;
    uint32_t maxPushDescriptors = 0;
    uint64_t layoutEpoch = 0;       // changes whenever a draw binds a different pipeline layout (a push set does not survive that)

    // depth attachment (one per swapchain size; contents are cleared every frame)
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    // Fenced uniform and texture-staging storage. Every draw copies its program's uniform block shadow to the next free spot of this ring and
    // binds it with a dynamic offset. Reset every frame (one frame in flight).
    VkBuffer ringBuffer = VK_NULL_HANDLE;
    VkDeviceMemory ringMemory = VK_NULL_HANDLE;
    uint8_t* ringMapped = nullptr;
    VkDeviceSize ringTotal = 64 << 20;
    VkDeviceSize ringSize = 64 << 20;     // capacity of one slot's region (ringTotal / slotCount)
    VkDeviceSize ringOffset = 0;          // absolute offset into ringBuffer, inside the current slot's region
    VkDeviceSize uboAlignment = 256;
    VkDescriptorPool staticPool = VK_NULL_HANDLE;   // descriptor sets that live as long as their program
    uint64_t frameCounter = 0, uniformGeneration = 0;
    struct UniformBuffer { std::vector<uint8_t> data; uint64_t frame=0, generation=0; uint32_t offset=0; };
    std::map<uint32_t, UniformBuffer> uniformBuffers;
    std::array<uint32_t,4> uniformBindings{};
    uint32_t nextUniformBuffer=0, uniformBlockLimit=65536;

    // What the current frame command buffer already has bound, so a draw only records what changed. Valid while serial == cmdSerial;
    // cmdSerial changes whenever the command buffer is begun again.
    struct DrawStateCache
    {
        uint64_t serial = ~0ull;
        VkPipeline pipeline = VK_NULL_HANDLE;
        bool viewportSet = false;
        VkViewport viewport{};
        VkRect2D scissor{};
        VkBuffer vertexBuffer[16]{};
        VkDeviceSize vertexOffset[16]{};
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkIndexType indexType = VK_INDEX_TYPE_UINT16;
        VkPipelineLayout layout = VK_NULL_HANDLE;       // layout the descriptor sets below were bound with
        VkDescriptorSet sets[3]{};
        uint32_t uniformOffset = 0, blockCount = 0, blockOffsets[4]{};
    } drawCache;
    DrawStateCache& cache()
    {
        if (drawCache.serial != cmdSerial)
        {
            drawCache = DrawStateCache{};
            drawCache.serial = cmdSerial;
        }
        return drawCache;
    }
    uint64_t cmdSerial = 0;
    uint64_t statDraws = 0, statBarriers = 0, statRenderPasses = 0, statSamplerSets = 0, statProgramBinds = 0;   // VulkanPacing counters
    // draws and render passes per target, keyed {framebuffer name, width, height, color attachments}; cleared every 300 frames
    std::map<std::array<uint32_t, 4>, std::pair<uint64_t, uint64_t>> statPassDraws;

    // per frame
    uint32_t imageIndex = 0;

    // read back buffer (test mode)
    VkBuffer readBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readMemory = VK_NULL_HANDLE;
    uint8_t* readMapped = nullptr;

    double lastFenceWaitMs = 0, lastAcquireWaitMs = 0, lastRecordingMs = 0, lastPresentMs = 0;
    // beginFrame breakdown for the slow-record log: whole call, deferred releases (count), framebuffer and query begin
    double lastBeginMs = 0, lastCollectMs = 0, lastFramebufferBeginMs = 0, lastQueryBeginMs = 0;
    size_t lastCollectCount = 0;
    double pipelineCreationMs = 0;
    uint64_t bufferRenames = 0, pipelinesCreated = 0, pipelineCacheHits = 0;
    uint64_t bufferPromotions = 0, bufferDemotions = 0, bufferVramBytes = 0;   // vertex/index copies kept in VRAM
    // A draw whose pipeline is missing is skipped while a worker compiles it, instead of stalling the frame. Off unless the owner
    // enables it; syncPipelineScopes > 0 forces synchronous builds (benchmarks, tests that need every draw).
    bool asyncPipelines = false;
    int syncPipelineScopes = 0;
    uint64_t pipelineDrawsSkipped = 0;
    // GL tolerates a draw that samples the texture it renders to (undefined result, usually unused). Vulkan forbids it: by default that
    // aborts; when tolerated, the sampler reads the fallback texture instead and the draw is counted.
    bool tolerateFeedback = false;
    uint64_t feedbackSubstitutions = 0;
    uint64_t bufferAllocations = 0, bufferPoolHits = 0, bufferPoolEvictions = 0;
    // Live vkAllocateMemory bytes by owner, for the VulkanPacing log; atomic because the reaper thread frees.
    std::atomic<int64_t> textureMemBytes{0}, targetMemBytes{0}, hostBufferMemBytes{0}, stagingMemBytes{0};
    std::atomic<int64_t> upscalerMemBytes{0};   // VulkanFsrUpscaler: SDK history, internal targets and pipelines
    uint32_t failMemoryTypes = 0;           // test hook: bit i makes slab-block allocation from memory type i fail
    bool failFsr2Create = false;            // test hook: VulkanFsrUpscaler::create/resize fail as if the SDK had refused
    bool failFsr2Dispatch = false;          // test hook: VulkanFsrUpscaler::dispatch records its barriers, the SDK call fails
    uint64_t fsr2CreateCalls = 0, fsr2DispatchCalls = 0;    // VulkanFsrUpscaler::create/resize and SDK dispatch attempts
    uint64_t bufferMemoryAllocations = 0;   // vkAllocateMemory calls behind vertex/index storage
    uint64_t textureUploadAllocations = 0, textureRingUploads = 0;
    std::chrono::steady_clock::time_point recordingStarted;
    uint64_t blockingSubmissions = 0; // synchronous partial submits; excluded from ordinary frame-fence pacing
    uint32_t validationErrors = 0;
    uint32_t validationWarnings = 0;

    bool init(HINSTANCE hinst, HWND hwnd, bool wantValidation, bool wantDepthClipControl = true);
    void shutdown();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkShaderModule loadShader(const char* path) const;
    VkShaderModule createShaderModule(const uint32_t* code, size_t bytes) const;
    uint32_t uploadRingAlloc(const void* data, uint32_t bytes, Stall reason = Stall::UniformRing, VkDeviceSize alignment = 0); // copy into the ring, returns the offset

    // Frame: acquire and start command recording; the framebuffer backend opens rendering scopes lazily.
    // endFrame optionally copies the image to readBuffer, then submits and presents.
    bool beginFrame(const float clear[4]);
    void endFrame(bool readBack);
};
