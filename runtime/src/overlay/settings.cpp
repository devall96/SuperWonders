#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace nx::overlay {

int default_controller_binding(size_t action, int preset) {
    constexpr int standard[kControlCount] = {
        PhySouth, PhyEast, PhyWest, PhyNorth, PhyShoulderL, PhyShoulderR, PhyTriggerL, PhyTriggerR,
        PhyStart, PhyBack, PhyStickL, PhyStickR, PhyLeft, PhyUp, PhyRight, PhyDown,
    };
    constexpr int nintendo[kControlCount] = {
        PhyEast, PhySouth, PhyNorth, PhyWest, PhyShoulderL, PhyShoulderR, PhyTriggerL, PhyTriggerR,
        PhyStart, PhyBack, PhyStickL, PhyStickR, PhyLeft, PhyUp, PhyRight, PhyDown,
    };
    return action < kControlCount ? (preset == 1 ? nintendo[action] : standard[action]) : PhySouth;
}

int controller_binding(const Settings& s, size_t action) {
    if (action >= kControlCount) return PhySouth;
    return s.controller_custom_mask & (1u << action) ? std::clamp(s.controller_bindings[action], 0, int(kControlCount) - 1)
                                                     : default_controller_binding(action, s.controller_preset);
}

bool load_settings(const std::string& path, Settings& s) {
    std::ifstream file(path);
    if (!file) return false;
    std::string line;
    while (std::getline(file, line)) {
        int v = 0;
        unsigned u = 0;
        const char* l = line.c_str();
        if (std::sscanf(l, "width=%d", &v) == 1 && v >= 640 && v <= 7680) s.width = v;
        else if (std::sscanf(l, "height=%d", &v) == 1 && v >= 360 && v <= 4320) s.height = v;
        else if (std::sscanf(l, "internal_resolution=%d", &v) == 1) s.internal_resolution = std::clamp(v, 0, 6);
        else if (std::sscanf(l, "volume=%d", &v) == 1) s.volume = std::clamp(v, 0, 100);
        else if (std::sscanf(l, "muted=%d", &v) == 1) s.muted = v != 0;
        else if (std::sscanf(l, "fullscreen=%d", &v) == 1) s.fullscreen = v != 0;
        else if (std::sscanf(l, "always_on_top=%d", &v) == 1) s.always_on_top = v != 0;
        else if (std::sscanf(l, "language=%d", &v) == 1) s.language = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "game_language=%d", &v) == 1) s.game_language = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "memory_limit_gb=%d", &v) == 1) s.memory_limit_gb = std::clamp(v, 2, 32);
        else if (std::sscanf(l, "low_vram_profile=%d", &v) == 1) s.low_vram_profile = v != 0;
        else if (std::sscanf(l, "controller_preset=%d", &v) == 1) s.controller_preset = std::clamp(v, 0, 1);
        else if (std::sscanf(l, "metrics_enabled=%d", &v) == 1) s.metrics_enabled = v != 0;
        else if (std::sscanf(l, "metric_fps=%d", &v) == 1) s.metric_fps = v != 0;
        else if (std::sscanf(l, "metric_one_percent_low=%d", &v) == 1) s.metric_one_percent_low = v != 0;
        else if (std::sscanf(l, "metric_game_speed=%d", &v) == 1) s.metric_game_speed = v != 0;
        else if (std::sscanf(l, "metric_frametime_graph=%d", &v) == 1) s.metric_frametime_graph = v != 0;
        else if (std::sscanf(l, "overlay_position=%d", &v) == 1) s.overlay_position = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "upscaler=%d", &v) == 1) s.upscaler = std::clamp(v, 0, 1);
        else if (std::sscanf(l, "fsr_sharpness=%d", &v) == 1) s.fsr_sharpness = std::clamp(v, 0, 200);
        else if (std::sscanf(l, "antialiasing=%d", &v) == 1) s.antialiasing = std::clamp(v, 0, 1);
        else if (std::sscanf(l, "anisotropy=%d", &v) == 1) s.anisotropy = std::clamp(v, 0, 16);
        else if (std::sscanf(l, "rumble=%d", &v) == 1) s.rumble = std::clamp(v, 0, 100);
        else if (std::sscanf(l, "vsync=%d", &v) == 1) s.vsync = std::clamp(v, 0, 3);
        else if (std::sscanf(l, "frame_cap=%d", &v) == 1) s.frame_cap = v == 30 ? 30 : 60;
        else if (std::sscanf(l, "controller_custom_mask=%u", &u) == 1) s.controller_custom_mask = u;
        else {
            int index = -1, binding = -1;
            if (std::sscanf(l, "keyboard_%d=%d", &index, &binding) == 2 && index >= 0 && index < int(kControlCount) &&
                binding >= 0 && binding <= 511)
                s.keyboard_bindings[size_t(index)] = binding;
            else if (std::sscanf(l, "controller_%d=%d", &index, &binding) == 2 && index >= 0 &&
                     index < int(kControlCount) && binding >= 0 && binding < int(kControlCount))
                s.controller_bindings[size_t(index)] = binding;
        }
    }
    return true;
}

void save_settings(const std::string& path, const Settings& s) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << "width=" << s.width << "\nheight=" << s.height << "\nvolume=" << s.volume
      << "\ninternal_resolution=" << s.internal_resolution << "\nmuted=" << s.muted << "\nfullscreen=" << s.fullscreen
      << "\nalways_on_top=" << s.always_on_top << "\nlanguage=" << s.language << "\ngame_language=" << s.game_language
      << "\nmemory_limit_gb=" << s.memory_limit_gb << "\nlow_vram_profile=" << s.low_vram_profile
      << "\ncontroller_preset=" << s.controller_preset << "\nmetrics_enabled=" << s.metrics_enabled
      << "\nmetric_fps=" << s.metric_fps << "\nmetric_one_percent_low=" << s.metric_one_percent_low
      << "\nmetric_game_speed=" << s.metric_game_speed << "\nmetric_frametime_graph=" << s.metric_frametime_graph
      << "\noverlay_position=" << s.overlay_position << "\ncontroller_custom_mask=" << s.controller_custom_mask
      << "\nupscaler=" << s.upscaler << "\nfsr_sharpness=" << s.fsr_sharpness << "\nantialiasing=" << s.antialiasing
      << "\nanisotropy=" << s.anisotropy << "\nrumble=" << s.rumble << "\nvsync=" << s.vsync
      << "\nframe_cap=" << s.frame_cap << '\n';
    for (size_t i = 0; i < kControlCount; ++i)
        f << "keyboard_" << i << '=' << s.keyboard_bindings[i] << '\n'
          << "controller_" << i << '=' << s.controller_bindings[i] << '\n';
}

}  // namespace nx::overlay
