// Module loader and dynamic linker (what rtld does on Horizon), indirect-call dispatch and HLE thunks.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "platform.h"
#include "frametrace.h"

namespace nx {

bool g_trace = false;
std::atomic<uint64_t> g_frame{0};

namespace {

std::mutex g_log_lock;
FILE* g_log_file = nullptr;

std::vector<LoadedModule> g_modules;
std::unordered_map<std::string, uint64_t> g_exports;

struct Thunk {
    std::string name;
    HleFn fn;
};
std::vector<Thunk> g_thunks;
std::unordered_map<std::string, uint64_t> g_thunk_by_name;

std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}

thread_local const char* t_missing_name = nullptr;

}  // namespace

// ---- logging --------------------------------------------------------------------------------------------------

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lk(g_log_lock);
    if (!g_log_file) {
        const char* path = std::getenv("NX_LOG");
#if defined(__ANDROID__)
        std::string def = plat::data_root() + "/nx.log";  // next to the data, readable from the phone
#elif defined(_WIN32)
        std::string def = plat::data_root() + "/nx.log";  // direct Explorer launch uses the same log location as the old batch launcher
#else
        // A developer's private/<game> keeps it at private/nx.log, where the test tools read it; a release's data
        // folder ($XDG_DATA_HOME/superwonders, a portable data/) keeps it inside, with everything else of the player's.
        const std::string root = plat::data_root();
        std::string def = root == game::kInfo.pc_data_root ? root + "/../nx.log" : root + "/nx.log";
#endif
        std::string file = path ? path : def;
        // Keep the previous session's log (a problem report should survive reopening the game).
        std::remove((file + ".prev").c_str());
        std::rename(file.c_str(), (file + ".prev").c_str());
        g_log_file = plat::fopen_utf8(file, "w");
    }
    plat::log_line(buf);
    ft::add(ft::kLogLines);  // the frame trace's log position (frametrace.h)
    if (g_log_file) {
        std::fprintf(g_log_file, "%s\n", buf);
        std::fflush(g_log_file);
    }
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
    plat::exit_process(1);
}

// ---- HLE registry ---------------------------------------------------------------------------------------------

HleRegistrar::HleRegistrar(const char* name, HleFn fn) { registry()[name] = fn; }

HleFn hle_lookup(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

static void missing_stub(CPU& c) {
    // Name comes from the thunk that dispatched here.
    log("missing import called: %s (lr %s) -> 0", t_missing_name, describe(c.x[30]).c_str());
    c.x[0] = 0;
}

uint64_t hle_address_or(const char* name, HleFn fallback) {
    static std::mutex lock;  // NVN hands out procs by name at run time, from any thread
    std::lock_guard lk(lock);
    // Other threads index g_thunks while we add to it: never reallocate (one entry per 16 bytes of the region).
    if (g_thunks.capacity() < kThunkSize / 16) g_thunks.reserve(kThunkSize / 16);
    auto it = g_thunk_by_name.find(name);
    if (it != g_thunk_by_name.end()) return it->second;
    if (g_thunks.size() == kThunkSize / 16) fatal("out of HLE thunks");
    HleFn fn = hle_lookup(name);
    uint64_t addr = kThunkBase + g_thunks.size() * 16;
    g_thunks.push_back({name, fn ? fn : fallback});
    g_thunk_by_name[name] = addr;
    return addr;
}

uint64_t hle_address(const char* name) { return hle_address_or(name, missing_stub); }

const char* hle_current_name() { return t_missing_name; }

// ---- dispatch -------------------------------------------------------------------------------------------------

const LoadedModule* module_at(uint64_t addr) {
    for (auto& m : g_modules)
        if (addr - m.base < m.size) return &m;
    return nullptr;
}

std::string describe(uint64_t addr) {
    char buf[512];
    if (addr - kThunkBase < kThunkSize) {
        size_t i = (addr - kThunkBase) / 16;
        snprintf(buf, sizeof buf, "hle:%s", i < g_thunks.size() ? g_thunks[i].name.c_str() : "?");
        return buf;
    }
    const LoadedModule* m = module_at(addr);
    if (!m) {
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)addr);
        return buf;
    }
    uint64_t off = addr - m->base;
    // Nearest preceding function symbol.
    const Symbol* best = nullptr;
    for (size_t k = 0; k < m->info->symbol_count; k++) {
        const Symbol& s = m->info->symbols[k];
        if (s.imported || s.type != 2 || s.value > off) continue;
        if (!best || s.value > best->value) best = &s;
    }
    if (best && off - best->value < 0x10000)
        snprintf(buf, sizeof buf, "%s+%llx (%s+%llx)", m->info->name, (unsigned long long)off, best->name,
                 (unsigned long long)(off - best->value));
    else
        snprintf(buf, sizeof buf, "%s+%llx", m->info->name, (unsigned long long)off);
    return buf;
}

