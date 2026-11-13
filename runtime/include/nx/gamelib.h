// The game code library: the recompiled modules built on the player's own PC, apart from the program.
//
// The program (wonder.exe, the runtime) holds nothing of the game, so it can ship ready built. The code recompiled
// from the player's main and sdk goes into a library of its own (wonder_game.dll / libwonder_game.so), built from
// their files, and the program loads it at start (runtime/src/gamelib.cpp). Savage Wind splits the same way (its
// swgame library, built on the player's PC): an update replaces the program, and the game code is only built again
// when the code generator changed.
//
// The boundary is small. The generated code calls four runtime functions (nx::call, svc, mrs, unimpl; msr and
// trace_sp when generated with them): the library defines them as jumps through the table the program hands over in
// nx_gamelib_bind (runtime/src/gamelib_entry.cpp), so it needs nothing exported from the program. The program gets
// the two module tables back. Both use the same C++ runtime (a DLL on Windows, the system's on Linux), so the
// LongJmp the runtime throws for a guest longjmp is caught by the recompiled function that called setjmp.
//
// Built as one program instead (NX_GAME_LIBRARY=OFF, Android and macOS), none of this is used.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nx/cpu.h>
#include <nx/module.h>

namespace nx {

// Raised whenever the boundary changes: what the generated code calls, these structs, or the layout of CPU. A library
// built for another version is refused (the program says to build the game code again) rather than run.
constexpr uint32_t kGameLibAbi = 1;

// Both sides check it: an older or newer cpu.h with the same kGameLibAbi still can't load.
constexpr uint64_t gamelib_layout() {
    return (uint64_t(kGameLibAbi) << 48) ^ (uint64_t(sizeof(CPU)) << 24) ^ (uint64_t(sizeof(ModuleInfo)) << 8) ^
           uint64_t(sizeof(FuncEntry));
}

struct GameLibHost {
    uint64_t layout;  // gamelib_layout() of the program
    void (*call)(CPU& c, uint64_t target);
    void (*unimpl)(CPU& c, uint64_t pc, const char* what);
    void (*svc)(CPU& c, uint32_t imm, uint64_t pc);
    uint64_t (*mrs)(CPU& c, uint32_t sysreg);
    void (*msr)(CPU& c, uint32_t sysreg, uint64_t value);
    void (*trace_sp)(CPU& c, uint64_t pc);
};

struct GameLib {
    uint64_t layout;    // gamelib_layout() of the library
    const char* game;   // NX_GAME it was built for ("wonder")
    const ModuleInfo* main;
    const ModuleInfo* sdk;
};

// The program's side (runtime/src/gamelib.cpp): the modules, from the library or linked in. Fatal if there are none.
const GameLib& game_lib();
#if NX_GAME_LIBRARY
std::string game_lib_path();  // the library game_lib() would load, "" when there is none (the launcher)
std::string program_dir();    // the folder of the running program
#endif

}  // namespace nx

#if defined(_WIN32)
#define NX_GAMELIB_EXPORT extern "C" __declspec(dllexport)
#else
#define NX_GAMELIB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// The library's only export. When host->layout isn't its own it keeps nothing of the host's and returns its GameLib
// anyway, so the program can say which layouts differ.
#define NX_GAMELIB_BIND_NAME "nx_gamelib_bind"
using NxGameLibBind = const nx::GameLib* (*)(const nx::GameLibHost* host);
