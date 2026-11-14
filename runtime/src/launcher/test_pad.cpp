// A scripted controller for tests (launcher/test_pad.h).
#include "launcher/test_pad.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <nx/runtime.h>

namespace nx::launcher {

namespace {

struct Press {
    uint64_t at, until;
    int button;  // SDL_GamepadButton, or -1..-4: the left stick left, right, up, down
};
std::vector<Press> g_script;
SDL_Joystick* g_pad = nullptr;

int button_of(const std::string& n) {
    static const struct {
        const char* name;
        int b;
    } names[] = {
        {"south", SDL_GAMEPAD_BUTTON_SOUTH},       {"east", SDL_GAMEPAD_BUTTON_EAST},
        {"west", SDL_GAMEPAD_BUTTON_WEST},         {"north", SDL_GAMEPAD_BUTTON_NORTH},
        {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},        {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},    {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"l", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},   {"r", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"start", SDL_GAMEPAD_BUTTON_START},       {"back", SDL_GAMEPAD_BUTTON_BACK},
        {"zl", 100},                               {"zr", 101},
        {"stick-left", -1},                        {"stick-right", -2},
        {"stick-up", -3},                          {"stick-down", -4},
    };
    for (const auto& e : names)
        if (n == e.name) return e.b;
    return -100;
}

}  // namespace

void test_pad_start() {
    const char* s = std::getenv("NX_TEST_PAD");
    if (!s || !*s) return;
    std::string all = s;
    for (size_t at = 0; at < all.size();) {
        size_t end = all.find(';', at);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(at, end - at);
        at = end + 1;
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        const size_t c2 = item.find(':', c1 + 1);
        const uint64_t frame = std::strtoull(item.c_str(), nullptr, 10);
        const std::string name = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        const uint64_t held = c2 == std::string::npos ? 6 : std::strtoull(item.c_str() + c2 + 1, nullptr, 10);
        const int b = button_of(name);
        if (b == -100) {
            log("test pad: unknown button '%s'", name.c_str());
            continue;
        }
        g_script.push_back({frame, frame + held, b});
    }
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "SuperWonders test pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    g_pad = id ? SDL_OpenJoystick(id) : nullptr;
    log("test pad: %s, %zu presses", g_pad ? "attached" : SDL_GetError(), g_script.size());
}

void test_pad_frame(uint64_t frame) {
    if (!g_pad) return;
    bool buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
    int lx = 0, ly = 0;
    bool zl = false, zr = false;
    for (const Press& p : g_script) {
        if (frame < p.at || frame >= p.until) continue;
        if (p.button >= 0 && p.button < SDL_GAMEPAD_BUTTON_COUNT) buttons[p.button] = true;
        else if (p.button == 100) zl = true;
        else if (p.button == 101) zr = true;
        else if (p.button == -1) lx = -32767;
        else if (p.button == -2) lx = 32767;
        else if (p.button == -3) ly = -32767;  // (SDL: up is negative)
        else if (p.button == -4) ly = 32767;
        if (frame == p.at) log("test pad: frame %llu press %d", (unsigned long long)frame, p.button);
    }
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) SDL_SetJoystickVirtualButton(g_pad, b, buttons[b]);
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX, Sint16(lx));
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY, Sint16(ly));
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, zl ? 32767 : 0);
    SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, zr ? 32767 : 0);
    SDL_UpdateJoysticks();
}

}  // namespace nx::launcher
