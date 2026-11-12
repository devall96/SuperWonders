// Linux input and player settings: the first SDL gamepad and the keyboard through the settings' bindings, the
// pc-settings.ini settings applied (window, volume, bindings, ...), and keys and mouse handed to the overlay menu
// (overlay/). Split from platform_linux.cpp (700-line rule).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include <nx/runtime.h>

#include "nvn/nvn_vk.h"
#include "overlay/overlay.h"
#include "overlay/settings.h"
#include "platform.h"
#include "platform_linux.h"
#include "svc_am.h"

namespace nx::plat {

namespace {

using linux_host::g_stream;
using linux_host::g_window;

std::mutex g_pad_lock;
PadState g_pad;

// Player settings (overlay/settings.h). Keyboard bindings are SDL scancodes. g_settings_lock guards g_settings; the
// bindings are copied into atomics for the input sampling.
std::mutex g_settings_lock;
overlay::Settings g_settings;
std::array<std::atomic<int>, overlay::kControlCount> g_key_binding{}, g_pad_binding{};
constexpr uint64_t kActionBits[overlay::kControlCount] = {
    kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kStickL, kStickR, kLeft, kUp, kRight, kDown,
};

std::atomic<bool> g_fast_forward{false};  // Tab held (sampled on the main thread)

std::mutex g_posted_lock;
std::deque<std::function<void()>> g_posted;

void post_to_main(std::function<void()> fn) {  // runs on the main thread soon; does not wait
    std::lock_guard lk(g_posted_lock);
    g_posted.push_back(std::move(fn));
}

std::string settings_path() { return data_root() + "/pc-settings.ini"; }

void apply_settings(const overlay::Settings& s, bool persist) {
    overlay::Settings before;
    {
        std::lock_guard lk(g_settings_lock);
        before = g_settings;
        g_settings = s;
    }
    for (size_t a = 0; a < overlay::kControlCount; ++a)
        g_key_binding[a] = s.keyboard_bindings[a], g_pad_binding[a] = overlay::controller_binding(s, a);
    if (g_stream) {
        float v = float(std::clamp(s.volume, 0, 100)) / 100.0f;
        SDL_SetAudioStreamGain(g_stream, s.muted ? 0.0f : v * v);
    }
    nx::set_internal_resolution_preset(s.internal_resolution);  // read at the next boot
    nx::set_game_language(s.game_language);
    set_memory_limit_gb(s.memory_limit_gb);
    setenv("NX_LOW_VRAM_PROFILE", s.low_vram_profile ? "1" : "0", 1);
    nvn::set_fsr(s.upscaler == 1, float(s.fsr_sharpness) / 100.0f);
    if (s.fullscreen != before.fullscreen || s.width != before.width || s.height != before.height ||
        s.always_on_top != before.always_on_top)
        post_to_main([s] {
            if (!g_window) return;
            SDL_SetWindowFullscreen(g_window, s.fullscreen);
            if (!s.fullscreen) SDL_SetWindowSize(g_window, s.width, s.height);
            SDL_SetWindowAlwaysOnTop(g_window, s.always_on_top);
        });
    if (persist) overlay::save_settings(settings_path(), s);
}

const char* key_name(int scancode) {
    const char* n = SDL_GetScancodeName(SDL_Scancode(scancode));
    return n && *n ? n : "-";
}

// Keys the overlay understands, from SDL keycodes.
overlay::Key menu_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_TAB: return overlay::KeyTab;
        case SDLK_LEFT: return overlay::KeyLeft;
        case SDLK_RIGHT: return overlay::KeyRight;
        case SDLK_UP: return overlay::KeyUp;
        case SDLK_DOWN: return overlay::KeyDown;
        case SDLK_PAGEUP: return overlay::KeyPageUp;
        case SDLK_PAGEDOWN: return overlay::KeyPageDown;
        case SDLK_HOME: return overlay::KeyHome;
        case SDLK_END: return overlay::KeyEnd;
        case SDLK_RETURN: case SDLK_KP_ENTER: return overlay::KeyEnter;
        case SDLK_ESCAPE: return overlay::KeyEscape;
        case SDLK_BACKSPACE: return overlay::KeyBackspace;
        case SDLK_DELETE: return overlay::KeyDelete;
        case SDLK_SPACE: return overlay::KeySpace;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return overlay::KeyShift;
        case SDLK_LCTRL: case SDLK_RCTRL: return overlay::KeyCtrl;
        case SDLK_F1: return overlay::KeyF1;
        default: return overlay::KeyNone;
    }
}

}  // namespace

bool fast_forward() { return g_fast_forward.load(std::memory_order_relaxed); }

void read_pad(PadState& p) {
    {
        std::lock_guard lk(g_pad_lock);
        p.buttons |= g_pad.buttons;
        if (g_pad.lx) p.lx = g_pad.lx;
        if (g_pad.ly) p.ly = g_pad.ly;
        if (g_pad.rx) p.rx = g_pad.rx;
        if (g_pad.ry) p.ry = g_pad.ry;
    }
    input_script(p);
    overlay::filter_pad(p);  // controller navigation of the menu; the game sees nothing while it is open
}

