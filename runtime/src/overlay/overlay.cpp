// The overlay's state, input queue, metrics and per-frame ImGui work (see overlay.h). The menu's tabs are in
// overlay_menu.cpp, the drawing in overlay_vk.cpp.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"

#include <nx/runtime.h>

#include "overlay/overlay_internal.h"

namespace nx::overlay {

namespace {

struct Event {
    enum Kind { KeyEv, Move, Button, Wheel, Text } kind;
    int raw = 0;
    Key key = KeyNone;
    bool down = false;
    float x = 0, y = 0;
    uint32_t ch = 0;
};

std::mutex g_lock;  // g_events, g_pending, g_host
std::vector<Event> g_events;
Host g_host;
bool g_has_pending = false;
Settings g_pending;   // from the platform (sync), taken by the next frame
Settings g_settings;  // the presenting thread's copy
std::atomic<bool> g_open{false};
std::atomic<bool> g_metrics{false};
std::atomic<int> g_input_frames{0};  // frames to run after input while closed (hover/close animations)

// The pad as the player holds it (before filter_pad clears it), for navigation.
std::atomic<uint64_t> g_pad_buttons{0};
std::atomic<int32_t> g_pad_lx{0}, g_pad_ly{0};
std::atomic<int> g_tab_request{-1};
uint64_t g_previous_buttons = 0;

int g_capture = -1;  // keyboard binding being captured
int g_tab = 0;

// Present intervals for the metrics panel (as platform_win.cpp's overlay).
constexpr size_t kHistory = 300;
std::mutex g_metrics_lock;
std::array<double, kHistory> g_intervals{};
size_t g_next = 0, g_count = 0;
uint64_t g_previous_ns = 0;
std::atomic<int> g_game_interval{1};

ImGuiKey imgui_key(Key k) {
    switch (k) {
        case KeyTab: return ImGuiKey_Tab;
        case KeyLeft: return ImGuiKey_LeftArrow;
        case KeyRight: return ImGuiKey_RightArrow;
        case KeyUp: return ImGuiKey_UpArrow;
        case KeyDown: return ImGuiKey_DownArrow;
        case KeyPageUp: return ImGuiKey_PageUp;
        case KeyPageDown: return ImGuiKey_PageDown;
        case KeyHome: return ImGuiKey_Home;
        case KeyEnd: return ImGuiKey_End;
        case KeyEnter: return ImGuiKey_Enter;
        case KeyEscape: return ImGuiKey_Escape;
        case KeyBackspace: return ImGuiKey_Backspace;
        case KeyDelete: return ImGuiKey_Delete;
        case KeySpace: return ImGuiKey_Space;
        case KeyShift: return ImGuiKey_LeftShift;
        case KeyCtrl: return ImGuiKey_LeftCtrl;
        default: return ImGuiKey_None;
    }
}

void push(const Event& e) {
    std::lock_guard lk(g_lock);
    if (g_events.size() < 4096) g_events.push_back(e);
}

// Fonts with the menu languages' Latin-1 letters; ImGui's built-in font has ASCII only.
const char* const kFonts[] = {
#if defined(_WIN32)
    "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
};
constexpr float kFontBake = 36.0f;  // baked large, drawn at kFontSize * scale through FontGlobalScale
constexpr float kFontSize = 18.0f;  // at 720p

void style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10, s.FrameRounding = 6, s.GrabRounding = 6, s.TabRounding = 6, s.PopupRounding = 6;
    s.WindowPadding = {16, 14}, s.FramePadding = {10, 6}, s.ItemSpacing = {10, 8};
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = {0.08f, 0.11f, 0.16f, 0.94f};
    c[ImGuiCol_PopupBg] = {0.10f, 0.13f, 0.19f, 0.98f};
    c[ImGuiCol_Border] = {0.33f, 0.44f, 0.59f, 0.6f};
    c[ImGuiCol_FrameBg] = {0.14f, 0.19f, 0.27f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = {0.20f, 0.28f, 0.40f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.24f, 0.34f, 0.48f, 1.0f};
    c[ImGuiCol_Button] = {0.18f, 0.26f, 0.38f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.26f, 0.38f, 0.55f, 1.0f};
    c[ImGuiCol_ButtonActive] = {0.30f, 0.45f, 0.65f, 1.0f};
    c[ImGuiCol_Tab] = {0.14f, 0.19f, 0.27f, 1.0f};
    c[ImGuiCol_TabHovered] = {0.30f, 0.45f, 0.65f, 1.0f};
    c[ImGuiCol_TabSelected] = {0.24f, 0.38f, 0.58f, 1.0f};
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = {0.35f, 0.75f, 1.0f, 1.0f};
    c[ImGuiCol_NavCursor] = {1.0f, 0.82f, 0.25f, 1.0f};
}

void draw_metrics(float scale) {
    std::array<float, kHistory> samples{};
    size_t count = 0;
    {
        std::lock_guard lk(g_metrics_lock);
        count = g_count;
        size_t start = (g_next + kHistory - count) % kHistory;
        for (size_t i = 0; i < count; ++i) samples[i] = float(g_intervals[(start + i) % kHistory]);
    }
    double sum = 0;
    size_t recent = std::min<size_t>(count, 60);
    for (size_t i = count - recent; i < count; ++i) sum += samples[i];
    double fps = sum > 0 ? double(recent) * 1000.0 / sum : 0.0;
    double low = 0;
    if (count) {
        std::vector<float> slow(samples.begin(), samples.begin() + long(count));
        std::sort(slow.begin(), slow.end(), std::greater<float>());
        size_t n = std::max<size_t>(1, (count + 99) / 100);
        double s = 0;
        for (size_t i = 0; i < n; ++i) s += slow[i];
        if (s > 0) low = double(n) * 1000.0 / s;
    }
    double speed = fps * double(std::max(1, g_game_interval.load())) / 60.0 * 100.0;
    const Settings& st = g_settings;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float m = 12 * scale;
    int pos = std::clamp(st.overlay_position, 0, 3);
    ImVec2 at{pos & 1 ? size.x - m : m, pos & 2 ? size.y - m : m};
    ImGui::SetNextWindowPos(at, ImGuiCond_Always, {pos & 1 ? 1.0f : 0.0f, pos & 2 ? 1.0f : 0.0f});
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##metrics", nullptr, f)) {
        if (st.metric_fps) ImGui::Text("FPS  %.1f", fps);
        if (st.metric_one_percent_low) ImGui::Text("1%% low  %.1f FPS", low);
        if (st.metric_game_speed) ImGui::Text("Game speed  %.1f%%", speed);
        if (st.metric_frametime_graph && count > 1) {
            ImGui::TextDisabled("Frametime (ms)");
            ImGui::PlotLines("##ft", samples.data(), int(count), 0, nullptr, 0.0f, 50.0f, {260 * scale, 80 * scale});
        }
    }
    ImGui::End();
}

// Controller navigation: the pad (Nintendo button meanings) as ImGui's gamepad keys. A confirms, B cancels.
void feed_pad(ImGuiIO& io) {
    uint64_t b = g_pad_buttons.load();
    int32_t lx = g_pad_lx.load(), ly = g_pad_ly.load();
    struct Map {
        uint64_t bit;
        ImGuiKey key;
    };
    static const Map map[] = {
        {plat::kA, ImGuiKey_GamepadFaceDown},     {plat::kB, ImGuiKey_GamepadFaceRight},
        {plat::kX, ImGuiKey_GamepadFaceUp},       {plat::kY, ImGuiKey_GamepadFaceLeft},
        {plat::kLeft, ImGuiKey_GamepadDpadLeft},  {plat::kRight, ImGuiKey_GamepadDpadRight},
        {plat::kUp, ImGuiKey_GamepadDpadUp},      {plat::kDown, ImGuiKey_GamepadDpadDown},
        {plat::kZL, ImGuiKey_GamepadL2},          {plat::kZR, ImGuiKey_GamepadR2},
    };
    for (const Map& m : map) io.AddKeyEvent(m.key, (b & m.bit) != 0);
    auto axis = [&](ImGuiKey neg, ImGuiKey posk, int32_t v) {
        float f = float(v) / float(plat::kStickMax);
        io.AddKeyAnalogEvent(neg, f < -0.3f, f < 0 ? -f : 0.0f);
        io.AddKeyAnalogEvent(posk, f > 0.3f, f > 0 ? f : 0.0f);
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
    axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, ly);  // Horizon: up is positive
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_IMGUI_MENU");
        return !(e && e[0] == '0');
    }();
    return on;
}

