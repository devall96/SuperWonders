// nn::vi (display/layers) and nv:: (graphics driver setup) at the API level. The layer is the platform's window
// (Win32 window / Android surface); its handle is the "native window" the game hands to vkCreateViSurfaceNN, which
// the Vulkan bridge turns into the platform's surface.
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx {

namespace {

constexpr uint64_t kFakeDisplay = kKernelBase - 0x1000;  // opaque pointers, never dereferenced by the game
constexpr uint64_t kFakeLayer = kKernelBase - 0x2000;

void ok(CPU& c) { c.x[0] = 0; }

void OpenDefaultDisplay(CPU& c) {
    st<uint64_t>(c.x[0], kFakeDisplay);
    c.x[0] = 0;
}

void CreateLayer(CPU& c) {
    void* w = plat::create_window();
    log("vi: layer created -> native window %p", w);
    plat::release_window(w);
    st<uint64_t>(c.x[0], kFakeLayer);
    c.x[0] = 0;
}

void GetNativeWindow(CPU& c) {  // (void** out, Layer*)
    st<uint64_t>(c.x[0], uint64_t(plat::create_window()));
    c.x[0] = 0;
}

void nv_noop(CPU&) {}

// The display's vsync: a kernel event a host thread signals every 1/60 s. The game gets it as an nn::os
// SystemEvent (AttachSystemEvent with our read/write handles, auto clear: each wait consumes one vsync). The same
// ticks pace presents (vsync_wait, nvn_queue.cpp).
std::shared_ptr<EventState> g_vsync = std::make_shared<EventState>();
std::mutex g_tick_m;
std::condition_variable g_tick_cv;
uint64_t g_ticks = 0;

void vsync_thread() {
    plat::set_thread_name("vsync");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        // On the display's own blank where its refresh is a multiple of 60 Hz: a timer of our own drifted against
        // it, and every few seconds MAILBOX showed a frame twice or dropped one (microstutter).
        if (plat::fast_forward()) {  // ticks as fast as the game takes them (500 Hz at most)
            plat::sleep_ns(2'000'000);
            next = plat::monotonic_ns();
        } else if (plat::wait_vblank()) {
            next = plat::monotonic_ns();
        } else {
            next += 16'666'667;
            int64_t wait = int64_t(next - plat::monotonic_ns());
            if (wait > 0) plat::sleep_ns(wait);
        }
        g_vsync->signaled = true;
        notify_all();
        {
            std::lock_guard lk(g_tick_m);
            g_ticks++;
        }
        g_tick_cv.notify_all();
    }
}

void start_vsync() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(vsync_thread).detach(); });
}

void GetDisplayVsyncEvent(CPU& c) {  // (os::SystemEventType*, Display*)
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = g_vsync;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEventType; 1: AutoClear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    start_vsync();
    c.x[0] = 0;
}

}  // namespace

// Waits until at least `target` vsyncs have passed since start; the count then.
uint64_t vsync_wait(uint64_t target) {
    start_vsync();
    std::unique_lock lk(g_tick_m);
    g_tick_cv.wait(lk, [&] { return g_ticks >= target; });
    return g_ticks;
}

NX_HLE("_ZN2nn2vi10InitializeEv", ok);
NX_HLE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", OpenDefaultDisplay);
NX_HLE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", CreateLayer);
NX_HLE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", GetNativeWindow);
NX_HLE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", GetDisplayVsyncEvent);
NX_HLE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv18InitializeGraphicsEPvm", nv_noop);

}  // namespace nx
