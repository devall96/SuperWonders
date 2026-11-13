// The recomp's entries in Wonder's own menus (docs/MENU.md). tools/wonder_menu_mod.py adds the buttons to the game's
// data; this file gives them meaning.
//
// Pause menu "Quit Game": its button (data) reports game::sequence::PauseResult "QuitToDesktop", a name the game does
// not know. Enum types are registered with callbacks the game only reaches through function pointers, so swapping a
// function-table entry replaces one for the whole game. The pause screen turns the chosen button's name into its value
// with the type's from-string callback when the button is chosen (not before): that call is the moment to quit.
#include <cstring>

#include <nx/game.h>
#include <nx/module.h>
#include <nx/runtime.h>

#include "platform.h"

extern const nx::ModuleInfo nx_module_main;

namespace nx::wonder {

namespace {

// game::sequence::PauseResult's from-string callback in main (1.2.0): bool (int* out, const char** name).
constexpr uint64_t kPauseResultFromString = 0x1d3415c;
Fn g_pause_result_from_string;

void pause_result_from_string(CPU& c) {
    if (std::strcmp(reinterpret_cast<const char*>(ld<uint64_t>(c.x[1])), "QuitToDesktop") != 0)
        return g_pause_result_from_string(c);
    log("menu: Quit Game chosen in the pause menu");
    plat::exit_process(0);  // what closing the window does
}

}  // namespace

void menu_init() {
    Fn* table = module_at(nx_module_main.base)->table;
    g_pause_result_from_string = table[kPauseResultFromString / 4];
    if (!g_pause_result_from_string)
        fatal("menu: no function at main+%llx (not the 1.2.0 code?)", (unsigned long long)kPauseResultFromString);
    table[kPauseResultFromString / 4] = pause_result_from_string;
}

}  // namespace nx::wonder
