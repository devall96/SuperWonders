// Memory pressure monitor (Linux desktop): private memory is checked every 250 ms and logged every 30 s. Past
// NX_MEM_LIMIT_GB (default 8), ask NVN to discard its reusable image cache and keep the game running. This is a
// cleanup threshold, not an allocation cap.
//
// What it reads: RssAnon + VmSwap from /proc/self/status, the closest match to Windows' private commit. Those are
// the kernel's counters (no page-table walk, unlike smaps_rollup, which stalled the game on Android:
// memguard_android.cpp). Resident from statm would also count the GPU driver's mappings of VRAM and shared files,
// which are not the game's to leak.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <nx/runtime.h>

#include "nvn/nvn_vk.h"
#include "platform.h"

namespace nx::plat {

namespace {

std::atomic<uint64_t> g_limit{0};
std::atomic<double> g_limit_gb{8.0};

uint64_t private_bytes() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "RssAnon:", 8) == 0 || std::strncmp(line, "VmSwap:", 7) == 0)
            kb += std::strtoull(std::strchr(line, ':') + 1, nullptr, 10);
    std::fclose(f);
    return kb << 10;
}

}  // namespace

void set_memory_limit_gb(double gb) {
    if (gb <= 0) return;
    g_limit_gb = gb;
    g_limit = uint64_t(gb * double(1ull << 30));
}

void start_memory_guard() {
    const char* e = std::getenv("NX_MEM_LIMIT_GB");
    double gb = e ? std::atof(e) : 8.0;
    if (gb <= 0) gb = 8.0;
    set_memory_limit_gb(gb);
    const char* m = std::getenv("NX_MEMGUARD");
    if (m && std::strcmp(m, "off") == 0) {
        log("memory: guard off (NX_MEMGUARD=off)");
        return;
    }
    log("memory: cleanup threshold %.1f GB (RssAnon + VmSwap); crossing it will not close the game", gb);
    std::thread([] {
        uint64_t ticks = 0, peak = 0;
        bool pressure = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            uint64_t f = private_bytes();
            peak = std::max(peak, f);
            double gb = g_limit_gb;
            if (++ticks % 120 == 0)
                log("memory: %.2f GB (peak %.2f GB, cleanup threshold %.1f GB)", double(f) / double(1ull << 30),
                    double(peak) / double(1ull << 30), gb);
            const uint64_t limit = g_limit.load(std::memory_order_relaxed);
            if (f <= limit * 9 / 10) pressure = false;
            if (f > limit && (!pressure || ticks % 120 == 0)) {
                pressure = true;
                const uint64_t released = nx::nvn::trim_reusable_image_cache();
                log("memory: %.2f GB passed the %.1f GB cleanup threshold; requested release of %.1f MiB from "
                    "the reusable Vulkan image cache; game continues (this is not a hard cap)",
                    double(f) / double(1ull << 30), gb, double(released) / (1ull << 20));
            }
        }
    }).detach();
}

uint64_t memguard_overlap_ns(uint64_t, uint64_t) { return 0; }

}  // namespace nx::plat