void init(const Host& host, const Settings& s) {
    std::lock_guard lk(g_lock);
    g_host = host;
    g_pending = s, g_has_pending = true;
    g_metrics = s.metrics_enabled;
}

void sync(const Settings& s) {
    std::lock_guard lk(g_lock);
    g_pending = s, g_has_pending = true;
    g_metrics = s.metrics_enabled;
}

void settings_changed(bool persist) {
    g_metrics = g_settings.metrics_enabled;
    Host h;
    {
        std::lock_guard lk(g_lock);
        h = g_host;
    }
    if (h.apply) h.apply(g_settings, persist);
}

void key_event(int raw, Key key, bool down) {
    if (key == KeyF1 && down) {
        toggle();
        return;
    }
    if (!g_open && key != KeyNone) return;
    push({Event::KeyEv, raw, key, down});
}
void mouse_move(float x, float y) {
    if (g_open) push({Event::Move, 0, KeyNone, false, x, y});
}
void mouse_button(int button, bool down) {
    if (g_open) push({Event::Button, button, KeyNone, down});
}
void mouse_wheel(float dy) {
    if (g_open) push({Event::Wheel, 0, KeyNone, false, 0, dy});
}
void text_input(uint32_t ch) {
    if (g_open) push({Event::Text, 0, KeyNone, false, 0, 0, ch});
}
void set_open(bool open, const char* why) {
    if (g_open.exchange(open) == open) return;
    g_input_frames = 30;
    log("overlay: menu %s (%s)", open ? "opened" : "closed", why);
}

