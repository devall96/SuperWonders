// A VkPipelineCache kept on disk (<data root>/pipelines_<vendor>_<device>.bin), and the cost of what the replay
// creates on the fly. Pipelines are compiled the first time a draw needs them, inside the replay: the course's
// stutters when new objects come in (16-42 ms replays on the RTX 3050). The driver checks the cache header itself
// (vendor, device, driver build): a stale file is ignored. Saved periodically when it grows and forcibly at shutdown.
// NX_PIPELINE_CACHE=0: no cache (bisecting, or to measure first-run stutter).
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <vector>

#include "nvn_pipeline_async.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_tried = false;
std::atomic<bool> g_cache_dirty{false};
size_t g_saved_size = 0;
std::string g_path;
uint64_t g_pipelines = 0, g_pipeline_ns = 0, g_astc = 0, g_astc_ns = 0;

void save_pipeline_cache_at_exit() { pipeline_cache_save(true); }

std::atomic<bool> g_cache_saving{false};

// On the background writer (periodic saves) or, at shutdown, on the caller after the writer is idle.
void write_pipeline_cache(bool force) {
    // Cleared first: a pipeline created while the data is read marks it dirty again for the next save.
    g_cache_dirty.store(false, std::memory_order_release);
    size_t n = 0;
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, nullptr) != VK_SUCCESS || !n) return note_pipeline_cache_changed();
    // Vulkan drivers generally grow the serialized cache as new entries are added. Avoid rewriting a large cache
    // every few seconds when its serialized size is unchanged; the explicit shutdown flush forces same-size updates.
    if (!force && n <= g_saved_size) return note_pipeline_cache_changed();  // still owed: the shutdown save writes it
    std::vector<uint8_t> data(n);
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, data.data()) != VK_SUCCESS) return note_pipeline_cache_changed();
    std::string tmp = g_path + ".tmp";
    if (FILE* f = plat::fopen_utf8(tmp, "wb")) {
        bool ok = std::fwrite(data.data(), 1, n, f) == n;
        ok = std::fclose(f) == 0 && ok;
        std::remove(g_path.c_str());
        if (ok && std::rename(tmp.c_str(), g_path.c_str()) == 0) {
            g_saved_size = n;
            log("nvn/vk: pipeline cache saved (%zu bytes)%s", n, force ? " at shutdown" : "");
            return;
        }
    }
    note_pipeline_cache_changed();  // not written: try again next time
}

void save_pipeline_cache(bool force) {
    if (!g_cache || !g_cache_dirty.load(std::memory_order_acquire)) return;
    if (force) {
        wait_background_writes();
        write_pipeline_cache(true);
        return;
    }
    if (g_cache_saving.exchange(true)) return;  // the last one is still being written
    background_write([] {
        write_pipeline_cache(false);
        g_cache_saving.store(false);
    });
}

}  // namespace

VkPipelineCache pipeline_cache() {
    if (g_tried) return g_cache;
    g_tried = true;
    if (const char* e = std::getenv("NX_PIPELINE_CACHE"); e && e[0] == '0') return VK_NULL_HANDLE;
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines_%04x_%04x.bin", p.vendorID, p.deviceID);
    g_path = plat::data_root() + name;
    std::vector<uint8_t> data;
    if (FILE* f = plat::fopen_utf8(g_path, "rb")) {
        plat::fseek64(f, 0, SEEK_END);
        int64_t n = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_SET);
        if (n > 0) data.resize(size_t(n)), data.resize(std::fread(data.data(), 1, data.size(), f));
        std::fclose(f);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    bool loaded = p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache) == VK_SUCCESS;
    if (!loaded) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;  // a cache the driver refuses: start empty
        p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache);
        data.clear();
    }
    g_saved_size = data.size();
    std::atexit(save_pipeline_cache_at_exit);
    log("nvn/vk: pipeline cache %s (%zu bytes loaded)", g_path.c_str(), loaded ? data.size() : size_t(0));
    return g_cache;
}

void pipeline_cache_save(bool force) { save_pipeline_cache(force); }

void note_pipeline_cache_changed() { g_cache_dirty.store(true, std::memory_order_release); }
void note_pipeline_created(uint64_t ns) { note_pipeline_cache_changed(); g_pipelines++, g_pipeline_ns += ns; }
void note_astc_decoded(uint64_t ns) { g_astc++, g_astc_ns += ns; }

// One line per timing window (nvn_vk.cpp log_timing): what the replay created, and the time it took.
void creation_log(uint64_t frames) {
    if (g_pipelines || g_astc || g_pipelines_skipped)
        log("nvn/vk: created over %llu frames: %llu pipelines (%.1f ms), %llu ASTC textures decoded (%.1f ms), "
            "%llu draws skipped while compiling",
            (unsigned long long)frames, (unsigned long long)g_pipelines, double(g_pipeline_ns) / 1e6,
            (unsigned long long)g_astc, double(g_astc_ns) / 1e6, (unsigned long long)g_pipelines_skipped);
    g_pipelines = g_pipeline_ns = g_astc = g_astc_ns = g_pipelines_skipped = 0;
    static uint64_t windows = 0;
    if (++windows % 10 == 0) pipeline_cache_save(), save_pipeline_keys();  // on the background writer
}

}  // namespace nx::nvn
