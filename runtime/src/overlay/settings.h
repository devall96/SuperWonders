// Player settings shared by every PC platform: what the F1 menu shows and changes, kept in <data root>/pc-settings.ini
// (the same file and keys as the Win32 menu's, pc_menu_win.cpp, so either menu reads the other's file). Key bindings
// are platform key codes: Windows virtual keys, SDL scancodes on Linux.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nx::overlay {

constexpr size_t kControlCount = 16;

// The 16 NX actions, in pad bit order of the bindings arrays.
enum Control : size_t {
    CtlA, CtlB, CtlX, CtlY, CtlL, CtlR, CtlZL, CtlZR, CtlPlus, CtlMinus, CtlLStick, CtlRStick,
    CtlLeft, CtlUp, CtlRight, CtlDown,
};

// Physical controller buttons, by position (SDL's south/east/west/north naming).
enum Physical : int {
    PhySouth, PhyEast, PhyWest, PhyNorth, PhyShoulderL, PhyShoulderR, PhyTriggerL, PhyTriggerR,
    PhyStart, PhyBack, PhyStickL, PhyStickR, PhyLeft, PhyUp, PhyRight, PhyDown,
};

struct Settings {
    int width = 1280, height = 720;
    int internal_resolution = 0;  // svc_am.cpp presets: 0 original/dynamic, then 720p, 900p, 1080p, 1440p
    int volume = 100;
    int language = 0;       // menu language: English, Italian, German, French
    int game_language = 0;  // language reported to the game at startup
    int memory_limit_gb = 8;
    int controller_preset = 1;  // 1 Nintendo positions (the default, as on the Switch: A on the right), 0 Xbox / DualSense
    bool muted = false, fullscreen = false, always_on_top = false;
    bool metrics_enabled = false, low_vram_profile = false;
    bool metric_fps = true, metric_one_percent_low = true, metric_game_speed = true, metric_frametime_graph = true;
    int overlay_position = 0;  // top left, top right, bottom left, bottom right
    int upscaler = 1;          // 0 bilinear, 1 FSR 1 (when the game's image is smaller than the window)
    int fsr_sharpness = 25;    // RCAS sharpness in hundredths of a stop: 0 the sharpest, 200 the softest
    int antialiasing = 0;      // 0 off, 1 FXAA (on the presented image)
    int anisotropy = 0;        // 0 the game's own, else 2, 4, 8, 16 (applies after a restart)
    std::array<int, kControlCount> keyboard_bindings{};
    std::array<int, kControlCount> controller_bindings{};
    uint32_t controller_custom_mask = 0;  // bit per action: controller_bindings[action] instead of the preset
};

// The physical button a preset gives an action.
int default_controller_binding(size_t action, int preset);
// The physical button an action is bound to now.
int controller_binding(const Settings& s, size_t action);

// Reads `path` over `s` (missing or invalid keys keep their value). False when there is no file.
bool load_settings(const std::string& path, Settings& s);
void save_settings(const std::string& path, const Settings& s);

}  // namespace nx::overlay
