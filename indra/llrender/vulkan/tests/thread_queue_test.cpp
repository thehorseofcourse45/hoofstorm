// Standalone test of rt::Thread (vkrenderthread.h): ordering, data records, batches larger than the batch limit, sync,
// frame throttle, and exception forwarding. No Vulkan. Build:
//   cl /nologo /EHsc /O2 /std:c++17 tests\thread_queue_test.cpp /Fe:thread_queue_test.exe
#include "../vkrenderthread.h"

#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <stdexcept>

static int sFailures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++sFailures; } \
    } while (0)

int main()
{
    // 1. records run in order on a different thread
    {
        rt::Thread t;
        static std::vector<int> order;
        static std::thread::id worker;
        order.clear();
        for (int i = 0; i < 1000; ++i)
        {
            t.post([i] { order.push_back(i); });
        }
        t.post([] { worker = std::this_thread::get_id(); });
        t.sync();
        CHECK(order.size() == 1000);
        bool sorted = true;
        for (int i = 0; i < (int) order.size(); ++i) sorted = sorted && order[i] == i;
        CHECK(sorted);
        CHECK(worker != std::this_thread::get_id());
    }
    // 2. data is copied at post time: the source may change right away
    {
        rt::Thread t;
        static std::vector<uint32_t> seen;
        seen.clear();
        uint32_t src[64];
        for (int round = 0; round < 200; ++round)
        {
            std::iota(src, src + 64, round * 1000u);
            t.postData(src, sizeof(src), [](const void* p) {
                const uint32_t* v = static_cast<const uint32_t*>(p);
                seen.push_back(v[0] + v[63]);
            });
            memset(src, 0xFF, sizeof(src));
        }
        t.sync();
        CHECK(seen.size() == 200);
        bool ok = true;
        for (int round = 0; round < (int) seen.size(); ++round) ok = ok && seen[round] == (uint32_t)(round * 1000u * 2 + 63);
        CHECK(ok);
    }
    // 3. more data than one batch holds; every byte arrives
    {
        rt::Thread t;
        static uint64_t sum = 0;
        sum = 0;
        std::vector<uint8_t> big(300 * 1024, 1);
        for (int i = 0; i < 20; ++i)
        {
            t.postData(big.data(), big.size(), [](const void* p) {
                const uint8_t* b = static_cast<const uint8_t*>(p);
                for (size_t k = 0; k < 300 * 1024; ++k) sum += b[k];
            });
        }
        t.sync();
        CHECK(sum == 20ull * 300 * 1024);
    }
    // 4. frame throttle: main never gets more than limit+1 frames ahead
    {
        rt::Thread t;
        static std::atomic<int> executed{0};
        executed = 0;
        rt::Thread* tp = &t;
        int maxAhead = 0;
        for (int f = 0; f < 20; ++f)
        {
            t.post([tp] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++executed;
                tp->frameDone();
            });
            t.frameQueued();
            t.throttle(1);
            maxAhead = std::max(maxAhead, (f + 1) - executed.load());
        }
        t.sync();
        CHECK(executed == 20);
        CHECK(maxAhead <= 2);
    }
    // 5. an exception in a record does not stop later records and is rethrown once on the posting side
    {
        rt::Thread t;
        static int after = 0;
        after = 0;
        t.post([] { throw std::runtime_error("boom"); });
        t.post([] { ++after; });
        bool caught = false;
        try { t.sync(); } catch (const std::runtime_error& e) { caught = std::string(e.what()) == "boom"; }
        CHECK(caught);
        CHECK(after == 1);
        t.post([] { ++after; });
        t.sync();   // no second throw
        CHECK(after == 2);
    }
    // 6. destruction drains pending work
    {
        static int ran = 0;
        ran = 0;
        {
            rt::Thread t;
            for (int i = 0; i < 100; ++i) t.post([] { ++ran; });
        }
        CHECK(ran == 100);
    }
    printf(sFailures ? "FAILED: %d\n" : "OK: all checks passed\n", sFailures);
    return sFailures ? 1 : 0;
}
