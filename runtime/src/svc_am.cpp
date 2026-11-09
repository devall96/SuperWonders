// am services (appletOE): application proxy, focus/operation-mode state, launch parameters.
#include <atomic>
#include <cstdlib>
#include <deque>

#include "ipc.h"

namespace nx {

namespace {
std::atomic<int> g_internal_resolution_preset{0};
}

void set_internal_resolution_preset(int preset) {
    if (preset < 0) preset = 0;
    if (preset > 4) preset = 4;
    g_internal_resolution_preset.store(preset, std::memory_order_relaxed);
}

float internal_resolution_scale() {
    static constexpr float scales[] = {1.0f, 2.0f / 3.0f, 5.0f / 6.0f, 1.0f, 4.0f / 3.0f};
    return scales[g_internal_resolution_preset.load(std::memory_order_relaxed)];
}

uint32_t internal_resolution_target_width() {
    static constexpr uint32_t widths[] = {0, 1280, 1600, 1920, 2560};
    return widths[g_internal_resolution_preset.load(std::memory_order_relaxed)];
}

namespace {

constexpr uint32_t kAmNoMessage = 0x680;  // am error 3
constexpr uint32_t kMsgFocusStateChanged = 15;
constexpr uint32_t kMsgOperationModeChanged = 30;
constexpr uint32_t kMsgPerformanceModeChanged = 31;

struct AppletState {
    std::deque<uint32_t> messages{kMsgFocusStateChanged, kMsgOperationModeChanged, kMsgPerformanceModeChanged};
    std::shared_ptr<EventState> message_event = std::make_shared<EventState>();
};

AppletState& applet() {
    static AppletState s;
    return s;
}

uint32_t event_handle(std::shared_ptr<EventState> st) {
    auto e = std::make_shared<KEvent>();
    e->state = std::move(st);
    return handle_add(e);
}

std::shared_ptr<Service> svc(const char* name, std::function<void(Service&)> setup) {
    auto s = std::make_shared<Service>(name);
    setup(*s);
    return s;
}

std::shared_ptr<Service> common_state_getter() {
    return svc("ICommonStateGetter", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {  // GetEventHandle
            applet().message_event->signaled = !applet().messages.empty();
            c.out_copy.push_back(event_handle(applet().message_event));
            return 0u;
        };
        s.cmds[1] = [](IpcCtx& c) {  // ReceiveMessage
            auto& a = applet();
            if (a.messages.empty()) {
                a.message_event->signaled = false;
                return kAmNoMessage;
            }
            c.push<uint32_t>(a.messages.front());
            a.messages.pop_front();
            if (a.messages.empty()) a.message_event->signaled = false;
            return 0u;
        };
        // NX_HANDHELD=1: handheld mode, so the game renders for 1280x720 (a lighter GPU load) instead of docked 1080p.
        static const bool handheld = std::getenv("NX_HANDHELD") != nullptr;
        s.cmds[5] = [](IpcCtx& c) { c.push<uint8_t>(handheld ? 0 : 1); return 0u; };   // GetOperationMode
        s.cmds[6] = [](IpcCtx& c) { c.push<uint32_t>(handheld ? 0 : 1); return 0u; };  // GetPerformanceMode
        s.cmds[9] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };   // GetCurrentFocusState: in focus
        s.cmds[60] = [](IpcCtx& c) {  // GetDefaultDisplayResolution
            const int preset = g_internal_resolution_preset.load(std::memory_order_relaxed);
            static constexpr uint32_t widths[] = {1920, 1280, 1600, 1920, 2560};
            static constexpr uint32_t heights[] = {1080, 720, 900, 1080, 1440};
            const uint32_t width = preset ? widths[preset] : handheld ? 1280 : 1920;
            const uint32_t height = preset ? heights[preset] : handheld ? 720 : 1080;
            static int logged_preset = -1;
            if (logged_preset != preset) {
                log("am: GetDefaultDisplayResolution preset %d -> %ux%u", preset, width, height);
                logged_preset = preset;
            }
            c.push<uint32_t>(width);
            c.push<uint32_t>(height);
            return 0u;
        };
        s.cmds[61] = [](IpcCtx& c) {  // GetDefaultDisplayResolutionChangeEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
    });
}

std::shared_ptr<Service> self_controller() {
    return svc("ISelfController", [](Service& s) {
        s.cmds[40] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // CreateManagedDisplayLayer
        s.cmds[0] = [](IpcCtx&) { return 0u; };                             // Exit
        s.cmds[62] = [](IpcCtx&) { return 0u; };                            // SetIdleTimeDetectionExtension
        for (uint32_t cmd : {19u, 50u, 71u, 110u})                           // AccumulatedSuspendedTickChangedEvent etc.
            s.cmds[cmd] = [](IpcCtx& c) {
                c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
                return 0u;
            };
    });
}

std::shared_ptr<Service> window_controller() {
    return svc("IWindowController", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) { c.push<uint64_t>(0x51); return 0u; };  // GetAppletResourceUserId
    });
}

std::shared_ptr<Service> application_functions() {
    return svc("IApplicationFunctions", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) {  // PopLaunchParameter(kind): the preselected user, once
            static bool popped = false;
            if (c.arg<uint32_t>(0) != 2 || popped) return 0x2A80u;
            popped = true;
            c.push_object(preselected_user_storage());
            return 0u;
        };
        s.cmds[20] = [](IpcCtx& c) { c.push<uint64_t>(0); return 0u; };  // EnsureSaveData
        s.cmds[21] = [](IpcCtx& c) {                                      // GetDesiredLanguage
            uint64_t code = 0;
            std::memcpy(&code, "en-US", 5);
            c.push<uint64_t>(code);
            return 0u;
        };
        s.cmds[40] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // NotifyRunning
        s.cmds[50] = [](IpcCtx& c) {                                     // GetPseudoDeviceId
            c.push<uint64_t>(0x1234);
            c.push<uint64_t>(0x5678);
            return 0u;
        };
        s.cmds[130] = [](IpcCtx& c) {  // GetGpuErrorDetectedSystemEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
        s.cmds[210] = [](IpcCtx&) { return 0u; };  // UnsetGameCardEject... (stub)
    });
}

std::shared_ptr<Service> application_proxy() {
    return svc("IApplicationProxy", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(common_state_getter()); return 0u; };
        s.cmds[1] = [](IpcCtx& c) { c.push_object(self_controller()); return 0u; };
        s.cmds[2] = [](IpcCtx& c) { c.push_object(window_controller()); return 0u; };
        s.cmds[3] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IAudioController")); return 0u; };
        s.cmds[4] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDisplayController")); return 0u; };
        s.cmds[10] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IProcessWindingController")); return 0u; };
        s.cmds[11] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("ILibraryAppletCreator")); return 0u; };
        s.cmds[20] = [](IpcCtx& c) { c.push_object(application_functions()); return 0u; };
        s.cmds[1000] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDebugFunctions")); return 0u; };
    });
}

ServiceRegistrar reg_applet_oe("appletOE", [] {
    return svc("appletOE", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(application_proxy()); return 0u; };  // OpenApplicationProxy
    });
});

}  // namespace

}  // namespace nx
