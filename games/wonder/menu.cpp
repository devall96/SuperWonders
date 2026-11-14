// The recomp's entries in Wonder's own menus (docs/MENU.md). tools/wonder_menu_mod.py adds them to the game's data;
// this file gives them meaning. All three quit the way closing the window does (plat::exit_process: the pipeline
// cache is saved, the log flushed).
//
// Pause menu "Quit Game": the button (data) reports game::sequence::PauseResult "QuitToDesktop", a name the game does
// not know. Enum types are registered with
// callbacks the game only reaches through function pointers, so swapping a function-table entry replaces one for the
// whole game. The pause screen turns the chosen button's name into its value with the type's from-string callback
// when the button is chosen (not before): that call is the moment to quit. Ported from the josiahg2502-glitch fork
// (b2e9708), which targets 1.2.0; the offsets here are 1.0.0's. (The fork's world-map menu button, b17c70a, is not
// here: at 1.0.0 that menu's cursor never stops on an added button. game::sequence::MainMenuResult's from-string
// callback is main+0x1c0beec, should it be wanted.)
//
// Title screen: it has no buttons (it waits for L + R), so the mod adds a "(-) Quit Game" line and holding Minus there
// quits. The title's sequence waits in an AINB node, WaitPressKeyInTitle, whose update (a virtual function, so also
// swappable) runs every frame until L + R: while it runs, the title is up.
#include <chrono>
#include <cstring>

#include <nx/game.h>
#include <nx/gamelib.h>
#include <nx/module.h>
#include <nx/runtime.h>

#include "platform.h"


namespace nx::wonder {

namespace {

// A function in main (1.0.0) the recomp replaces, and its first instructions: a different game version has other code
// there, and then that entry is left alone.
struct Hook {
    const char* what;
    uint64_t offset;
    uint32_t code[8];
    Fn replacement;
    Fn game = nullptr;
};

// PauseResult's from-string callback: bool (int* out, const char** name). Enum types are registered by main+0x581d38.
void pause_result_from_string(CPU& c);
// WaitPressKeyInTitle's update (this): done once the title's key state (main+0x3630a98, +0x4c) reads 3.
void wait_press_key_in_title(CPU& c);

Hook g_pause = {"pause menu", 0x1c0bd9c,
                {0xa9be7bfd, 0xf9000bf3, 0x910003fd, 0xf9400021, 0xaa0003f3, 0x910063a0, 0xb9001bbf, 0x940015a4},
                pause_result_from_string};
Hook g_title = {"title screen", 0x1bfcddc,
                {0x9000d1a8, 0xf9454d08, 0xb9404d08, 0x71000d1f, 0x54000040, 0xd65f03c0, 0xb9401808, 0x52b00009},
                wait_press_key_in_title};

[[noreturn]] void quit(const char* where) {
    log("menu: Quit Game chosen on the %s", where);
    plat::exit_process(0);  // what closing the window does
}

void pause_result_from_string(CPU& c) {
    if (std::strcmp(reinterpret_cast<const char*>(ld<uint64_t>(c.x[1])), "QuitToDesktop") == 0) quit(g_pause.what);
    g_pause.game(c);
}

// Minus held on its own this long quits: a held press, so a tap (or the start of Minus + Plus, the overlay's combo,
// which the game never sees) does not.
constexpr auto kHold = std::chrono::milliseconds(400);

// The title takes any button as "start", Minus too: while Minus is held the game's update is held back (the title
// keeps waiting), so a hold can quit; let go sooner and the update runs, and the title starts as it always did.
void wait_press_key_in_title(CPU& c) {
    static bool told = false;
    if (!told) log("menu: the title screen is up (hold Minus to quit)"), told = true;
    static std::chrono::steady_clock::time_point since{};
    if (hid_buttons() == plat::kMinus) {
        auto now = std::chrono::steady_clock::now();
        if (since == std::chrono::steady_clock::time_point{}) since = now, log("menu: Minus held on the title screen");
        if (now - since >= kHold) quit(g_title.what);
        return;
    }
    since = {};
    g_title.game(c);
}

void install(Hook& h) {
    const uint64_t main_base = game_lib().main->base;  // (nx/gamelib.h: linked in, or the game code library)
    const LoadedModule* m = module_at(main_base);
    const uint64_t at = main_base + h.offset;
    for (int k = 0; k < 8; k++)
        if (ld<uint32_t>(at + 4 * k) != h.code[k]) {
            log("menu: main+%llx is not the 1.0.0 code: no Quit Game on the %s", (unsigned long long)h.offset, h.what);
            return;
        }
    h.game = m->table[h.offset / 4];
    if (!h.game) {
        log("menu: no function at main+%llx: no Quit Game on the %s", (unsigned long long)h.offset, h.what);
        return;
    }
    m->table[h.offset / 4] = h.replacement;
}

}  // namespace

void menu_init() {
    if (plat::setting("quit_game") == "0") return;  // NX_QUIT_GAME=0: the game's menus as they are
    install(g_pause);
    install(g_title);
}

}  // namespace nx::wonder