namespace linux_host {

void initial_window(int& w, int& h, bool& fullscreen) {
    std::lock_guard lk(g_settings_lock);
    w = g_settings.width, h = g_settings.height, fullscreen = g_settings.fullscreen;
}

void load_player_settings() {
    overlay::Settings s;
    const int keys[overlay::kControlCount] = {
        SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_Q, SDL_SCANCODE_E,
        SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_M, SDL_SCANCODE_N,
        SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    };
    for (size_t a = 0; a < overlay::kControlCount; ++a)
        s.keyboard_bindings[a] = keys[a], s.controller_bindings[a] = overlay::default_controller_binding(a, s.controller_preset);
    bool found = overlay::load_settings(settings_path(), s);
    if (const char* e = std::getenv("NX_MEM_LIMIT_GB"); e && std::atof(e) > 0) s.memory_limit_gb = std::atoi(e);
    apply_settings(s, false);
    overlay::Host host;
    host.apply = apply_settings;
    host.key_name = key_name;
    // Wayland has no "always on top" for clients; X11 does.
    const char* driver = SDL_GetCurrentVideoDriver();
    host.can_always_on_top = !driver || std::strcmp(driver, "wayland") != 0;
    overlay::init(host, s);
    log("platform: settings %s (%s)", found ? "loaded" : "defaults", settings_path().c_str());
}

void run_posted() {
    std::deque<std::function<void()>> posted;
    {
        std::lock_guard lk(g_posted_lock);
        posted.swap(g_posted);
    }
    for (auto& fn : posted) fn();
}

bool handle_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (down && e.key.scancode == SDL_SCANCODE_F11 && g_window) {
                bool full = !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(g_window, full);
                std::lock_guard lk(g_settings_lock);
                g_settings.fullscreen = full;
                overlay::sync(g_settings);
            }
            if (overlay::enabled() && !e.key.repeat) overlay::key_event(int(e.key.scancode), menu_key(e.key.key), down);
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (overlay::wants_mouse() && g_window) {
                float d = SDL_GetWindowPixelDensity(g_window);
                overlay::mouse_move(e.motion.x * d, e.motion.y * d);
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (overlay::wants_mouse()) {
                int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                overlay::mouse_button(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            if (overlay::wants_mouse()) overlay::mouse_wheel(e.wheel.y);
            return true;
        default:
            return false;
    }
}

void sample_input() {
    PadState p;
    static SDL_Gamepad* pad = nullptr;
    if (!pad) {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }
    if (pad) {
        // Physical buttons by position (overlay::Physical order), then each action takes its bound button
        // (the menu's preset or remapping; the default preset puts A at the bottom, as on Windows).
        static const SDL_GamepadButton physical[] = {
            SDL_GAMEPAD_BUTTON_SOUTH,       SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_NORTH,       SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_INVALID,     SDL_GAMEPAD_BUTTON_INVALID,  // the triggers are axes
            SDL_GAMEPAD_BUTTON_START,       SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_LEFT_STICK,
            SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_DPAD_UP,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT,  SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        };
        bool down[overlay::kControlCount] = {};
        for (size_t k = 0; k < overlay::kControlCount; ++k)
            down[k] = physical[k] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad, physical[k]);
        down[overlay::PhyTriggerL] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000;
        down[overlay::PhyTriggerR] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000;
        for (size_t a = 0; a < overlay::kControlCount; ++a)
            if (down[std::clamp(g_pad_binding[a].load(), 0, int(overlay::kControlCount) - 1)]) p.buttons |= kActionBits[a];
        auto axis = [&](SDL_GamepadAxis a, bool invert) {
            int v = SDL_GetGamepadAxis(pad, a);
            if (v > -7849 && v < 7849) return 0;
            v = std::clamp(v, -kStickMax, kStickMax);
            return invert ? -v : v;
        };
        p.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
        p.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);  // SDL: down is positive; Horizon: up is positive
        p.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
        p.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
    }
    // Keyboard: WASD / arrows move the stick; the 16 actions follow their bindings (defaults: Space A, Shift B, J X,
    // K Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -, M/N stick clicks, arrows D-pad). Not while the menu has the keys.
    bool tab = false;
    if (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) && !overlay::wants_keyboard()) {
        int count = 0;
        const bool* k = SDL_GetKeyboardState(&count);
        tab = k[SDL_SCANCODE_TAB];  // fast-forward (hle_vi.cpp)
        auto held = [&](int sc) { return sc > 0 && sc < count && k[sc]; };
        if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) p.lx = -kStickMax;
        if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) p.lx = kStickMax;
        if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) p.ly = kStickMax;
        if (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) p.ly = -kStickMax;
        for (size_t a = 0; a < overlay::kControlCount; ++a)
            if (held(g_key_binding[a].load())) p.buttons |= kActionBits[a];
        if (k[SDL_SCANCODE_RSHIFT] && g_key_binding[overlay::CtlB].load() == SDL_SCANCODE_LSHIFT) p.buttons |= kB;
    }
    g_fast_forward = tab;
    std::lock_guard lk(g_pad_lock);
    g_pad = p;
}

}  // namespace linux_host

}  // namespace nx::plat
