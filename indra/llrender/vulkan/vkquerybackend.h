// Native query results survive render-target switches and remain asynchronous until explicitly read.
#pragma once
#include "vkcontext.h"
#include <array>
#include <map>
#include <memory>
#include <mutex>

class VulkanQueryBackend
{
public:
    explicit VulkanQueryBackend(VkContext* ctx);
    ~VulkanQueryBackend();
    bool supported(uint32_t target) const;
    void generate(int count, uint32_t* ids);
    void destroy(int count, const uint32_t* ids);
    void begin(uint32_t target, uint32_t id);
    void end(uint32_t target);
    bool available(uint32_t id);
    uint64_t result(uint32_t id);
    void beginRendering();
    void endRendering();
    bool statisticsActive() const { return mActive.count(0x8C87)!=0; }
    void beginFrame();
    void endFrame();
    void beginRecording();
    void endRecording();
    // Per-render-pass GPU time, off by default (about 5 ms/frame of render-thread readback in a big crowd), FS_VK_PASS_TIMING=1
    // enables. Results arrive two frames late and accumulate in
    // passTimes until the pacing log drains them.
    void passBegin(const std::array<uint32_t, 4>& key);
    void passEnd();
    void passDraw(uint32_t program);      // main G-buffer pass only: time the stretches between shader changes
    // G-buffer stretches also count input-assembly vertices, vertex shader invocations and fragment shader invocations.
    struct ProgramTime { double ms = 0; uint64_t draws = 0, stretches = 0, vertices = 0, vsInvocations = 0, fragments = 0; };
    std::map<uint32_t, ProgramTime> programTimes;
    std::map<std::pair<uint32_t, uint32_t>, ProgramTime> screenProgramTimes;   // other full-size passes, keyed by {framebuffer, program}
    double programSetupMs = 0;            // from pass start to the first draw (clears, load ops)
    std::map<uint32_t, double> drawTimes; // FS_VK_DRAW_TIMING=<program>: ms per draw of that program, keyed by its order in the pass
    struct PassTime { double ms = 0, gapMs = 0; uint64_t count = 0; };   // gapMs: idle/transition time before the pass
    std::map<std::array<uint32_t, 4>, PassTime> passTimes;
    size_t poolCount() const { return mPages.size(); }
    static uint64_t elapsed(uint64_t start, uint64_t end, uint32_t bits, double period);
private:
    struct Page { VkQueryPool pool; VkQueryType type; uint32_t used=0; uint64_t frame=0; };
    struct Slot { std::shared_ptr<Page> page; uint32_t index; uint64_t serial=0; bool geometry=false; };
    struct Query
    {
        uint32_t target=0;
        bool ended=false, cached=false, timeOpen=false;
        uint64_t value=0;
        std::vector<Slot> slots;
    };
    void startTime(Query& query);
    void finishTime(Query& query);
    void startOcclusion();
    void startStatistics();
    Slot allocate(VkQueryType type, uint32_t count=1);
    Query& object(uint32_t id);
    bool read(Query& query, bool wait);
    VkContext* mCtx;
    mutable std::recursive_mutex mLock;     // the threaded renderer reads results on the main thread while the render thread records
    std::vector<std::shared_ptr<Page>> mPages;
    std::map<uint32_t, Query> mQueries;
    std::map<uint32_t, uint32_t> mActive;
    Slot mOcclusion{}, mStatistics{};
    struct Segment { uint32_t program; Slot slot; uint32_t draws; uint32_t ordinal = 0; int32_t stats = -1; };
    static constexpr uint32_t STATS_PER_SLOT = 1024;
    VkQueryPool mStatsPool = VK_NULL_HANDLE;    // pipeline statistics for G-buffer stretches, STATS_PER_SLOT per frame slot
    uint32_t mStatsUsed[2] = {};
    int32_t mStatsOpen = -1;
    void closeStretchStats();
    struct Pass { std::array<uint32_t, 4> key; Slot slot; std::vector<Segment> segments; };
    std::vector<Pass> mPasses[2];           // indexed by frame slot; read back when the slot is reused
    const bool mPassTiming;
    const uint32_t mDrawTimingProgram;
    uint32_t mDrawOrdinal = 0;
    bool mPassOpen = false;
    uint32_t mNext=0, mFrameTimers[2]={};
};
