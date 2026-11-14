// Worker threads for pipeline compiles (nvn_pipeline_async.h).
#include "nvn_pipeline_async.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <thread>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// Workers wait on one queue. Leaked on purpose: detached threads may still run while statics are destroyed at exit.
struct Pool {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    unsigned threads = 0;
    Pool() {
        unsigned n = std::thread::hardware_concurrency();
        // Leave cores to the game's own threads; vkCreateGraphicsPipelines scales with threads on AMD and RADV.
        threads = n > 4 ? n - 2 : 2;
        if (threads > 12) threads = 12;
        for (unsigned k = 0; k < threads; k++)
            std::thread([this] {
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock l(lock);
                        cv.wait(l, [&] { return !queue.empty(); });
                        job = std::move(queue.front());
                        queue.pop_front();
                    }
                    job();
                }
            }).detach();
        log("nvn/vk: %u pipeline compile threads", threads);
    }
    void push(std::function<void()> job) {
        {
            std::lock_guard l(lock);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }
};

Pool& pool() {
    static Pool* p = new Pool();
    return *p;
}

}  // namespace

uint64_t g_pipelines_skipped = 0;

bool async_pipelines() {
    static const bool on = [] {
        const char* e = std::getenv("NX_ASYNC_PIPELINES");
        return !(e && e[0] == '0');
    }();
    return on;
}

std::shared_ptr<AsyncPipeline> compile_pipeline_async(std::shared_ptr<PipelineBuild> build) {
    auto a = std::make_shared<AsyncPipeline>();
    a->build = std::move(build);
    pipeline_cache();  // made on this thread before any worker uses it
    pool().push([a] {
        const uint64_t t0 = plat::monotonic_ns();
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &a->build->ci, nullptr, &p);
        std::lock_guard l(a->lock);
        a->pipeline = p, a->result = r, a->ns = plat::monotonic_ns() - t0, a->done = true;
        a->finished.store(true, std::memory_order_release);
        a->cv.notify_all();
    });
    return a;
}

void run_on_pool(std::function<void()> job) { pool().push(std::move(job)); }

void run_parallel(std::vector<std::function<void()>>& jobs, const std::function<void()>& tick) {
    if (!async_pipelines()) {  // NX_ASYNC_PIPELINES=0: one after another on this thread, as before (bisecting)
        for (auto& job : jobs) {
            job();
            if (tick) tick();
        }
        return;
    }
    struct Count {
        std::mutex lock;
        std::condition_variable cv;
        size_t left = 0;
    } count;
    count.left = jobs.size();
    for (auto& job : jobs)
        pool().push([&count, &job] {
            job();
            std::lock_guard l(count.lock);  // notified under the lock: `count` lives on the waiting stack
            if (--count.left == 0) count.cv.notify_all();
        });
    std::unique_lock l(count.lock);
    if (!tick) {
        count.cv.wait(l, [&] { return count.left == 0; });
        return;
    }
    // tick (outside the lock) every 16 ms until the jobs are done
    while (!count.cv.wait_for(l, std::chrono::milliseconds(16), [&] { return count.left == 0; })) {
        l.unlock();
        tick();
        l.lock();
    }
}

namespace {

struct Writer {
    std::mutex lock;
    std::condition_variable cv, idle;
    std::deque<std::function<void()>> queue;
    bool busy = false;
    Writer() {
        std::thread([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock l(lock);
                    cv.wait(l, [&] { return !queue.empty(); });
                    job = std::move(queue.front());
                    queue.pop_front();
                    busy = true;
                }
                job();
                std::lock_guard l(lock);
                busy = false;
                if (queue.empty()) idle.notify_all();
            }
        }).detach();
    }
};

std::atomic<Writer*> g_writer{nullptr};
Writer& writer() {
    static Writer* w = new Writer();  // leaked like the pool
    g_writer.store(w, std::memory_order_release);
    return *w;
}

}  // namespace

void background_write(std::function<void()> job) {
    if (!async_pipelines()) return job();  // NX_ASYNC_PIPELINES=0: on this thread, as before
    Writer& w = writer();
    {
        std::lock_guard l(w.lock);
        w.queue.push_back(std::move(job));
    }
    w.cv.notify_one();
}

void wait_background_writes() {
    Writer* p = g_writer.load(std::memory_order_acquire);
    if (!p) return;  // nothing was ever queued: no thread to start at shutdown
    Writer& w = *p;
    std::unique_lock l(w.lock);
    w.idle.wait(l, [&] { return w.queue.empty() && !w.busy; });
}

}  // namespace nx::nvn
