// Built into the game code library only (nx/gamelib.h): the runtime functions the generated code calls, as jumps
// through the program's table, and the library's one export, nx_gamelib_bind.
#include <nx/gamelib.h>

extern const nx::ModuleInfo nx_module_main;
extern const nx::ModuleInfo nx_module_sdk;

#ifndef NX_GAME_NAME
#error "NX_GAME_NAME: the game this library is for (CMake sets it)"
#endif

namespace {
nx::GameLibHost g_host;  // set once, before the program runs any of the game's code
}

namespace nx {

void call(CPU& c, uint64_t target) { g_host.call(c, target); }
[[noreturn]] void unimpl(CPU& c, uint64_t pc, const char* what) {
    g_host.unimpl(c, pc, what);
    __builtin_unreachable();
}
void svc(CPU& c, uint32_t imm, uint64_t pc) { g_host.svc(c, imm, pc); }
uint64_t mrs(CPU& c, uint32_t sysreg) { return g_host.mrs(c, sysreg); }
void msr(CPU& c, uint32_t sysreg, uint64_t value) { g_host.msr(c, sysreg, value); }
void trace_sp(CPU& c, uint64_t pc) { g_host.trace_sp(c, pc); }

}  // namespace nx

NX_GAMELIB_EXPORT const nx::GameLib* nx_gamelib_bind(const nx::GameLibHost* host) {
    static const nx::GameLib lib = {nx::gamelib_layout(), NX_GAME_NAME, &nx_module_main, &nx_module_sdk};
    if (!host || host->layout != nx::gamelib_layout()) return &lib;  // (the program sees the mismatch and stops)
    g_host = *host;
    return &lib;
}
