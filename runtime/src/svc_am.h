#pragma once

namespace nx {

// Preset selected in the Windows quick settings; zero leaves the game's default mode unchanged.
void set_internal_resolution_preset(int preset);
float internal_resolution_scale();
uint32_t internal_resolution_target_width();
void set_game_language(int language);

}  // namespace nx
