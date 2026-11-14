// Memory pressure monitor (macOS): the process's physical footprint is checked every 250 ms. Past NX_MEM_LIMIT_GB
// (default 8), ask NVN to discard its reusable image cache and keep the game running. This is a cleanup threshold,
// not an allocation cap; the footprint is logged every 30 s.
#include <mach/mach.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <thread>

#include <nx/runtime.h>

#include "nvn/nvn_vk.h"
#include "platform.h"

namespace nx::plat {

namespace {

uint64_t footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

}  // namespace

void start_memory_guard() {
    const char* e = std::getenv("NX_MEM_LIMIT_GB");
    double gb = e ? std::atof(e) : 8.0;
    if (gb <= 0) gb = 8.0;
    const uint64_t limit = uint64_t(gb * double(1ull << 30));
    std::thread([limit, gb] {
        uint64_t ticks = 0, peak = 0;
        bool pressure = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            uint64_t f = footprint();
            if (f > peak) peak = f;
            if (++ticks % 120 == 0)
                log("memory: %.2f GB (peak %.2f GB, cleanup threshold %.1f GB)", double(f) / double(1ull << 30),
                    double(peak) / double(1ull << 30), gb);
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
