#include "vkquerybackend.h"
#include "vkframebufferbackend.h"
#include "vkprogram.h"
#include <algorithm>
#include <cstdlib>

namespace
{
constexpr uint32_t TIME=0x88BF, SAMPLES=0x8914, ANY=0x8C2F, CONSERVATIVE=0x8D6A, PRIMITIVES=0x8C87;
constexpr uint32_t CAPACITY=256;
bool occlusion(uint32_t target) { return target==SAMPLES || target==ANY || target==CONSERVATIVE; }
}
VulkanQueryBackend::VulkanQueryBackend(VkContext* ctx) : mCtx(ctx), mPassTiming(getenv("FS_VK_PASS_TIMING") != nullptr)
{
    if (supported(TIME)) for (auto& timer:mFrameTimers) generate(1,&timer);
    ctx->queries=this;
}
VulkanQueryBackend::~VulkanQueryBackend()
{
    mCtx->framebuffers->endRendering();
    mCtx->queries=nullptr;
    for (auto& page:mPages)
    {
        auto device=mCtx->device; auto pool=page->pool;
        mCtx->deferRelease([device,pool]{vkDestroyQueryPool(device,pool,nullptr);});
    }
}
bool VulkanQueryBackend::supported(uint32_t target) const
{
    if (!mCtx->hostQueryReset) return false;
    if (target==TIME) return mCtx->timestampBits!=0;
    if (target==SAMPLES) return mCtx->preciseOcclusion;
    if (target==PRIMITIVES) return mCtx->pipelineStatistics;
    return (target==ANY || target==CONSERVATIVE) && mCtx->depthClamp;
}
void VulkanQueryBackend::generate(int count,uint32_t* ids)
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    if (count<0 || (count && !ids)) throw std::runtime_error("Invalid query allocation");
    for (int i=0;i<count;++i) { ids[i]=++mNext; mQueries.emplace(ids[i],Query{}); }
}
VulkanQueryBackend::Query& VulkanQueryBackend::object(uint32_t id)
{
    auto it=mQueries.find(id);
    if (it==mQueries.end()) throw std::runtime_error("Unknown Vulkan query");
    return it->second;
}
void VulkanQueryBackend::destroy(int count,const uint32_t* ids)
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    if (count<0 || (count && !ids)) throw std::runtime_error("Invalid query deletion");
    for (int i=0;i<count;++i)
    {
        for (auto& active:mActive) if (active.second==ids[i]) throw std::runtime_error("Deleting active Vulkan query");
        mQueries.erase(ids[i]);
    }
}
VulkanQueryBackend::Slot VulkanQueryBackend::allocate(VkQueryType type,uint32_t count)
{
    std::shared_ptr<Page> selected;
    // Fast path: a page with room needs no fence query and no reset (its free slots are already reset).
    for (auto& page:mPages)
        if (page->type==type && page->used+count<=CAPACITY) { selected=page; break; }
    if (!selected)
    {
        mCtx->refreshCompleted();
        for (auto& page:mPages)
        {
            if (page->type!=type) continue;
            // Only recycle a page after its GPU fence and every logical result referencing it are finished.
            if (page.use_count()==1 && page->frame<=mCtx->completedFrame)
            {
                // Host reset makes availability unambiguously false before the new submission executes.
                vkResetQueryPool(mCtx->device,page->pool,0,CAPACITY);
                page->used=0; selected=page; break;
            }
        }
    }
    if (!selected)
    {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType=type; info.queryCount=CAPACITY;
        if (type==VK_QUERY_TYPE_PIPELINE_STATISTICS)
            info.pipelineStatistics=VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                (mCtx->geometryShader ? VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT : 0);
        VkQueryPool pool;
        VK_CHECK(vkCreateQueryPool(mCtx->device,&info,nullptr,&pool));
        vkResetQueryPool(mCtx->device,pool,0,CAPACITY);
        selected=std::make_shared<Page>(Page{pool,type}); mPages.push_back(selected);
    }
    Slot slot{selected,selected->used,mCtx->submissionSerial+1};
    selected->used+=count; selected->frame=mCtx->frameCounter;
    return slot;
}
void VulkanQueryBackend::begin(uint32_t target,uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    if (!supported(target)) throw std::runtime_error("Unsupported Vulkan query target");
    if (mActive.count(target)) throw std::runtime_error("Vulkan query target already active");
    if (!mCtx->recording)
    {
        const float clear[4]={0,0,0,1};
        if (!mCtx->beginFrame(clear)) throw std::runtime_error("Query issued while window unavailable");
    }
    for (auto& active:mActive) if (active.second==id) throw std::runtime_error("Query name already active");
    if (occlusion(target))
        for (auto& active:mActive) if (occlusion(active.first)) { mCtx->framebuffers->endRendering(); break; }
    auto& query=object(id);
    query=Query{}; query.target=target;
    if (target==TIME)
    {
        startTime(query);
    }
    mActive.emplace(target,id);
    if (mCtx->framebuffers->isRendering())
    {
        if (occlusion(target)) { startOcclusion(); query.slots.push_back(mOcclusion); }
        if (target==PRIMITIVES) { startStatistics(); query.slots.push_back(mStatistics); }
    }
}
void VulkanQueryBackend::end(uint32_t target)
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    auto active=mActive.find(target);
    if (active==mActive.end()) throw std::runtime_error("Ending inactive Vulkan query");
    if (occlusion(target))
    {
        unsigned count=0; for (auto& item:mActive) count+=occlusion(item.first);
        if (count>1) mCtx->framebuffers->endRendering();
    }
    Slot* slot=occlusion(target) ? &mOcclusion : target==PRIMITIVES ? &mStatistics : nullptr;
    if (slot && slot->page)
    {
        vkCmdEndQuery(mCtx->slot().cmd,slot->page->pool,slot->index); *slot=Slot{};
    }
    auto& query=object(active->second);
    if (target==TIME)
    {
        finishTime(query);
    }
    query.ended=true; mActive.erase(active);
}
void VulkanQueryBackend::startTime(Query& query)
{
    auto first=allocate(VK_QUERY_TYPE_TIMESTAMP,2), last=first; ++last.index;
    query.slots.insert(query.slots.end(),{first,last}); query.timeOpen=true;
    vkCmdWriteTimestamp(mCtx->slot().cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,first.page->pool,first.index);
}
void VulkanQueryBackend::finishTime(Query& query)
{
    if (!query.timeOpen) return;
    if (!mCtx->recording) throw std::runtime_error("Timer segment escaped command recording");
    auto& last=query.slots.back(); last.serial=mCtx->submissionSerial+1;
    vkCmdWriteTimestamp(mCtx->slot().cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,last.page->pool,last.index);
    query.timeOpen=false;
}
void VulkanQueryBackend::beginRecording()
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    auto active=mActive.find(TIME);
    if (active!=mActive.end()) startTime(object(active->second));
}
void VulkanQueryBackend::endRecording()
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    auto active=mActive.find(TIME);
    if (active!=mActive.end()) finishTime(object(active->second));
}
void VulkanQueryBackend::startOcclusion()
{
    mOcclusion=allocate(VK_QUERY_TYPE_OCCLUSION);
    vkCmdBeginQuery(mCtx->slot().cmd,mOcclusion.page->pool,mOcclusion.index,
        mActive.count(SAMPLES) ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}
void VulkanQueryBackend::startStatistics()
{
    mStatistics=allocate(VK_QUERY_TYPE_PIPELINE_STATISTICS);
    mStatistics.geometry=mCtx->program && mCtx->program->gs;
    vkCmdBeginQuery(mCtx->slot().cmd,mStatistics.page->pool,mStatistics.index,0);
}
void VulkanQueryBackend::beginRendering()
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    bool samples=false;
    for (auto& active:mActive) samples|=occlusion(active.first);
    if (samples) startOcclusion();
    if (statisticsActive()) startStatistics();
    for (auto& active:mActive)
    {
        if (occlusion(active.first)) object(active.second).slots.push_back(mOcclusion);
        if (active.first==PRIMITIVES) object(active.second).slots.push_back(mStatistics);
    }
}
void VulkanQueryBackend::endRendering()
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    for (auto* slot:{&mOcclusion,&mStatistics}) if (slot->page)
    {
        vkCmdEndQuery(mCtx->slot().cmd,slot->page->pool,slot->index); *slot=Slot{};
    }
}
uint64_t VulkanQueryBackend::elapsed(uint64_t start,uint64_t end,uint32_t bits,double period)
{
    if (!bits || bits>64 || period<=0) throw std::runtime_error("Invalid GPU timestamp properties");
    const uint64_t mask=bits==64 ? UINT64_MAX : (uint64_t(1)<<bits)-1;
    return uint64_t(double((end-start)&mask)*period);
}
bool VulkanQueryBackend::read(Query& query,bool wait)
{
    if (!query.ended) return false;
    if (query.cached) return true;
    for (auto& slot:query.slots) if (slot.serial>mCtx->submissionSerial)
    {
        if (!wait) return false; // Reset/write commands have not reached the queue; old availability is invalid.
        mCtx->flushCommands(VkContext::Stall::Query); break;
    }
    uint64_t sum=0, firstTimestamp=0; bool first=true;
    for (auto& slot:query.slots)
    {
        uint64_t values[3]{};
        const bool statistics=slot.page->type==VK_QUERY_TYPE_PIPELINE_STATISTICS;
        const uint32_t counters=statistics && mCtx->geometryShader ? 2 : 1;
        const size_t bytes=(counters+1)*sizeof(uint64_t);
        const auto start=std::chrono::steady_clock::now();
        const VkResult status=vkGetQueryPoolResults(mCtx->device,slot.page->pool,slot.index,1,bytes,values,bytes,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | (wait ? VK_QUERY_RESULT_WAIT_BIT : 0));
        if (wait) mCtx->recordStall(VkContext::Stall::Query,start);
        if (status==VK_NOT_READY) return false;
        VK_CHECK(status);
        if (!values[counters]) return false;
        if (query.target==TIME)
        {
            if (first) firstTimestamp=values[0];
            else sum+=elapsed(firstTimestamp,values[0],mCtx->timestampBits,mCtx->timestampPeriod);
            first=!first;
        }
        else sum+=values[statistics && slot.geometry ? 1 : 0];
    }
    if (query.target==ANY || query.target==CONSERVATIVE) sum=sum!=0;
    query.value=sum; query.cached=true; query.slots.clear();
    return true;
}
bool VulkanQueryBackend::available(uint32_t id) { std::lock_guard<std::recursive_mutex> lock(mLock); return read(object(id),false); }
uint64_t VulkanQueryBackend::result(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    auto& query=object(id);
    if (!read(query,true)) throw std::runtime_error("Reading unfinished Vulkan query");
    return query.value;
}
void VulkanQueryBackend::passBegin(const std::array<uint32_t, 4>& key)
{
    if (!mPassTiming || mCtx->timestampBits == 0 || mCtx->currentSlot >= 2) return;
    std::lock_guard<std::recursive_mutex> lock(mLock);
    auto slot = allocate(VK_QUERY_TYPE_TIMESTAMP, 2);
    mPasses[mCtx->currentSlot].push_back({key, slot});
    vkCmdWriteTimestamp(mCtx->slot().cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.page->pool, slot.index);
    mPassOpen = true;
}
void VulkanQueryBackend::passEnd()
{
    if (!mPassTiming || !mPassOpen) return;
    std::lock_guard<std::recursive_mutex> lock(mLock);
    const Slot& slot = mPasses[mCtx->currentSlot].back().slot;
    vkCmdWriteTimestamp(mCtx->slot().cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.page->pool, slot.index + 1);
    mPassOpen = false;
}
void VulkanQueryBackend::beginFrame()
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    if (mPassTiming && mCtx->currentSlot < 2)
    {
        auto& passes = mPasses[mCtx->currentSlot];      // recorded two frames ago, so its fence has signalled
        for (auto& pass : passes)
        {
            uint64_t v[4]{};    // timestamp, availability, timestamp, availability
            const VkResult status = vkGetQueryPoolResults(mCtx->device, pass.slot.page->pool, pass.slot.index, 2, sizeof(v), v,
                2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (status == VK_SUCCESS && v[1] && v[3])
            {
                auto& total = passTimes[pass.key];
                total.ms += double(elapsed(v[0], v[2], mCtx->timestampBits, mCtx->timestampPeriod)) / 1e6;
                ++total.count;
            }
        }
        passes.clear();     // drops the page references so the pools can be recycled
        mPassOpen = false;
    }
    const uint32_t timer=mFrameTimers[mCtx->currentSlot];   // last used two frames ago with two slots, so it has finished
    if (!timer) return;
    auto& query=object(timer);
    if (query.ended && available(timer)) mCtx->lastGpuMs=double(query.value)/1e6;
    // Frame timing is independent of the viewer's GL_TIME_ELAPSED scopes.
    query=Query{}; query.target=TIME;
    startTime(query);
}
void VulkanQueryBackend::endFrame()
{
    std::lock_guard<std::recursive_mutex> lock(mLock);
    const uint32_t timer=mFrameTimers[mCtx->currentSlot];
    if (!timer) return;
    auto& query=object(timer); finishTime(query);
    query.ended=true;
}