void backtrace(CPU& c);

// Indirect calls (every blr of the game) look the target up here: the modules' text ranges in a small flat array.
struct FastModule {
    uint64_t base, text_size;
    const Fn* table;
};
FastModule g_fast[4];
size_t g_fast_count = 0;

void call(CPU& c, uint64_t t) {
    // Wonder's per-frame routine and frame governor have a few useful indirect-call sites. When requested,
    // report distinct targets and batch timing/value data without flooding nx.log.
    static const bool trace_frame_callbacks = [] {
        const char* e = std::getenv("NX_TRACE_FRAME_CALLBACKS");
        return e && *e && *e != '0';
    }();
    int frame_callback_site = -1;
    if (trace_frame_callbacks) {
        constexpr uint64_t main_base = 0x4000000000ull;
        constexpr std::array<uint64_t, 6> call_sites = {0xe18d4, 0xe18e4, 0xe18f4, 0xe1914, 0xe1928, 0xe16ac};
        constexpr std::array<const char*, 6> slots = {"vtable+0xe8", "vtable+0xf0", "vtable+0x70",
                                                      "vtable+0x110", "vtable+0x118", "governor-value"};
        const uint64_t lr = c.x[30];
        if (lr >= main_base) {
            const uint64_t off = lr - main_base;
            for (size_t site = 0; site < call_sites.size(); ++site) {
                if (off != call_sites[site]) continue;
                frame_callback_site = static_cast<int>(site);
                static std::mutex trace_lock;
                static std::array<std::array<uint64_t, 4>, 6> seen_targets{};
                static std::array<size_t, 6> seen_counts{};
                bool first = false;
                {
                    std::lock_guard lk(trace_lock);
                    auto& targets = seen_targets[site];
                    size_t& count = seen_counts[site];
                    first = std::find(targets.begin(), targets.begin() + count, t) == targets.begin() + count;
                    if (first && count < targets.size()) targets[count++] = t;
                    else first = false;
                }
                if (first)
                    log("frame-callback: main+%llx %s -> %s (target %llx, x0 %llx, x1 %llx)",
                        (unsigned long long)off, slots[site], describe(t).c_str(), (unsigned long long)t,
                        (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
                break;
            }
            // These are the indirect child calls inside the two most expensive per-frame callbacks
            // identified so far. Mapping their targets will show which subsystem actually receives
            // the update and where the elapsed-time value is consumed.
            constexpr std::array<uint64_t, 29> child_call_sites = {
                0xf20c0, 0xf20d4, 0xf20e4, 0xf2100, 0xf2118, 0xf2130, 0xf2148, 0xf2164, 0xf2194,
                0xf21d8, 0xf21e4, 0xf21f8, 0xf2210, 0xf224c, 0xf22b4, 0xf22c8, 0xf22dc, 0xf22f0,
                0xf2398, 0xf70e0, 0xf7114, 0xf7128, 0xf7144, 0xf7170, 0xf7198, 0xf71e4, 0xf7208,
                0xf7224, 0xf7374};
            for (size_t site = 0; site < child_call_sites.size(); ++site) {
                if (off != child_call_sites[site]) continue;
                static std::mutex child_trace_lock;
                static std::array<std::array<uint64_t, 4>, child_call_sites.size()> child_targets{};
                static std::array<size_t, child_call_sites.size()> child_counts{};
                bool first = false;
                {
                    std::lock_guard lk(child_trace_lock);
                    auto& targets = child_targets[site];
                    size_t& count = child_counts[site];
                    first = std::find(targets.begin(), targets.begin() + count, t) == targets.begin() + count;
                    if (first && count < targets.size()) targets[count++] = t;
                    else first = false;
                }
                if (first)
                    log("frame-child: main+%llx -> %s (target %llx, x0 %llx, x1 %llx)",
                        (unsigned long long)off, describe(t).c_str(), (unsigned long long)t,
                        (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
                break;
            }
        }
    }
    for (size_t k = 0; k < g_fast_count; k++) {
        const FastModule& m = g_fast[k];
        uint64_t off = t - m.base;
        if (off < m.text_size) [[likely]] {
            Fn fn = m.table[off >> 2];
            if (!fn) [[unlikely]] {
                for (int k = 0; k < 31; k += 4)
                    log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k],
                        (unsigned long long)c.x[k + 1], (unsigned long long)c.x[k + 2],
                        (unsigned long long)(k + 3 < 31 ? c.x[k + 3] : c.x[31]));
                backtrace(c);
                fatal("call into the middle of a function: %s (lr %s)", describe(t).c_str(), describe(c.x[30]).c_str());
            }
            if (trace_frame_callbacks && frame_callback_site >= 0) {
                using Clock = std::chrono::steady_clock;
                const auto start = Clock::now();
                // main+0xe1914 stores the elapsed guest ticks at object+0x118 immediately
                // before dispatching this callback. Capture that delta and the callback's
                // host cost in batches so the trace can tell whether the game update is
                // tied to elapsed time without producing a log line every frame.
                uint64_t guest_delta_ticks = 0;
                if (frame_callback_site == 3 && c.x[0])
                    guest_delta_ticks = ld<uint64_t>(c.x[0] + 0x118);
                const uint64_t governor_value = frame_callback_site == 5 ? c.x[1] : 0;
                fn(c);
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
                struct FrameCallbackStats {
                    uint64_t count = 0;
                    uint64_t guest_ticks = 0;
                    uint64_t min_ticks = UINT64_MAX;
                    uint64_t max_ticks = 0;
                    uint64_t host_ns = 0;
                    uint64_t max_host_ns = 0;
                    uint64_t value_one = 0;
                    uint64_t value_two = 0;
                    uint64_t value_other = 0;
                    uint64_t min_value = UINT64_MAX;
                    uint64_t max_value = 0;
                };
                static std::mutex stats_lock;
                static std::array<FrameCallbackStats, 6> stats{};
                bool report = false;
                FrameCallbackStats snapshot;
                {
                    std::lock_guard lk(stats_lock);
                    auto& s = stats[frame_callback_site];
                    ++s.count;
                    if (frame_callback_site == 3) {
                        s.guest_ticks += guest_delta_ticks;
                        s.min_ticks = std::min(s.min_ticks, guest_delta_ticks);
                        s.max_ticks = std::max(s.max_ticks, guest_delta_ticks);
                    }
                    if (frame_callback_site == 5) {
                        s.min_value = std::min(s.min_value, governor_value);
                        s.max_value = std::max(s.max_value, governor_value);
                        if (governor_value == 1) ++s.value_one;
                        else if (governor_value == 2) ++s.value_two;
                        else ++s.value_other;
                    }
                    const auto host_ns = static_cast<uint64_t>(elapsed);
                    s.host_ns += host_ns;
                    s.max_host_ns = std::max(s.max_host_ns, host_ns);
                    if (s.count >= 300) {
                        snapshot = s;
                        s = {};
                        report = true;
                    }
                }
                if (report) {
                    constexpr std::array<const char*, 6> names = {"vtable+0xe8", "vtable+0xf0", "vtable+0x70",
                                                                   "vtable+0x110", "vtable+0x118", "governor-value"};
                    if (frame_callback_site == 3) {
                        log("frame-callback-stats: %s n=%llu guest-delta-us min/avg/max=%.2f/%.2f/%.2f host-us avg/max=%.2f/%.2f",
                            names[frame_callback_site], (unsigned long long)snapshot.count,
                            double(snapshot.min_ticks) * 1'000'000.0 / 19'200'000.0,
                            double(snapshot.guest_ticks) * 1'000'000.0 / (19'200'000.0 * snapshot.count),
                            double(snapshot.max_ticks) * 1'000'000.0 / 19'200'000.0,
                            double(snapshot.host_ns) / (1000.0 * snapshot.count),
                            double(snapshot.max_host_ns) / 1000.0);
                    } else if (frame_callback_site == 5) {
                        log("frame-callback-stats: %s n=%llu value[min/max]=%llu/%llu counts[1/2/other]=%llu/%llu/%llu host-us avg/max=%.2f/%.2f",
                            names[frame_callback_site], (unsigned long long)snapshot.count,
                            (unsigned long long)snapshot.min_value, (unsigned long long)snapshot.max_value,
                            (unsigned long long)snapshot.value_one, (unsigned long long)snapshot.value_two,
                            (unsigned long long)snapshot.value_other,
                            double(snapshot.host_ns) / (1000.0 * snapshot.count),
                            double(snapshot.max_host_ns) / 1000.0);
                    } else {
                        log("frame-callback-stats: %s n=%llu host-us avg/max=%.2f/%.2f", names[frame_callback_site],
                            (unsigned long long)snapshot.count, double(snapshot.host_ns) / (1000.0 * snapshot.count),
                            double(snapshot.max_host_ns) / 1000.0);
                    }
                }
            } else {
                fn(c);
            }
            return;
        }
    }
    uint64_t off = t - kThunkBase;
    if (off < kThunkSize && off / 16 < g_thunks.size()) {
        const Thunk& th = g_thunks[off / 16];
        if (g_trace) log("hle %s", th.name.c_str());
        const char* prev = t_missing_name;
        t_missing_name = th.name.c_str();
        th.fn(c);
        t_missing_name = prev;
        return;
    }
    fatal("call to %llx (%s), lr %s", (unsigned long long)t, describe(t).c_str(), describe(c.x[30]).c_str());
}

void trace_sp(CPU& c, uint64_t pc) {
    log("sp %llx x19 %llx at %s", (unsigned long long)c.x[31], (unsigned long long)c.x[19], describe(pc).c_str());
}

[[noreturn]] void unimpl(CPU& c, uint64_t pc, const char* what) {
    for (int k = 0; k < 31; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)(k + 3 < 32 ? c.x[k + 3] : 0));
    backtrace(c);
    fatal("unimplemented instruction at %s: %s (lr %s)", describe(pc).c_str(), what, describe(c.x[30]).c_str());
}

// ---- loading and linking --------------------------------------------------------------------------------------

uint64_t find_symbol(const char* name) {
    auto it = g_exports.find(name);
    return it == g_exports.end() ? 0 : it->second;
}

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir) {
    // Map every module first so symbols of all of them are known before relocating.
    for (size_t i = 0; i < count; i++) {
        const ModuleInfo* mi = mods[i];
        uint64_t image = mi->data + mi->data_size;
        uint64_t size = ((mi->bss + mi->bss_size) + 0xFFF) & ~0xFFFull;
        if (!commit(mi->base, size)) fatal("cannot commit module %s", mi->name);
        std::string path = data_dir + "/" + mi->name + ".bin";
        std::ifstream f(path, std::ios::binary);
        if (!f) fatal("missing %s (run tools/nso2bin.py)", path.c_str());
        f.read(reinterpret_cast<char*>(mi->base), std::streamsize(image));
        if (uint64_t(f.gcount()) != image) fatal("%s: expected %llx bytes", path.c_str(), (unsigned long long)image);
        vm_set(mi->base + mi->text, mi->text_size, kMemCode, 5);
        vm_set(mi->base + mi->ro, mi->ro_size, kMemCode, 1);
        vm_set(mi->base + mi->data, size - mi->data, kMemCodeData, 3);

        LoadedModule lm{mi, mi->base, size, new Fn[mi->text_size / 4]()};
        for (size_t k = 0; k < mi->func_count; k++) lm.table[mi->funcs[k].offset / 4] = mi->funcs[k].fn;
        g_modules.push_back(lm);
        if (g_fast_count < 4) g_fast[g_fast_count++] = {lm.base, mi->text_size, lm.table};

        for (size_t k = 0; k < mi->symbol_count; k++) {
            const Symbol& s = mi->symbols[k];
            if (s.imported || !s.name[0] || s.bind == 0) continue;  // skip locals
            auto it = g_exports.find(s.name);
            if (it == g_exports.end() || s.bind == 1)  // global beats weak; first global wins
                if (it == g_exports.end()) g_exports[s.name] = mi->base + s.value;
        }
        log("loaded %s at %llx (%llu functions, %llu relocs)", mi->name, (unsigned long long)mi->base,
            (unsigned long long)mi->func_count, (unsigned long long)mi->reloc_count);
    }

    size_t hle = 0, missing = 0, linked = 0;
    for (auto& lm : g_modules) {
        const ModuleInfo* mi = lm.info;
        for (size_t k = 0; k < mi->reloc_count; k++) {
            const Reloc& r = mi->relocs[k];
            uint64_t* slot = reinterpret_cast<uint64_t*>(lm.base + r.offset);
            if (r.type == R_AARCH64_RELATIVE) {
                *slot = lm.base + uint64_t(r.addend);
                continue;
            }
            if (r.type != R_AARCH64_GLOB_DAT && r.type != R_AARCH64_JUMP_SLOT && r.type != R_AARCH64_ABS64) {
                log("%s: relocation type %u at %llx not handled", mi->name, r.type, (unsigned long long)r.offset);
                continue;
            }
            const Symbol& s = mi->symbols[r.sym];
            uint64_t target = 0;
            if (s.type == 2 || s.imported) {
                if (hle_lookup(s.name)) {
                    target = hle_address(s.name);
                    hle++;
                }
            }
            if (!target) {
                target = find_symbol(s.name);
                if (target) linked++;
            }
            if (!target && !s.imported) target = lm.base + s.value;
            if (!target) {
                // Unresolved: functions get a logging stub; data stays null.
                if (s.type == 2 || s.type == 0) target = hle_address(s.name);
                missing++;
            }
            *slot = target + uint64_t(r.addend);
        }
    }
    log("linked: %zu to HLE, %zu across modules, %zu unresolved", hle, linked, missing);
}

}  // namespace nx
