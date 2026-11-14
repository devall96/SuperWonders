// NX_FRAMETRACE: one CSV row per frame presented to the display (frametrace.h; docs/PERF-SUITE.md lists the columns).
#include "frametrace.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "platform.h"

namespace nx::ft {

std::atomic<uint64_t> g_counter[kCount];

namespace {

uint64_t thread_cpu_ns() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime, k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime, u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 100ull;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
#endif
}

// Page faults of the process so far: minor (a page mapped in) and major (read from disk). Windows has one count.
void page_faults(uint64_t& minor, uint64_t& major) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    minor = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PageFaultCount : 0;
    major = 0;
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    minor = uint64_t(ru.ru_minflt), major = uint64_t(ru.ru_majflt);
#endif
}

struct Guest {  // the latest game-side present, written by the game's thread, read by the presenting thread
    std::atomic<uint64_t> presents{0}, gap_ns{0}, cpu_ns{0}, replay_wait_ns{0}, pace_ns{0};
    std::atomic<int> interval{1};
} g_guest;

FILE* g_file = nullptr;
std::mutex g_lock;
uint64_t g_t0 = 0;

FILE* open_file() {
    static bool tried = false;
    if (tried) return g_file;
    tried = true;
    const char* path = std::getenv("NX_FRAMETRACE");
    if (!path || !*path) return nullptr;
    g_file = plat::fopen_utf8(path, "w");
    if (!g_file) return nullptr;
    static char buf[1 << 20];
    std::setvbuf(g_file, buf, _IOFBF, sizeof buf);
    std::fprintf(g_file,
                 "frame,t_ms,frame_ms,acquire_ms,queue_present_ms,replay_ms,encode_ms,fence_ms,upload_mb,"
                 "present_cpu_ms,guest_presents,guest_gap_ms,guest_cpu_ms,guest_replay_wait_ms,guest_pace_ms,"
                 "guest_interval,pipelines,pipeline_ms,skipped,astc,astc_ms,file_reads,file_mb,file_ms,minflt,majflt,"
                 "log_line\n");
    std::atexit([] {
        std::lock_guard lk(g_lock);
        if (g_file) std::fflush(g_file);
    });
    g_t0 = plat::monotonic_ns();
    return g_file;
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* p = std::getenv("NX_FRAMETRACE");
        return p && *p;
    }();
    return on;
}

void guest_present(uint64_t replay_wait_ns, uint64_t pace_ns, int interval) {
    if (!enabled()) return;
    static thread_local uint64_t last_t = 0, last_cpu = 0;
    const uint64_t t = plat::monotonic_ns(), cpu = thread_cpu_ns();
    // Summed until the presenting thread takes them (two game presents can fall between two display presents).
    g_guest.gap_ns.store(last_t ? t - last_t : 0, std::memory_order_relaxed);
    g_guest.cpu_ns.fetch_add(last_cpu ? cpu - last_cpu : 0, std::memory_order_relaxed);
    g_guest.replay_wait_ns.fetch_add(replay_wait_ns, std::memory_order_relaxed);
    g_guest.pace_ns.fetch_add(pace_ns, std::memory_order_relaxed);
    g_guest.interval.store(interval, std::memory_order_relaxed);
    g_guest.presents.fetch_add(1, std::memory_order_relaxed);
    last_t = t, last_cpu = cpu;
}

uint64_t host_present(uint64_t acquire_ns, uint64_t queue_present_ns, uint64_t encode_total_ns, uint64_t fence_total_ns) {
    if (!enabled()) return UINT64_MAX;
    std::lock_guard lk(g_lock);
    FILE* f = open_file();
    if (!f) return UINT64_MAX;
    static uint64_t frame = 0, last_t = 0, last_cpu = 0, last_encode = 0, last_fence = 0, last_minor = 0, last_major = 0;
    static uint64_t last[kCount] = {};
    const uint64_t t = plat::monotonic_ns(), cpu = thread_cpu_ns();
    uint64_t minor = 0, major = 0;
    page_faults(minor, major);
    uint64_t now[kCount], d[kCount];
    for (int k = 0; k < kCount; k++) now[k] = g_counter[k].load(std::memory_order_relaxed), d[k] = now[k] - last[k];
    const uint64_t presents = g_guest.presents.exchange(0, std::memory_order_relaxed);
    const uint64_t gcpu = g_guest.cpu_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t gwait = g_guest.replay_wait_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t gpace = g_guest.pace_ns.exchange(0, std::memory_order_relaxed);
    auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
    std::fprintf(f,
                 "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%.3f,%.3f,%.3f,%.3f,%d,%llu,%.3f,%llu,%llu,%.3f,"
                 "%llu,%.3f,%.3f,%llu,%llu,%llu\n",
                 (unsigned long long)frame, ms(t - g_t0), last_t ? ms(t - last_t) : 0.0, ms(acquire_ns),
                 ms(queue_present_ns), ms(d[kReplayNs]), ms(encode_total_ns - last_encode),
                 ms(fence_total_ns - last_fence), double(d[kUploadBytes]) / 1e6, last_cpu ? ms(cpu - last_cpu) : 0.0,
                 (unsigned long long)presents, ms(g_guest.gap_ns.load(std::memory_order_relaxed)), ms(gcpu), ms(gwait),
                 ms(gpace), g_guest.interval.load(std::memory_order_relaxed), (unsigned long long)d[kPipelines],
                 ms(d[kPipelineNs]), (unsigned long long)d[kSkipped], (unsigned long long)d[kAstc], ms(d[kAstcNs]),
                 (unsigned long long)d[kFileReads], double(d[kFileBytes]) / 1e6, ms(d[kFileNs]),
                 (unsigned long long)(last_t ? minor - last_minor : 0), (unsigned long long)(last_t ? major - last_major : 0),
                 (unsigned long long)now[kLogLines]);
    const uint64_t row = frame;
    if (++frame % 600 == 0) std::fflush(f);
    for (int k = 0; k < kCount; k++) last[k] = now[k];
    last_t = t, last_cpu = cpu, last_encode = encode_total_ns, last_fence = fence_total_ns;
    last_minor = minor, last_major = major;
    return row;
}

void displayed(uint64_t frame, uint64_t t_ns) {
    static std::mutex lock;
    static FILE* f = nullptr;
    static bool tried = false;
    std::lock_guard lk(lock);
    if (!tried) {
        tried = true;
        f = plat::fopen_utf8(std::string(std::getenv("NX_FRAMETRACE")) + ".display.csv", "w");
        if (f) {
            static char buf[1 << 16];
            std::setvbuf(f, buf, _IOFBF, sizeof buf);
            std::fprintf(f, "frame,displayed_ms\n");
            std::atexit([] { std::lock_guard l(lock); if (f) std::fflush(f); });
        }
    }
    if (!f) return;
    std::fprintf(f, "%llu,%.3f\n", (unsigned long long)frame, double(t_ns - g_t0) / 1e6);
    static uint64_t n = 0;
    if (++n % 600 == 0) std::fflush(f);
}

}  // namespace nx::ft
