#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace nx::plat {

constexpr size_t kPcControlCount = 16;

enum PcControl : size_t {
    PcA, PcB, PcX, PcY, PcL, PcR, PcZL, PcZR, PcPlus, PcMinus, PcLStick, PcRStick,
    PcDpadLeft, PcDpadUp, PcDpadRight, PcDpadDown,
};

enum PcPhysicalButton : int {
    PcSouth, PcEast, PcWest, PcNorth, PcShoulderL, PcShoulderR, PcTriggerL, PcTriggerR,
    PcStart, PcBack, PcStickL, PcStickR, PcLeft, PcUp, PcRight, PcDown,
};

inline int pc_default_controller_binding(size_t action, int preset) {
    constexpr int standard[kPcControlCount] = {
        PcSouth, PcEast, PcWest, PcNorth, PcShoulderL, PcShoulderR, PcTriggerL, PcTriggerR,
        PcStart, PcBack, PcStickL, PcStickR, PcLeft, PcUp, PcRight, PcDown,
    };
    constexpr int nintendo[kPcControlCount] = {
        PcEast, PcSouth, PcNorth, PcWest, PcShoulderL, PcShoulderR, PcTriggerL, PcTriggerR,
        PcStart, PcBack, PcStickL, PcStickR, PcLeft, PcUp, PcRight, PcDown,
    };
    return action < kPcControlCount ? (preset == 1 ? nintendo[action] : standard[action]) : PcSouth;
}

struct PcMenuState {
    int width = 0;
    int height = 0;
    int internal_resolution = 0;
    int volume = 100;
    int language = 0;  // English, Italian, German, French
    int game_language = 0;  // Language returned to the game at startup
    int memory_limit_gb = 8;  // Process private-commit guard; not a dedicated VRAM budget
    int controller_preset = 0;  // Xbox / DualSense, Nintendo
    bool muted = false;
    bool fullscreen = false;
    bool always_on_top = false;
    bool metrics_enabled = false;
    bool low_vram_profile = false;  // ASTC textures use BC3 instead of expanded RGBA8 where ASTC is unavailable
    bool metric_fps = true;
    bool metric_one_percent_low = true;
    bool metric_game_speed = true;
    bool metric_frametime_graph = true;
    int overlay_position = 0;  // top left, top right, bottom left, bottom right
    int upscaler = 1;          // overlay menu: 0 bilinear, 1 FSR 1
    int fsr_sharpness = 25;    // overlay menu: RCAS stops x 100
    int antialiasing = 0;      // overlay menu: 0 off, 1 FXAA
    int anisotropy = 0;        // overlay menu: 0 the game's, else 2..16
    int rumble = 100;          // overlay menu: rumble strength, percent
    std::array<int, kPcControlCount> keyboard_bindings = {
        VK_SPACE, VK_SHIFT, 'J', 'K', 'Q', 'E', 'Z', 'C', VK_RETURN, VK_ESCAPE, 'M', 'N',
        VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN,
    };
    std::array<int, kPcControlCount> controller_bindings{};
    uint32_t controller_custom_mask = 0;
};

using PcMenuApply = void (*)(const PcMenuState&, bool persist);

void pc_menu_init(HWND owner, const PcMenuState& state, PcMenuApply apply);
void pc_menu_toggle();
bool pc_menu_open();
void pc_menu_sync(const PcMenuState& state);
void pc_menu_capture_controller_button(int physical_button);  // may be called by the game input thread

}  // namespace nx::plat
