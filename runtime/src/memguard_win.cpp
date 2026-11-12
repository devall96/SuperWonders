// Memory guard (Windows), as memguard_mac.cpp: the process's private commit (PrivateUsage: the guest heap, host
// allocations and what the GPU driver commits for us; with an integrated GPU that includes its "video" memory) is
// checked every 250 ms; past NX_MEM_LIMIT_GB (default 8) the process ends at once, so a leak can never push the
// machine into swap (one reached 85 GB on the Mac). The footprint is logged every 30 s so growth shows early.
// NX_MEMGUARD_STANDALONE: only the helpers, for tests/memguard_test.cpp.
#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>

#ifndef NX_MEMGUARD_STANDALONE
#include <nx/runtime.h>

#include "kernel.h"
#include "nvn/nvn_vk.h"
#include "platform.h"
#endif

namespace nx::plat {

// Bytes of private memory committed by this process (0 if the query fails).
uint64_t memory_footprint() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
        return 0;
    return uint64_t(pmc.PrivateUsage);
}

// NX_MEM_LIMIT_GB's value in GB: a positive number, else 8.
double memory_limit_gb(const char* env) {
    double gb = env ? std::atof(env) : 0.0;
    return gb > 0 ? gb : 8.0;
}

static std::atomic<uint64_t> g_limit_bytes{8ull << 30};
void set_memory_limit_gb(double gb) {
    if (gb < 2.0) gb = 2.0;
    if (gb > 32.0) gb = 32.0;
    g_limit_bytes.store(uint64_t(gb * double(1ull << 30)), std::memory_order_relaxed);
}

#ifndef NX_MEMGUARD_STANDALONE

void start_memory_guard() {
    if (const char* env = std::getenv("NX_MEM_LIMIT_GB")) set_memory_limit_gb(memory_limit_gb(env));
    const uint64_t limit = g_limit_bytes.load(std::memory_order_relaxed);
    const double gb = double(limit) / double(1ull << 30);
    log("memory: guard on, limit %.1f GB (NX_MEM_LIMIT_GB)", gb);
    std::thread([limit, gb] {
        uint64_t ticks = 0, peak = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const uint64_t current_limit = g_limit_bytes.load(std::memory_order_relaxed);
            const double current_gb = double(current_limit) / double(1ull << 30);
            uint64_t f = memory_footprint();
            if (f > peak) peak = f;
            if (++ticks % 120 == 0) {
                const nx::nvn::GpuImageMemoryStats images = nx::nvn::gpu_image_memory_stats();
                log("memory: %.2f GB (peak %.2f GB, limit %.1f GB); guest heap %.2f GB; Vulkan images %.2f GB "
                    "(peak %.2f GB, %llu live)",
                    double(f) / double(1ull << 30), double(peak) / double(1ull << 30), current_gb,
                    double(nx::heap_size()) / double(1ull << 30), double(images.live_bytes) / double(1ull << 30),
                    double(images.peak_bytes) / double(1ull << 30), (unsigned long long)images.live_allocations);
            }
            if (f > current_limit) {
                char msg[160];
                std::snprintf(msg, sizeof msg, "memory: %.2f GB is over the %.1f GB limit (NX_MEM_LIMIT_GB): stopping",
                              double(f) / double(1ull << 30), current_gb);
                log("%s", msg);
                std::fprintf(stderr, "%s\n", msg);
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    }).detach();
}

#endif

uint64_t memguard_overlap_ns(uint64_t, uint64_t) { return 0; }

}  // namespace nx::plat
