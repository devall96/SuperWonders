// Between platform_linux.cpp (memory, threads, window, audio, Vulkan, main) and platform_linux_input.cpp (gamepad
// and keyboard, the player settings and the overlay menu's input).
#pragma once

#include <SDL3/SDL.h>

namespace nx::plat::linux_host {

extern SDL_Window* g_window;        // made on the main thread (create_window)
extern SDL_AudioStream* g_stream;   // null when muted or headless

// Main thread.
void load_player_settings();  // after SDL_Init: pc-settings.ini, applied, handed to the overlay
void sample_input();          // every loop: the pad snapshot read_pad() copies
bool handle_event(const SDL_Event& e);  // keys and mouse (F11, the overlay menu); true when used
void run_posted();            // settings work posted by other threads (window size, fullscreen)
// The window's first size and fullscreen state from the settings.
void initial_window(int& w, int& h, bool& fullscreen);

}  // namespace nx::plat::linux_host
