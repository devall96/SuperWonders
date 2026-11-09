#pragma once

#include <windows.h>

namespace nx::plat {

struct PcMenuState {
    int width = 0;
    int height = 0;
    int internal_resolution = 0;
    int volume = 100;
    int language = 0;  // English, Italian, German, French
    int controller_preset = 0;  // Xbox / DualSense, Nintendo
    bool muted = false;
    bool fullscreen = false;
    bool always_on_top = false;
};

using PcMenuApply = void (*)(const PcMenuState&, bool persist);

void pc_menu_init(HWND owner, const PcMenuState& state, PcMenuApply apply);
void pc_menu_toggle();
bool pc_menu_open();
void pc_menu_sync(const PcMenuState& state);

}  // namespace nx::plat
