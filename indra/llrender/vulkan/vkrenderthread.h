// Command queue and worker thread for the threaded renderer (see doc/vulkan/render-thread-design.md).
// The main thread appends small records to a batch; the worker runs them in order. Records are trivially copyable
// callables stored inline, so a post costs a memcpy and no allocation. Nothing in here knows about Vulkan.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rt
{

class Thread
{
public:
    struct Stats
    {
        uint64_t posts = 0, bytes = 0, syncs = 0, flushes = 0;
        double syncMs = 0, throttleMs = 0;
    };

    explicit Thread(const char* name = "VulkanRender")
    {
        mWorker = std::thread([this, name] { run(name); });
    }
    ~Thread() { stop(); }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Run f() on the worker. F must be trivially copyable (pointers and plain values only).
    template<class F> void post(F f)
    {
        static_assert(std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F>, "records must be plain data");
        static_assert(alignof(F) <= 16, "record alignment");
        void* p = append(sizeof(F), 0, [](void* q) { (*static_cast<F*>(q))(); });
        new (p) F(f);
    }
    // Same, with `bytes` of caller memory copied into the record; f receives a pointer to the copy.
    template<class F> void postData(const void* src, size_t bytes, F f)
    {
        static_assert(std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F>, "records must be plain data");
        static_assert(alignof(F) <= 16, "record alignment");
        void* p = append(sizeof(F), bytes, [](void* q) {
            (*static_cast<F*>(q))(static_cast<const uint8_t*>(q) + align16(sizeof(F)));
        });
        new (p) F(f);
        if (bytes) memcpy(static_cast<uint8_t*>(p) + align16(sizeof(F)), src, bytes);
    }

    // Hand the current batch to the worker.
    void flush()
    {
        if (mCur.empty()) return;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            // bound the memory held by queued batches
            mCvDone.wait(lock, [&] { return mPending.size() < kMaxPending; });
            mPending.push_back(std::move(mCur));
            ++mPosted;
            mCur.clear();
            if (!mFree.empty())
            {
                mCur = std::move(mFree.back());
                mFree.pop_back();
            }
        }
        ++mStats.flushes;
        mCvWork.notify_one();
    }
    // Wait until everything posted so far has run. After this returns the worker is idle and stays idle until the next
    // post, so the caller may use the real backends directly.
    void sync()
    {
        const auto start = std::chrono::steady_clock::now();
        flush();
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mCvDone.wait(lock, [&] { return mExecuted >= mPosted; });
        }
        ++mStats.syncs;
        mStats.syncMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        rethrow();
    }
    // Frames: the main thread calls frameQueued() after posting a frame's last record, and the record itself calls frameDone().
    void frameQueued()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        ++mFramesQueued;
    }
    void frameDone()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            --mFramesQueued;
        }
        mCvDone.notify_all();
    }
    // Flush and wait until at most `limit` frames are still queued: keeps the main thread from running further ahead.
    void throttle(int limit)
    {
        const auto start = std::chrono::steady_clock::now();
        flush();
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mCvDone.wait(lock, [&] { return mFramesQueued <= limit; });
        }
        mStats.throttleMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        rethrow();
    }
    // Rethrow the first exception a record threw since the last call (the worker keeps going, as a direct call chain would).
    void rethrow()
    {
        std::exception_ptr e;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            e = mFailure;
            mFailure = nullptr;
        }
        if (e) std::rethrow_exception(e);
    }
    Stats& stats() { return mStats; }
    bool onWorker() const { return std::this_thread::get_id() == mWorker.get_id(); }

private:
    using Thunk = void (*)(void*);
    struct Header
    {
        Thunk run;
        uint32_t total;
        uint32_t pad;
    };
    static_assert(sizeof(Header) == 16, "header size");
    static constexpr size_t align16(size_t n) { return (n + 15) & ~size_t(15); }
    static constexpr size_t kBatchLimit = 1 << 20;
    static constexpr size_t kMaxPending = 16;

    void* append(size_t fixed, size_t extra, Thunk run)
    {
        if (std::this_thread::get_id() != mOwner)
        {
            fprintf(stderr, "FATAL: threaded renderer: a graphics call came from a thread other than the one that created the renderer\n");
            abort();
        }
        if (mCur.size() >= kBatchLimit) flush();
        const size_t total = sizeof(Header) + align16(fixed) + align16(extra);
        const size_t at = mCur.size();
        if (mCur.capacity() == 0) mCur.reserve(kBatchLimit + 4096);
        mCur.resize(at + total);
        auto* h = reinterpret_cast<Header*>(mCur.data() + at);
        h->run = run;
        h->total = static_cast<uint32_t>(total);
        h->pad = 0;
        ++mStats.posts;
        mStats.bytes += total;
        return mCur.data() + at + sizeof(Header);
    }

    void execute(std::vector<uint8_t>& batch)
    {
        for (size_t offset = 0; offset < batch.size();)
        {
            auto* h = reinterpret_cast<Header*>(batch.data() + offset);
            try
            {
                h->run(batch.data() + offset + sizeof(Header));
            }
            catch (const std::exception& e)
            {
                fprintf(stderr, "render thread: %s\n", e.what());
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mFailure) mFailure = std::current_exception();
            }
            catch (...)
            {
                fprintf(stderr, "render thread: unknown exception\n");
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mFailure) mFailure = std::current_exception();
            }
            offset += h->total;
        }
    }

    void run(const char* name)
    {
#ifdef _WIN32
        wchar_t wide[64] = {};
        for (int i = 0; name[i] && i < 63; ++i) wide[i] = static_cast<wchar_t>(name[i]);
        SetThreadDescription(GetCurrentThread(), wide);
#else
        (void) name;
#endif
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;)
        {
            mCvWork.wait(lock, [&] { return mQuit || !mPending.empty(); });
            if (mPending.empty())
            {
                if (mQuit) return;
                continue;
            }
            std::vector<uint8_t> batch = std::move(mPending.front());
            mPending.pop_front();
            lock.unlock();
            execute(batch);
            batch.clear();
            lock.lock();
            mFree.push_back(std::move(batch));
            ++mExecuted;
            mCvDone.notify_all();
        }
    }

    void stop()
    {
        if (!mWorker.joinable()) return;
        flush();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQuit = true;
        }
        mCvWork.notify_all();
        mWorker.join();
    }

    const std::thread::id mOwner = std::this_thread::get_id();     // the only thread that may post
    std::vector<uint8_t> mCur;                    // main thread only
    Stats mStats;                                 // written by the main thread
    std::mutex mMutex;
    std::condition_variable mCvWork, mCvDone;
    std::deque<std::vector<uint8_t>> mPending;
    std::vector<std::vector<uint8_t>> mFree;
    uint64_t mPosted = 0, mExecuted = 0;
    int mFramesQueued = 0;
    bool mQuit = false;
    std::exception_ptr mFailure;
    std::thread mWorker;
};

} // namespace rt
