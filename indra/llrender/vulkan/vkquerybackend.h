// Native query results survive render-target switches and remain asynchronous until explicitly read.
#pragma once
#include "vkcontext.h"
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
    uint32_t mNext=0, mFrameTimers[2]={};
};