void toggle() { set_open(!g_open.load(), "toggle"); }
bool is_open() { return g_open; }
bool wants_mouse() { return g_open; }
bool wants_keyboard() { return g_open; }

void filter_pad(plat::PadState& p) {
    if (!enabled()) return;
    g_pad_buttons = p.buttons, g_pad_lx = p.lx, g_pad_ly = p.ly;
    const uint64_t combo = plat::kPlus | plat::kMinus;
    bool combo_now = (p.buttons & combo) == combo, combo_before = (g_previous_buttons & combo) == combo;
    if (combo_now && !combo_before) toggle();
    if (g_open) {  // L / R switch tabs
        uint64_t pressed = p.buttons & ~g_previous_buttons;
        if (pressed & plat::kL) g_tab_request = (g_tab + menu_tab_count() - 1) % menu_tab_count();
        if (pressed & plat::kR) g_tab_request = (g_tab + 1) % menu_tab_count();
    }
    g_previous_buttons = p.buttons;
    if (g_open || combo_now) p = plat::PadState{};
}

void presented(uint64_t now_ns) {
    std::lock_guard lk(g_metrics_lock);
    if (g_previous_ns && now_ns > g_previous_ns) {
        double ms = double(now_ns - g_previous_ns) / 1e6;
        if (ms > 0 && ms < 10000) {
            g_intervals[g_next] = ms;
            g_next = (g_next + 1) % kHistory;
            g_count = std::min(kHistory, g_count + 1);
        }
    }
    g_previous_ns = now_ns;
}

void game_interval(int interval) { g_game_interval = std::max(1, interval); }

bool frame_wanted() { return g_open || g_metrics || g_input_frames > 0; }

void ensure_context() {
    if (ImGui::GetCurrentContext()) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "nx_overlay";
    style();
    for (const char* path : kFonts)
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            if (io.Fonts->AddFontFromFileTTF(path, kFontBake, nullptr, io.Fonts->GetGlyphRangesDefault())) {
                log("overlay: font %s", path);
                break;
            }
        }
    if (io.Fonts->Fonts.empty()) {
        ImFontConfig c;
        c.SizePixels = kFontBake;
        io.Fonts->AddFontDefault(&c);
    }
}

ImDrawData* build_frame(uint32_t w, uint32_t h) {
    ImGuiIO& io = ImGui::GetIO();
    static uint64_t last_ns = 0;
    uint64_t now = plat::monotonic_ns();
    io.DeltaTime = last_ns && now > last_ns ? std::min(float(double(now - last_ns) / 1e9), 0.25f) : 1.0f / 60.0f;
    last_ns = now;
    io.DisplaySize = {float(w), float(h)};
    const float scale = std::clamp(float(h) / 720.0f, 0.75f, 4.0f);
    io.FontGlobalScale = kFontSize * scale / kFontBake;

    std::vector<Event> events;
    {
        std::lock_guard lk(g_lock);
        events.swap(g_events);
        if (g_has_pending) g_settings = g_pending, g_has_pending = false;
    }
    bool changed = false;
    for (const Event& e : events) {
        switch (e.kind) {
            case Event::KeyEv:
                if (g_capture >= 0 && e.down) {  // the next key becomes the binding; Esc cancels
                    if (e.key != KeyEscape) g_settings.keyboard_bindings[size_t(g_capture)] = e.raw, changed = true;
                    g_capture = -1;
                    break;
                }
                if (e.key == KeyEscape && e.down && g_capture < 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
                    set_open(false, "Esc");
                    break;
                }
                if (ImGuiKey k = imgui_key(e.key); k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
                break;
            case Event::Move: io.AddMousePosEvent(e.x, e.y); break;
            case Event::Button: io.AddMouseButtonEvent(e.raw, e.down); break;
            case Event::Wheel: io.AddMouseWheelEvent(0, e.y); break;
            case Event::Text: io.AddInputCharacter(e.ch); break;
        }
    }
    if (changed) settings_changed(true);
    // B on a controller backs out: inside a dropdown or an edit ImGui handles it; at the top it closes the menu.
    static uint64_t previous_pad = 0;
    const uint64_t pad = g_pad_buttons.load();
    if (g_open && (pad & ~previous_pad & plat::kB) && g_capture < 0 && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
        set_open(false, "B");
    previous_pad = pad;
    feed_pad(io);
    if (g_input_frames > 0) g_input_frames--;

    ImGui::NewFrame();
    bool open = g_open;
    static bool was_open = false;
    const bool appearing = open && !was_open;
    was_open = open;
    if (g_settings.metrics_enabled) draw_metrics(scale);
    if (open) {
        MenuContext c{g_settings, g_host, g_capture, g_tab_request.exchange(-1), open, scale, appearing};
        draw_menu(c);
        if (c.tab_request >= 0 && c.tab_request != g_tab) log("overlay: tab %d", c.tab_request);
        if (c.tab_request >= 0) g_tab = c.tab_request;
        if (!open) set_open(false, "menu"), g_capture = -1;
    }
    ImGui::Render();
    return ImGui::GetDrawData();
}

}  // namespace nx::overlay
