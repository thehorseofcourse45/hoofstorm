// Small fork-join pool: run fn(i) for i in [0, count) on worker threads and the calling thread, and return when every call is done.
// The caller must not touch the data fn works on until run() returns (the calling thread is parked inside it too).
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "llrand.h"

class FSParallelPool
{
public:
    static FSParallelPool& get()
    {
        static FSParallelPool pool;
        return pool;
    }

    size_t workerCount() const { return mWorkers.size(); }

    // Runs inline when the pool has no workers or there is a single task. fn must not throw (a throw is swallowed and the task counts as done).
    template<class F> void run(size_t count, F& fn)
    {
        if (count == 0) return;
        if (count == 1 || mWorkers.empty())
        {
            for (size_t i = 0; i < count; ++i) fn(i);
            return;
        }
        Job job{[](void* ctx, size_t i) { (*static_cast<F*>(ctx))(i); }, &fn, count};
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mJob = &job;
            ++mGeneration;
        }
        mCvWork.notify_all();
        work(job);
        std::unique_lock<std::mutex> lock(mMutex);
        mCvDone.wait(lock, [&] { return job.done.load() >= count && mActive == 0; });
        mJob = nullptr;
    }

private:
    struct Job
    {
        void (*call)(void*, size_t);
        void* ctx;
        size_t count;
        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
    };

    FSParallelPool()
    {
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned workers = hw >= 4 ? std::min(hw / 2 - 1, 6u) : 0;     // leave the other cores to the render thread, decoders and the OS
        for (unsigned i = 0; i < workers; ++i) mWorkers.emplace_back([this] { loop(); });
    }
    ~FSParallelPool()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQuit = true;
        }
        mCvWork.notify_all();
        for (auto& w : mWorkers) w.join();
    }

    void work(Job& job)
    {
        for (;;)
        {
            const size_t i = job.next.fetch_add(1);
            if (i >= job.count) break;
            try
            {
                job.call(job.ctx, i);
            }
            catch (...)
            {
            }
            if (job.done.fetch_add(1) + 1 == job.count)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mCvDone.notify_all();
            }
        }
    }

    void loop()
    {
        {
            // the first ll_frand() on a thread seeds that thread's generator; do it one thread at a time
            static std::mutex seed_mutex;
            std::lock_guard<std::mutex> lock(seed_mutex);
            ll_frand();
        }
        size_t seen = 0;
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;)
        {
            mCvWork.wait(lock, [&] { return mQuit || mGeneration != seen; });
            if (mQuit) return;
            seen = mGeneration;
            Job* job = mJob;
            if (!job) continue;
            ++mActive;
            lock.unlock();
            work(*job);
            lock.lock();
            --mActive;
            mCvDone.notify_all();
        }
    }

    std::vector<std::thread> mWorkers;
    std::mutex mMutex;
    std::condition_variable mCvWork, mCvDone;
    Job* mJob = nullptr;
    size_t mGeneration = 0, mActive = 0;
    bool mQuit = false;
};
