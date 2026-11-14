// Per-frame trace for the performance suite (tools/perf/, docs/PERF-SUITE.md). NX_FRAMETRACE=<path.csv> writes one row
// per frame presented to the display: when it was shown, how long since the last one, and what happened in between
// (pipelines built, draws skipped while compiling, ASTC decodes, uploads, file reads, page faults, the presenting
// thread's and the game thread's CPU time, the waits on the swapchain, the GPU, the replay and the vsync pacing, and
// the log line count, so a hitch can be matched to the log). Averages hide stutter: a 60 fps median can hold a
// 160 ms frame. The report (tools/perf/frametrace_report.py) turns the rows into 1% / 0.1% lows, hitches with their
// cause, and stutter.
//
// The counters are always on: one relaxed atomic add at events that cost far more themselves. Nothing is written
// without NX_FRAMETRACE. Design after Savage Wind's frametrace (Revivedx/SavageWindRecomp: runtime/perfcount.h,
// tools/perf_routes.py), which records what each frame did and names the cause of each hitch.
#pragma once

#include <atomic>
#include <cstdint>

namespace nx::ft {

enum Counter {
    kPipelines,    // graphics and compute pipelines created (any thread)
    kPipelineNs,   // ... their creation time
    kSkipped,      // draws skipped because their pipeline was still compiling
    kAstc,         // ASTC textures decoded on the CPU
    kAstcNs,
    kFileReads,    // guest file and RomFS reads
    kFileBytes,
    kFileNs,
    kReplayNs,     // the presenting thread recording the game's command lists into Vulkan
    kUploadBytes,  // bytes through the upload buffer
    kLogLines,     // log() lines written (the report prints a hitch's lines from the NX_LOG file)
    kCount
};
extern std::atomic<uint64_t> g_counter[kCount];
inline void add(Counter k, uint64_t v = 1) { g_counter[k].fetch_add(v, std::memory_order_relaxed); }

bool enabled();
// The game's present (nvnQueuePresentTexture, on the game's thread): replay_wait_ns is the time it waited for the
// replay of an earlier frame (at most two queued), pace_ns the time it waited for the vsync pacing.
void guest_present(uint64_t replay_wait_ns, uint64_t pace_ns, int interval);
// The display present (gpu_present, on the presenting thread), after vkQueuePresentKHR: acquire_ns is this frame's
// vkAcquireNextImageKHR, queue_present_ns its vkQueuePresentKHR; encode/fence are the presenting thread's cumulative
// vkQueueSubmit and fence-wait totals (frame_times()), differenced here.
// Returns the row's frame number (UINT64_MAX without a trace), for displayed().
uint64_t host_present(uint64_t acquire_ns, uint64_t queue_present_ns, uint64_t encode_total_ns, uint64_t fence_total_ns);
// When frame `frame` reached the display (vkWaitForPresentKHR returned): <NX_FRAMETRACE>.display.csv, the same
// clock as the trace's t_ms. Where the GPU and surface support it; the report then reports what was on screen.
void displayed(uint64_t frame, uint64_t t_ns);

}  // namespace nx::ft
