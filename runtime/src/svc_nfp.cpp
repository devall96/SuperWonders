// Host-side NFP service for systems without a Switch NFC reader. It implements
// the user-service command surface and reports an empty device list instead of
// claiming that operations on a nonexistent Amiibo succeeded. Reading actual
// tags requires a separate host NFC-device integration.
#include "ipc.h"

#include <atomic>

namespace nx {
namespace {

// Result format: (description << 9) | NFP module id (0x73).
constexpr uint32_t kNfpDeviceNotFound = (64u << 9) | 0x73u;
constexpr uint32_t kNfcDisabled = (80u << 9) | 0x73u;

std::shared_ptr<Service> make_nfp_user_interface() {
    auto user = std::make_shared<Service>("nfp:IUser");
    auto availability_changed = std::make_shared<EventState>();
    auto initialized = std::make_shared<std::atomic_bool>(false);
    auto no_device = [initialized](IpcCtx&) {
        return initialized->load(std::memory_order_relaxed) ? kNfpDeviceNotFound : kNfcDisabled;
    };

    user->cmds[0] = [initialized](IpcCtx&) { initialized->store(true, std::memory_order_relaxed); return 0u; };
    user->cmds[1] = [initialized](IpcCtx&) { initialized->store(false, std::memory_order_relaxed); return 0u; };
    user->cmds[2] = [initialized](IpcCtx& ctx) {  // ListDevicesNfp -> empty list, count.
        if (!initialized->load(std::memory_order_relaxed)) return kNfcDisabled;
        Buffer out = ctx.out_buf(0);
        if (out.addr && out.size) std::memset(reinterpret_cast<void*>(out.addr), 0, size_t(out.size));
        ctx.push<int32_t>(0);
        return 0u;
    };

    // Commands that require a physical NFC device or a detected tag. Return
    // the service's documented error instead of a success with missing data.
    for (uint32_t cmd : {3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u,
                         13u, 14u, 15u, 16u, 17u, 18u, 20u, 21u, 22u, 24u, 25u})
        user->cmds[cmd] = no_device;

    user->cmds[19] = [initialized](IpcCtx& ctx) {  // GetStateNfp -> NonInitialized / Initialized.
        ctx.push<uint32_t>(initialized->load(std::memory_order_relaxed) ? 1u : 0u);
        return 0u;
    };
    user->cmds[23] = [availability_changed, initialized](IpcCtx& ctx) {  // GetAvailabilityChangeEventHandleNfp.
        if (!initialized->load(std::memory_order_relaxed)) return kNfcDisabled;
        auto event = std::make_shared<KEvent>();
        event->state = availability_changed;  // Never signaled: no NFC/Amiibo device is available.
        ctx.out_copy.push_back(handle_add(event));
        return 0u;
    };
    return user;
}

std::shared_ptr<Service> make_nfp_user_manager() {
    auto manager = std::make_shared<Service>("nfp:user");
    manager->cmds[0] = [](IpcCtx& ctx) {  // CreateUserInterface -> IUser session.
        ctx.push_object(make_nfp_user_interface());
        return 0u;
    };
    return manager;
}

ServiceRegistrar r_nfp_user("nfp:user", make_nfp_user_manager);

}  // namespace
}  // namespace nx
