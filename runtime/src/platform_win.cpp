// Windows host: memory, threads, time, files, the game window (F11 / Alt+Enter fullscreen), XInput/SDL gamepads + keyboard,
// XAudio2, the Vulkan loader and win32 surface, crash handler, hardware watchpoints (NX_WATCH) and main().
#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// SetProcessDpiAwarenessContext and per-monitor-v2 awareness are Win10 APIs.
// The bundled LLVM-MinGW headers hide their declarations unless the target
// Windows version is explicit (unlike the user's Visual Studio environment).
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <xaudio2.h>
#include <xinput.h>
#include <SDL3/SDL.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "nvn/nvn_vk.h"
#include "overlay/overlay.h"
#include "pc_menu_win.h"
#include "svc_am.h"
#include "platform.h"

namespace nx {
HWND g_window = nullptr;
void boot();
}  // namespace nx

namespace nx::plat {

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    return VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return VirtualAlloc(reinterpret_cast<void*>(a), e - a, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void decommit(uint64_t addr, uint64_t size) { VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT); }

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static DWORD WINAPI thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return 0;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    CreateThread(nullptr, stack_size, thread_entry, new ThreadStart{fn, arg}, STACK_SIZE_PARAM_IS_A_RESERVATION,
                 nullptr);
}

void exit_thread() { ExitThread(0); }
void exit_process(int code) { nx::nvn::flush_pipeline_cache(); ExitProcess(UINT(code)); }
void sleep_ns(int64_t ns) {
    // A high-resolution waitable timer (Windows 10 1803+): Sleep rounds to the system tick (1 to 15.6 ms), and the
    // 60 Hz pacing jittered by that much.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100);  // relative, in 100 ns
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        return;
    }
    WaitForSingleObject(timer, INFINITE);
}

// D3DKMTWaitForVerticalBlankEvent on the adapter output that shows the game window (gdi32, no WDK header).
bool wait_vblank() {
    struct OpenFromHdc { HDC hdc; UINT adapter; LUID luid; UINT source; };
    struct Blank { UINT adapter; UINT device; UINT source; };
    using OpenFn = LONG(APIENTRY*)(OpenFromHdc*);
    using WaitFn = LONG(APIENTRY*)(const Blank*);
    static const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    static const auto open = gdi ? reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc")) : nullptr;
    static const auto wait = gdi ? reinterpret_cast<WaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
    static const bool timer = std::getenv("NX_TIMER_VSYNC") != nullptr;  // the 60 Hz timer instead (comparing)
    static HMONITOR monitor = nullptr;
    static Blank blank{};
    static int per_tick = 0;  // blanks per 1/60 s; 0: not paced on the display
    if (timer || !open || !wait || !g_window) return false;
    // shortcut: the adapter handle of a monitor the window left is not closed (moving monitors is rare).
    if (HMONITOR m = MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY); m != monitor) {
        monitor = m, per_tick = 0;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        int hz = 0;
        if (GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            hz = int(dm.dmDisplayFrequency);  // 59.94 Hz reads 59 or 60
            int n = (hz + 30) / 60;
            if (n >= 1 && std::abs(hz - 60 * n) <= 1)
                if (HDC dc = CreateDCW(nullptr, mi.szDevice, nullptr, nullptr)) {
                    OpenFromHdc o{dc};
                    if (open(&o) == 0) blank = {o.adapter, 0, o.source}, per_tick = n;
                    DeleteDC(dc);
                }
        }
        log("display: %d Hz, the game paced on %s", hz, per_tick ? "its vertical blank" : "a 60 Hz timer");
    }
    for (int k = 0; k < per_tick; k++)
        if (wait(&blank) != 0) return per_tick = 0, false;
    return per_tick != 0;
}
void yield() { SwitchToThread(); }
void set_thread_name(const char*) {}
float default_render_scale() { return 1.0f; }
void release_window(void*) {}
uint64_t window_generation() { return 1; }
void audio_pause(bool) {}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}
int64_t current_thread_id() { return int64_t(GetCurrentThreadId()); }
bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }

uint64_t monotonic_ns() {
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(double(t.QuadPart) * 1e9 / double(f.QuadPart));
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) {
    std::wstring wp = std::filesystem::u8path(path).wstring();
    std::wstring wm(mode, mode + std::strlen(mode));
    return _wfopen(wp.c_str(), wm.c_str());
}
int64_t ftell64(FILE* f) { return _ftelli64(f); }
int fseek64(FILE* f, int64_t off, int origin) { return _fseeki64(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    _lock_file(f);  // seek + read as one step for this FILE
    _fseeki64_nolock(f, off, SEEK_SET);
    uint64_t n = _fread_nolock(buf, 1, size, f);
    _unlock_file(f);
    return n;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    if (r && *r) return r;

    // A directly launched build has no batch file to set NX_DATA_ROOT. Keep game data
    // next to the executable so it works regardless of the process working directory.
    std::wstring module_path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size()) return nx::game::kInfo.pc_data_root;
    module_path.resize(length);
    const auto root = (std::filesystem::path(module_path).parent_path() / L"data").u8string();
    return std::string(reinterpret_cast<const char*>(root.data()), root.size());
}

static PcMenuState g_pc_settings;
static std::atomic<IXAudio2MasteringVoice*> g_master_voice{nullptr};
static std::array<std::atomic<int>, kPcControlCount> g_keyboard_bindings{};
static std::array<std::atomic<int>, kPcControlCount> g_controller_bindings{};
static std::atomic<HWND> g_metrics_window{nullptr};
static std::atomic<bool> g_metrics_enabled{false}, g_metric_fps{true}, g_metric_one_percent_low{true};
static std::atomic<bool> g_metric_game_speed{true}, g_metric_frametime_graph{true};
static std::atomic<int> g_metrics_position{0}, g_game_present_interval{1};
static constexpr size_t kFrameHistory = 300;
static std::mutex g_metrics_mutex;
static std::array<double, kFrameHistory> g_frame_intervals_ms{};
static size_t g_frame_interval_next = 0, g_frame_interval_count = 0;
static uint64_t g_previous_present_ns = 0;
static constexpr uint64_t kPadActionBits[kPcControlCount] = {
    kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kStickL, kStickR, kLeft, kUp, kRight, kDown,
};

static void metrics_reposition();

// ---- the Dear ImGui overlay menu (overlay/; NX_IMGUI_MENU=0: the Win32 menu and metrics window as before) ----------

static overlay::Settings to_overlay(const PcMenuState& s) {
    overlay::Settings o;
    o.width = s.width, o.height = s.height, o.internal_resolution = s.internal_resolution, o.volume = s.volume;
    o.language = s.language, o.game_language = s.game_language, o.memory_limit_gb = s.memory_limit_gb;
    o.controller_preset = s.controller_preset, o.muted = s.muted, o.fullscreen = s.fullscreen;
    o.always_on_top = s.always_on_top, o.metrics_enabled = s.metrics_enabled, o.low_vram_profile = s.low_vram_profile;
    o.metric_fps = s.metric_fps, o.metric_one_percent_low = s.metric_one_percent_low;
    o.metric_game_speed = s.metric_game_speed, o.metric_frametime_graph = s.metric_frametime_graph;
    o.overlay_position = s.overlay_position, o.controller_custom_mask = s.controller_custom_mask;
    o.upscaler = s.upscaler, o.fsr_sharpness = s.fsr_sharpness, o.antialiasing = s.antialiasing;
    o.anisotropy = s.anisotropy;
    for (size_t i = 0; i < kPcControlCount; ++i)
        o.keyboard_bindings[i] = s.keyboard_bindings[i], o.controller_bindings[i] = s.controller_bindings[i];
    return o;
}

static PcMenuState from_overlay(const overlay::Settings& o) {
    PcMenuState s = g_pc_settings;
    s.width = o.width, s.height = o.height, s.internal_resolution = o.internal_resolution, s.volume = o.volume;
    s.language = o.language, s.game_language = o.game_language, s.memory_limit_gb = o.memory_limit_gb;
    s.controller_preset = o.controller_preset, s.muted = o.muted, s.fullscreen = o.fullscreen;
    s.always_on_top = o.always_on_top, s.metrics_enabled = o.metrics_enabled, s.low_vram_profile = o.low_vram_profile;
    s.metric_fps = o.metric_fps, s.metric_one_percent_low = o.metric_one_percent_low;
    s.metric_game_speed = o.metric_game_speed, s.metric_frametime_graph = o.metric_frametime_graph;
    s.overlay_position = o.overlay_position, s.controller_custom_mask = o.controller_custom_mask;
    s.upscaler = o.upscaler, s.fsr_sharpness = o.fsr_sharpness, s.antialiasing = o.antialiasing;
    s.anisotropy = o.anisotropy;
    for (size_t i = 0; i < kPcControlCount; ++i)
        s.keyboard_bindings[i] = o.keyboard_bindings[i], s.controller_bindings[i] = o.controller_bindings[i];
    return s;
}

// The overlay applies on the presenting thread; window changes belong to the window thread, so they go there.
constexpr UINT kOverlayApplyMessage = WM_APP + 60;
struct OverlayApply {
    PcMenuState state;
    bool persist;
};

static void overlay_apply(const overlay::Settings& s, bool persist) {
    auto* a = new OverlayApply{from_overlay(s), persist};
    if (!g_window || !PostMessageW(g_window, kOverlayApplyMessage, 0, reinterpret_cast<LPARAM>(a))) delete a;
}

static const char* overlay_key_name(int vk) {
    thread_local char name[64];
    UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
    LONG l = LONG(scan) << 16;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_INSERT || vk == VK_DELETE ||
        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_RCONTROL || vk == VK_RMENU)
        l |= 1 << 24;  // extended keys
    if (GetKeyNameTextA(l, name, sizeof name) > 0) return name;
    std::snprintf(name, sizeof name, "VK %d", vk);
    return name;
}

static overlay::Key overlay_key(WPARAM vk) {
    switch (vk) {
        case VK_TAB: return overlay::KeyTab;
        case VK_LEFT: return overlay::KeyLeft;
        case VK_RIGHT: return overlay::KeyRight;
        case VK_UP: return overlay::KeyUp;
        case VK_DOWN: return overlay::KeyDown;
        case VK_PRIOR: return overlay::KeyPageUp;
        case VK_NEXT: return overlay::KeyPageDown;
        case VK_HOME: return overlay::KeyHome;
        case VK_END: return overlay::KeyEnd;
        case VK_RETURN: return overlay::KeyEnter;
        case VK_ESCAPE: return overlay::KeyEscape;
        case VK_BACK: return overlay::KeyBackspace;
        case VK_DELETE: return overlay::KeyDelete;
        case VK_SPACE: return overlay::KeySpace;
        case VK_SHIFT: return overlay::KeyShift;
        case VK_CONTROL: return overlay::KeyCtrl;
        default: return overlay::KeyNone;
    }
}

static bool metrics_has_content() {
    return g_metrics_enabled.load(std::memory_order_relaxed) &&
           (g_metric_fps.load(std::memory_order_relaxed) || g_metric_one_percent_low.load(std::memory_order_relaxed) ||
            g_metric_game_speed.load(std::memory_order_relaxed) ||
            g_metric_frametime_graph.load(std::memory_order_relaxed));
}

static void apply_metrics_settings(const PcMenuState& state) {
    g_metrics_enabled.store(state.metrics_enabled && !overlay::enabled(), std::memory_order_relaxed);
    g_metric_fps.store(state.metric_fps, std::memory_order_relaxed);
    g_metric_one_percent_low.store(state.metric_one_percent_low, std::memory_order_relaxed);
    g_metric_game_speed.store(state.metric_game_speed, std::memory_order_relaxed);
    g_metric_frametime_graph.store(state.metric_frametime_graph, std::memory_order_relaxed);
    g_metrics_position.store(std::clamp(state.overlay_position, 0, 3), std::memory_order_relaxed);
    metrics_reposition();
}

static void apply_input_bindings(const PcMenuState& state) {
    for (size_t i = 0; i < kPcControlCount; ++i) {
        g_keyboard_bindings[i].store(std::clamp(state.keyboard_bindings[i], 0, 255), std::memory_order_relaxed);
        const int controller = state.controller_custom_mask & (1u << i)
                                   ? state.controller_bindings[i]
                                   : pc_default_controller_binding(i, state.controller_preset);
        g_controller_bindings[i].store(std::clamp(controller, 0, int(kPcControlCount) - 1),
                                       std::memory_order_relaxed);
    }
}

static void save_pc_settings() {
    std::ofstream file(data_root() + "/pc-settings.ini", std::ios::trunc);
    if (!file) return;
    file << "width=" << g_pc_settings.width << "\nheight=" << g_pc_settings.height << "\nvolume=" << g_pc_settings.volume
         << "\ninternal_resolution=" << g_pc_settings.internal_resolution << "\nmuted=" << g_pc_settings.muted << "\nfullscreen=" << g_pc_settings.fullscreen
         << "\nalways_on_top=" << g_pc_settings.always_on_top << "\nlanguage=" << g_pc_settings.language
         << "\ngame_language=" << g_pc_settings.game_language
         << "\nmemory_limit_gb=" << g_pc_settings.memory_limit_gb
         << "\nlow_vram_profile=" << g_pc_settings.low_vram_profile
         << "\ncontroller_preset=" << g_pc_settings.controller_preset
         << "\nmetrics_enabled=" << g_pc_settings.metrics_enabled << "\nmetric_fps=" << g_pc_settings.metric_fps
         << "\nmetric_one_percent_low=" << g_pc_settings.metric_one_percent_low
         << "\nmetric_game_speed=" << g_pc_settings.metric_game_speed
         << "\nmetric_frametime_graph=" << g_pc_settings.metric_frametime_graph
         << "\noverlay_position=" << g_pc_settings.overlay_position
         << "\ncontroller_custom_mask=" << g_pc_settings.controller_custom_mask
         << "\nupscaler=" << g_pc_settings.upscaler << "\nfsr_sharpness=" << g_pc_settings.fsr_sharpness
         << "\nantialiasing=" << g_pc_settings.antialiasing << "\nanisotropy=" << g_pc_settings.anisotropy << '\n';
    for (size_t i = 0; i < kPcControlCount; ++i)
        file << "keyboard_" << i << '=' << g_pc_settings.keyboard_bindings[i] << '\n'
             << "controller_" << i << '=' << g_pc_settings.controller_bindings[i] << '\n';
}

static void load_pc_settings() {
    std::ifstream file(data_root() + "/pc-settings.ini");
    std::string line;
    while (std::getline(file, line)) {
        int value = 0;
        if (std::sscanf(line.c_str(), "width=%d", &value) == 1 && value >= 640 && value <= 7680) g_pc_settings.width = value;
        else if (std::sscanf(line.c_str(), "height=%d", &value) == 1 && value >= 360 && value <= 4320) g_pc_settings.height = value;
        else if (std::sscanf(line.c_str(), "internal_resolution=%d", &value) == 1) g_pc_settings.internal_resolution = std::clamp(value, 0, 4);
        else if (std::sscanf(line.c_str(), "volume=%d", &value) == 1) g_pc_settings.volume = std::clamp(value, 0, 100);
        else if (std::sscanf(line.c_str(), "muted=%d", &value) == 1) g_pc_settings.muted = value != 0;
        else if (std::sscanf(line.c_str(), "fullscreen=%d", &value) == 1) g_pc_settings.fullscreen = value != 0;
        else if (std::sscanf(line.c_str(), "always_on_top=%d", &value) == 1) g_pc_settings.always_on_top = value != 0;
        else if (std::sscanf(line.c_str(), "language=%d", &value) == 1) g_pc_settings.language = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "game_language=%d", &value) == 1) g_pc_settings.game_language = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "memory_limit_gb=%d", &value) == 1) g_pc_settings.memory_limit_gb = std::clamp(value, 2, 32);
        else if (std::sscanf(line.c_str(), "low_vram_profile=%d", &value) == 1) g_pc_settings.low_vram_profile = value != 0;
        else if (std::sscanf(line.c_str(), "controller_preset=%d", &value) == 1)
            g_pc_settings.controller_preset = std::clamp(value, 0, 1);
        else if (std::sscanf(line.c_str(), "metrics_enabled=%d", &value) == 1) g_pc_settings.metrics_enabled = value != 0;
        else if (std::sscanf(line.c_str(), "metric_fps=%d", &value) == 1) g_pc_settings.metric_fps = value != 0;
        else if (std::sscanf(line.c_str(), "metric_one_percent_low=%d", &value) == 1)
            g_pc_settings.metric_one_percent_low = value != 0;
        else if (std::sscanf(line.c_str(), "metric_game_speed=%d", &value) == 1)
            g_pc_settings.metric_game_speed = value != 0;
        else if (std::sscanf(line.c_str(), "metric_frametime_graph=%d", &value) == 1)
            g_pc_settings.metric_frametime_graph = value != 0;
        else if (std::sscanf(line.c_str(), "overlay_position=%d", &value) == 1)
            g_pc_settings.overlay_position = std::clamp(value, 0, 3);
        else if (std::sscanf(line.c_str(), "controller_custom_mask=%u", &g_pc_settings.controller_custom_mask) == 1) {}
        else if (std::sscanf(line.c_str(), "upscaler=%d", &value) == 1) g_pc_settings.upscaler = std::clamp(value, 0, 1);
        else if (std::sscanf(line.c_str(), "fsr_sharpness=%d", &value) == 1)
            g_pc_settings.fsr_sharpness = std::clamp(value, 0, 200);
        else if (std::sscanf(line.c_str(), "antialiasing=%d", &value) == 1)
            g_pc_settings.antialiasing = std::clamp(value, 0, 1);
        else if (std::sscanf(line.c_str(), "anisotropy=%d", &value) == 1)
            g_pc_settings.anisotropy = std::clamp(value, 0, 16);
        else {
            int index = -1, binding = -1;
            if (std::sscanf(line.c_str(), "keyboard_%d=%d", &index, &binding) == 2 &&
                index >= 0 && index < int(kPcControlCount) && binding >= 0 && binding <= 255)
                g_pc_settings.keyboard_bindings[size_t(index)] = binding;
            else if (std::sscanf(line.c_str(), "controller_%d=%d", &index, &binding) == 2 &&
                     index >= 0 && index < int(kPcControlCount) && binding >= 0 && binding < int(kPcControlCount))
                g_pc_settings.controller_bindings[size_t(index)] = binding;
        }
    }
    nx::set_internal_resolution_preset(g_pc_settings.internal_resolution);
    nx::set_game_language(g_pc_settings.game_language);
    apply_input_bindings(g_pc_settings);
    set_memory_limit_gb(g_pc_settings.memory_limit_gb);
    _putenv_s("NX_LOW_VRAM_PROFILE", g_pc_settings.low_vram_profile ? "1" : "0");
    nx::nvn::set_fsr(g_pc_settings.upscaler == 1, float(g_pc_settings.fsr_sharpness) / 100.0f);
    nx::nvn::set_fxaa(g_pc_settings.antialiasing == 1);
    nx::nvn::set_anisotropy(g_pc_settings.anisotropy);
}

static void set_game_volume() {
    if (IXAudio2MasteringVoice* voice = g_master_voice.load()) {
        const float volume = float(std::clamp(g_pc_settings.volume, 0, 100)) / 100.0f;
        voice->SetVolume(g_pc_settings.muted ? 0.0f : volume * volume, XAUDIO2_COMMIT_NOW);
    }
}

// ---- window ---------------------------------------------------------------------------------------------------

static std::wstring widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }  // ASCII titles
// The game renders its swapchain at the render resolution; the window can be any size (the compositor scales).

static constexpr DWORD kWindowed = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};
static std::atomic<uint64_t> g_shader_compiled{0}, g_shader_total{0};
static std::atomic<bool> g_shader_progress_visible{false};
static void toggle_fullscreen(HWND h);

static void resize_window_client(HWND h, int width, int height) {
    if (!h || g_fullscreen) return;
    RECT r{0, 0, width, height};
    const DWORD style = DWORD(GetWindowLongPtrW(h, GWL_STYLE));
    const DWORD ex_style = DWORD(GetWindowLongPtrW(h, GWL_EXSTYLE));
    AdjustWindowRectEx(&r, style, FALSE, ex_style);
    RECT current{};
    GetWindowRect(h, &current);
    const int outer_w = r.right - r.left, outer_h = r.bottom - r.top;
    const int x = current.left + ((current.right - current.left) - outer_w) / 2;
    const int y = current.top + ((current.bottom - current.top) - outer_h) / 2;
    SetWindowPos(h, nullptr, std::max(0, x), std::max(0, y), outer_w, outer_h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static int metrics_overlay_height(bool graph, int rows) { return 42 + rows * 23 + (graph ? 92 : 0); }

static void metrics_paint(HWND h) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT client{};
    GetClientRect(h, &client);
    HBRUSH key = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &client, key);
    DeleteObject(key);

    const bool show_fps = g_metric_fps.load(std::memory_order_relaxed);
    const bool show_low = g_metric_one_percent_low.load(std::memory_order_relaxed);
    const bool show_speed = g_metric_game_speed.load(std::memory_order_relaxed);
    const bool show_graph = g_metric_frametime_graph.load(std::memory_order_relaxed);
    const int rows = int(show_fps) + int(show_low) + int(show_speed);
    RECT panel{2, 2, client.right - 2, client.bottom - 2};
    HBRUSH panel_brush = CreateSolidBrush(RGB(20, 27, 40));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(83, 112, 151));
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, border);
    RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 12, 12);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(border);

    std::array<double, kFrameHistory> samples{};
    size_t count = 0;
    {
        std::lock_guard lock(g_metrics_mutex);
        count = g_frame_interval_count;
        const size_t start = (g_frame_interval_next + kFrameHistory - count) % kFrameHistory;
        for (size_t i = 0; i < count; ++i) samples[i] = g_frame_intervals_ms[(start + i) % kFrameHistory];
    }
    double sum = 0.0;
    const size_t recent = std::min<size_t>(count, 60);
    for (size_t i = count - recent; i < count; ++i) sum += samples[i];
    const double fps = sum > 0.0 ? double(recent) * 1000.0 / sum : 0.0;
    double one_low = 0.0;
    if (count) {
        std::vector<double> slow(samples.begin(), samples.begin() + count);
        std::sort(slow.begin(), slow.end(), std::greater<double>());
        const size_t slow_count = std::max<size_t>(1, (count + 99) / 100);
        double slow_sum = 0.0;
        for (size_t i = 0; i < slow_count; ++i) slow_sum += slow[i];
        if (slow_sum > 0.0) one_low = double(slow_count) * 1000.0 / slow_sum;
    }
    const int target_interval = std::max(1, g_game_present_interval.load(std::memory_order_relaxed));
    const double speed = fps * double(target_interval) / 60.0 * 100.0;

    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, RGB(235, 242, 253));
    int y = 9;
    wchar_t line[96];
    auto draw_metric = [&](const wchar_t* name, double value, const wchar_t* unit, int precision) {
        if (precision == 0) swprintf_s(line, L"%s  %.0f%s", name, value, unit);
        else swprintf_s(line, L"%s  %.1f%s", name, value, unit);
        RECT text{14, y, client.right - 12, y + 22};
        DrawTextW(dc, line, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 23;
    };
    if (show_fps) draw_metric(L"FPS", fps, L"", 1);
    if (show_low) draw_metric(L"1% low", one_low, L" FPS", 1);
    if (show_speed) draw_metric(L"Game speed", speed, L"%", 1);
    SelectObject(dc, old_font);
    DeleteObject(font);

    if (show_graph) {
        SetTextColor(dc, RGB(170, 190, 218));
        RECT graph_title{14, y + 1, client.right - 12, y + 19};
        DrawTextW(dc, L"Frametime (ms)", -1, &graph_title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT graph{14, y + 23, client.right - 12, client.bottom - 10};
        HBRUSH graph_brush = CreateSolidBrush(RGB(31, 40, 56));
        FillRect(dc, &graph, graph_brush);
        DeleteObject(graph_brush);
        HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(62, 74, 95));
        HGDIOBJ previous_pen = SelectObject(dc, grid_pen);
        for (int row = 1; row < 4; ++row) {
            const int gy = graph.top + (graph.bottom - graph.top) * row / 4;
            MoveToEx(dc, graph.left, gy, nullptr);
            LineTo(dc, graph.right, gy);
        }
        SelectObject(dc, previous_pen);
        DeleteObject(grid_pen);
        if (count > 1) {
            std::vector<POINT> points;
            points.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                const int x = graph.left + int((graph.right - graph.left - 1) * i / (count - 1));
                const double ms = std::clamp(samples[i], 0.0, 50.0);
                const int py = graph.bottom - 1 - int((graph.bottom - graph.top - 1) * ms / 50.0);
                points.push_back({x, py});
            }
            HPEN line_pen = CreatePen(PS_SOLID, 2, RGB(89, 190, 255));
            HGDIOBJ previous = SelectObject(dc, line_pen);
            Polyline(dc, points.data(), int(points.size()));
            SelectObject(dc, previous);
            DeleteObject(line_pen);
        }
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK metrics_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT:
            metrics_paint(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void metrics_init(HWND owner) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = metrics_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WonderRecompMetricsOverlay";
    RegisterClassW(&wc);
    HWND overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   wc.lpszClassName, L"Wonder Metrics", WS_POPUP, 0, 0, 320, 180, owner, nullptr,
                                   wc.hInstance, nullptr);
    if (overlay) {
        SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), 230, LWA_COLORKEY | LWA_ALPHA);
        g_metrics_window.store(overlay, std::memory_order_release);
    }
}

static void metrics_reposition() {
    HWND overlay = g_metrics_window.load(std::memory_order_acquire);
    HWND owner = nx::g_window;
    if (!overlay || !owner) return;
    if (!metrics_has_content()) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }
    const bool show_graph = g_metric_frametime_graph.load(std::memory_order_relaxed);
    const int rows = int(g_metric_fps.load(std::memory_order_relaxed)) +
                     int(g_metric_one_percent_low.load(std::memory_order_relaxed)) +
                     int(g_metric_game_speed.load(std::memory_order_relaxed));
    const int width = 320, height = metrics_overlay_height(show_graph, rows);
    RECT client{};
    GetClientRect(owner, &client);
    POINT origin{0, 0};
    ClientToScreen(owner, &origin);
    const int position = g_metrics_position.load(std::memory_order_relaxed);
    const bool right = position == 1 || position == 3;
    const bool bottom = position == 2 || position == 3;
    const int x = origin.x + (right ? std::max(12L, client.right - width - 16) : 16);
    const int y = origin.y + (bottom ? std::max(12L, client.bottom - height - 16) : 16);
    SetWindowPos(overlay, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(overlay, nullptr, FALSE);
}

static void apply_pc_menu(const PcMenuState& state, bool persist) {
    const bool size_changed = state.width != g_pc_settings.width || state.height != g_pc_settings.height;
    const bool topmost_changed = state.always_on_top != g_pc_settings.always_on_top;
    g_pc_settings = state;
    g_pc_settings.width = std::clamp(g_pc_settings.width, 640, 7680);
    g_pc_settings.height = std::clamp(g_pc_settings.height, 360, 4320);
    g_pc_settings.volume = std::clamp(g_pc_settings.volume, 0, 100);
    g_pc_settings.internal_resolution = std::clamp(g_pc_settings.internal_resolution, 0, 4);
    g_pc_settings.language = std::clamp(g_pc_settings.language, 0, 3);
    g_pc_settings.game_language = std::clamp(g_pc_settings.game_language, 0, 3);
    g_pc_settings.memory_limit_gb = std::clamp(g_pc_settings.memory_limit_gb, 2, 32);
    g_pc_settings.controller_preset = std::clamp(g_pc_settings.controller_preset, 0, 1);
    g_pc_settings.overlay_position = std::clamp(g_pc_settings.overlay_position, 0, 3);
    apply_input_bindings(g_pc_settings);
    nx::set_game_language(g_pc_settings.game_language);
    apply_metrics_settings(g_pc_settings);
    set_memory_limit_gb(g_pc_settings.memory_limit_gb);
    _putenv_s("NX_LOW_VRAM_PROFILE", g_pc_settings.low_vram_profile ? "1" : "0");
    nx::nvn::set_fsr(g_pc_settings.upscaler == 1, float(g_pc_settings.fsr_sharpness) / 100.0f);
    nx::nvn::set_fxaa(g_pc_settings.antialiasing == 1);
    nx::nvn::set_anisotropy(g_pc_settings.anisotropy);
    if (g_fullscreen != g_pc_settings.fullscreen) toggle_fullscreen(g_window);
    else if (size_changed && !g_fullscreen) resize_window_client(g_window, g_pc_settings.width, g_pc_settings.height);
    if (topmost_changed && g_window)
        SetWindowPos(g_window, g_pc_settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    set_game_volume();
    if (persist) save_pc_settings();
    pc_menu_sync(g_pc_settings);
}

static void toggle_fullscreen(HWND h) {
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        GetWindowPlacement(h, &g_placement);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, kWindowed);
        SetWindowPlacement(h, &g_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
    SetWindowPos(h, g_pc_settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_pc_settings.fullscreen = g_fullscreen;
    if (!g_fullscreen) resize_window_client(h, g_pc_settings.width, g_pc_settings.height);
    pc_menu_sync(g_pc_settings);
    if (overlay::enabled()) overlay::sync(to_overlay(g_pc_settings));
    save_pc_settings();
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == kOverlayApplyMessage) {
        std::unique_ptr<OverlayApply> a(reinterpret_cast<OverlayApply*>(l));
        apply_pc_menu(a->state, a->persist);
        return 0;
    }
    if (overlay::enabled() && overlay::is_open()) {
        switch (msg) {
            case WM_KEYDOWN:
            case WM_KEYUP:
                if (w == VK_F1 || w == VK_F11) break;  // handled below
                if (msg == WM_KEYUP || !(l & (1ll << 30))) overlay::key_event(int(w), overlay_key(w), msg == WM_KEYDOWN);
                return 0;
            case WM_CHAR:
                overlay::text_input(uint32_t(w));
                return 0;
            case WM_MOUSEMOVE:
                overlay::mouse_move(float(GET_X_LPARAM(l)), float(GET_Y_LPARAM(l)));
                return 0;
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                overlay::mouse_button(0, msg == WM_LBUTTONDOWN);
                return 0;
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                overlay::mouse_button(1, msg == WM_RBUTTONDOWN);
                return 0;
            case WM_MOUSEWHEEL:
                overlay::mouse_wheel(float(GET_WHEEL_DELTA_WPARAM(w)) / float(WHEEL_DELTA));
                return 0;
        }
    }
    switch (msg) {
        case WM_MOVE:
        case WM_SIZE:
            metrics_reposition();
            break;
        case WM_PAINT: {
            if (!g_shader_progress_visible.load(std::memory_order_acquire))
                return DefWindowProcW(h, msg, w, l);
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT client{};
            GetClientRect(h, &client);
            HBRUSH background = CreateSolidBrush(RGB(17, 22, 32));
            FillRect(dc, &client, background);
            DeleteObject(background);
            const int width = int(std::max(1L, client.right - client.left));
            const int height = int(std::max(1L, client.bottom - client.top));
            const int left = std::max(32, width / 8);
            const int right = width - left;
            const int center_y = height / 2;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(245, 247, 250));
            HFONT font = CreateFontW(-MulDiv(22, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_SEMIBOLD,
                                     FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HGDIOBJ old_font = SelectObject(dc, font);
            RECT title{left, center_y - 62, right, center_y - 25};
            DrawTextW(dc, L"Compiling shaders", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            wchar_t count[96];
            const uint64_t compiled = g_shader_compiled.load(std::memory_order_relaxed);
            const uint64_t total = g_shader_total.load(std::memory_order_relaxed);
            swprintf_s(count, L"%llu / %llu compiled", (unsigned long long)compiled, (unsigned long long)total);
            SetTextColor(dc, RGB(188, 198, 214));
            RECT label{left, center_y - 15, right, center_y + 15};
            DrawTextW(dc, count, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old_font);
            DeleteObject(font);

            RECT bar{left, center_y + 30, right, center_y + 43};
            HBRUSH track = CreateSolidBrush(RGB(49, 59, 74));
            FillRect(dc, &bar, track);
            DeleteObject(track);
            if (total) {
                const uint64_t bounded = std::min(compiled, total);
                bar.right = bar.left + int64_t(bar.right - bar.left) * bounded / total;
                HBRUSH fill = CreateSolidBrush(RGB(54, 139, 235));
                FillRect(dc, &bar, fill);
                DeleteObject(fill);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CLOSE:
            log("window closed");
            nx::nvn::flush_pipeline_cache();
            ExitProcess(0);
        case WM_KEYDOWN:
            if (w == VK_F1 && !(l & (1ll << 30))) {
                if (overlay::enabled())
                    overlay::toggle();
                else
                    pc_menu_toggle();
                return 0;
            }
            if (w == VK_F11) toggle_fullscreen(h);
            break;
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN) {  // Alt+Enter
                toggle_fullscreen(h);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (w == VK_RETURN) return 0;  // no beep
            break;
    }
    return DefWindowProcW(h, msg, w, l);
}

// The window lives on its own thread so its message loop keeps running whatever the game threads do.
static void window_thread(std::atomic<bool>* ready) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    const std::wstring cls = widen(nx::game::kInfo.title) + L"Recomp";  // tools/capture.ps1 finds the window by it
    wc.lpszClassName = cls.c_str();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // The render resolution's aspect (NX_RESOLUTION, default 16:9), about 80% of the screen height, centred.
    unsigned rw = 16, rh = 9;
    if (const char* r = std::getenv("NX_RESOLUTION")) std::sscanf(r, "%ux%u", &rw, &rh);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ch = std::max(540, sh * 8 / 10), cw = int(int64_t(ch) * rw / std::max(1u, rh));
    if (g_pc_settings.width > 0 && g_pc_settings.height > 0) cw = g_pc_settings.width, ch = g_pc_settings.height;
    RECT r{0, 0, cw, ch};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (sh - h) / 2;
    // NX_HEADLESS: created hidden (the surface still needs a window), never brought to the front.
    const DWORD style = headless() ? (kWindowed & ~WS_VISIBLE) : kWindowed;
    g_window = CreateWindowW(wc.lpszClassName, (widen(nx::game::kInfo.title) + L" (Switch recomp)  -  F11: fullscreen").c_str(), style,
                             std::max(0, x), std::max(0, y), w, h, nullptr, nullptr, wc.hInstance, nullptr);
    const char* fullscreen_env = std::getenv("NX_FULLSCREEN");
    if (!headless() && (g_pc_settings.fullscreen || (fullscreen_env && *fullscreen_env == '1'))) toggle_fullscreen(g_window);
    RECT client{};
    GetClientRect(g_window, &client);
    if (!g_pc_settings.width) g_pc_settings.width = client.right - client.left;
    if (!g_pc_settings.height) g_pc_settings.height = client.bottom - client.top;
    if (g_pc_settings.always_on_top)
        SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pc_menu_init(g_window, g_pc_settings, apply_pc_menu);
    if (overlay::enabled()) {
        overlay::Host host;
        host.apply = overlay_apply;
        host.key_name = overlay_key_name;
        overlay::init(host, to_overlay(g_pc_settings));
    }
    metrics_init(g_window);
    apply_metrics_settings(g_pc_settings);
    if (!headless()) SetForegroundWindow(g_window);
    *ready = true;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void* create_window() {
    if (!g_window) {
        static std::atomic<bool> ready{false};
        std::thread(window_thread, &ready).detach();
        while (!ready) Sleep(1);
    }
    return g_window;
}

bool window_minimized() { return g_window && IsIconic(g_window); }

void shader_compile_progress(uint64_t compiled, uint64_t total, bool visible) {
    g_shader_compiled.store(compiled, std::memory_order_relaxed);
    g_shader_total.store(total, std::memory_order_relaxed);
    g_shader_progress_visible.store(visible, std::memory_order_release);
    if (g_window) InvalidateRect(g_window, nullptr, TRUE);
}

void overlay_game_interval(int interval) {
    g_game_present_interval.store(std::max(1, interval), std::memory_order_relaxed);
    overlay::game_interval(interval);
}

void overlay_presented(uint64_t now_ns) {
    overlay::presented(now_ns);
    {
        std::lock_guard lock(g_metrics_mutex);
        if (g_previous_present_ns && now_ns > g_previous_present_ns) {
            const double interval_ms = double(now_ns - g_previous_present_ns) / 1e6;
            if (interval_ms > 0.0 && interval_ms < 10000.0) {
                g_frame_intervals_ms[g_frame_interval_next] = interval_ms;
                g_frame_interval_next = (g_frame_interval_next + 1) % kFrameHistory;
                g_frame_interval_count = std::min(kFrameHistory, g_frame_interval_count + 1);
            }
        }
        g_previous_present_ns = now_ns;
    }
    if (metrics_has_content()) {
        if (HWND overlay = g_metrics_window.load(std::memory_order_acquire)) InvalidateRect(overlay, nullptr, FALSE);
    }
}

void window_size(uint32_t& w, uint32_t& h) {
    RECT r{};
    if (g_window) GetClientRect(g_window, &r);
    w = uint32_t(r.right - r.left);
    h = uint32_t(r.bottom - r.top);
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }

// ---- input: XInput / SDL gamepad + per-action keyboard bindings -----------------------------------------------

static bool key(int vk) {
    return vk > 0 && g_window && GetForegroundWindow() == g_window && (GetAsyncKeyState(vk) & 0x8000);
}
static std::array<bool, kPcControlCount> g_previous_controller_state{};

bool fast_forward() { return key(VK_TAB); }

bool headless() {  // NX_HEADLESS (platform.h)
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void read_pad(PadState& p) {
    std::array<bool, kPcControlCount> physical{};
    XINPUT_STATE xs{};
    bool have_xinput = XInputGetState(0, &xs) == ERROR_SUCCESS;
    if (have_xinput) {
        const auto& g = xs.Gamepad;
        physical[PcSouth] = (g.wButtons & XINPUT_GAMEPAD_A) != 0;
        physical[PcEast] = (g.wButtons & XINPUT_GAMEPAD_B) != 0;
        physical[PcWest] = (g.wButtons & XINPUT_GAMEPAD_X) != 0;
        physical[PcNorth] = (g.wButtons & XINPUT_GAMEPAD_Y) != 0;
        physical[PcShoulderL] = (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        physical[PcShoulderR] = (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        physical[PcTriggerL] = g.bLeftTrigger > 100;
        physical[PcTriggerR] = g.bRightTrigger > 100;
        physical[PcStart] = (g.wButtons & XINPUT_GAMEPAD_START) != 0;
        physical[PcBack] = (g.wButtons & XINPUT_GAMEPAD_BACK) != 0;
        physical[PcStickL] = (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        physical[PcStickR] = (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        physical[PcLeft] = (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        physical[PcUp] = (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
        physical[PcRight] = (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        physical[PcDown] = (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        auto dz = [](int v) { return (v > -7849 && v < 7849) ? 0 : std::max(-kStickMax, std::min(kStickMax, v)); };
        p.lx = dz(g.sThumbLX);
        p.ly = dz(g.sThumbLY);
        p.rx = dz(g.sThumbRX);
        p.ry = dz(g.sThumbRY);
    }
    // SDL's HIDAPI backend reads DualSense directly; keep XInput as the first-choice path for Xbox pads.
    if (!have_xinput) {
        SDL_UpdateGamepads();
        static SDL_Gamepad* gamepad = nullptr;
        if (gamepad && !SDL_GamepadConnected(gamepad)) {
            SDL_CloseGamepad(gamepad);
            gamepad = nullptr;
        }
        if (!gamepad) {
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            SDL_Gamepad* fallback = nullptr;
            for (int i = 0; ids && i < count; ++i) {
                SDL_Gamepad* candidate = SDL_OpenGamepad(ids[i]);
                if (!candidate) continue;
                if (SDL_GetGamepadType(candidate) == SDL_GAMEPAD_TYPE_PS5) {
                    if (fallback) SDL_CloseGamepad(fallback);
                    gamepad = candidate;
                    break;
                }
                if (!fallback) fallback = candidate;
                else SDL_CloseGamepad(candidate);
            }
            SDL_free(ids);
            if (!gamepad) gamepad = fallback;
        }
        if (gamepad) {
            auto down = [](SDL_Gamepad* pad, SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
            physical[PcSouth] = down(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
            physical[PcEast] = down(gamepad, SDL_GAMEPAD_BUTTON_EAST);
            physical[PcWest] = down(gamepad, SDL_GAMEPAD_BUTTON_WEST);
            physical[PcNorth] = down(gamepad, SDL_GAMEPAD_BUTTON_NORTH);
            physical[PcShoulderL] = down(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            physical[PcShoulderR] = down(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            physical[PcTriggerL] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 10000;
            physical[PcTriggerR] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 10000;
            physical[PcStart] = down(gamepad, SDL_GAMEPAD_BUTTON_START);
            physical[PcBack] = down(gamepad, SDL_GAMEPAD_BUTTON_BACK);
            physical[PcStickL] = down(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
            physical[PcStickR] = down(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            physical[PcLeft] = down(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
            physical[PcUp] = down(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
            physical[PcRight] = down(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            physical[PcDown] = down(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
            auto dz = [](int v) { return (v > -7849 && v < 7849) ? 0 : std::max(-kStickMax, std::min(kStickMax, v)); };
            p.lx = dz(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = dz(-SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = dz(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = dz(-SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }

    PadState nav{};  // the controller through its bindings, for the overlay's navigation
    nav.lx = p.lx, nav.ly = p.ly, nav.rx = p.rx, nav.ry = p.ry;
    for (size_t action = 0; action < kPcControlCount; ++action) {
        const int source = g_controller_bindings[action].load(std::memory_order_relaxed);
        if (source >= 0 && source < int(physical.size()) && physical[size_t(source)]) nav.buttons |= kPadActionBits[action];
    }
    const bool menu_open = pc_menu_open() || (overlay::enabled() && overlay::is_open());
    if (pc_menu_open()) {
        for (size_t i = 0; i < physical.size(); ++i)
            if (physical[i] && !g_previous_controller_state[i]) pc_menu_capture_controller_button(int(i));
    } else {
        for (size_t action = 0; action < kPcControlCount; ++action) {
            const int source = g_controller_bindings[action].load(std::memory_order_relaxed);
            if (source >= 0 && source < int(physical.size()) && physical[size_t(source)])
                p.buttons |= kPadActionBits[action];
            const int vk = g_keyboard_bindings[action].load(std::memory_order_relaxed);
            if (key(vk)) {
                p.buttons |= kPadActionBits[action];
                if (action == PcDpadLeft) p.lx = -kStickMax;
                else if (action == PcDpadRight) p.lx = kStickMax;
                else if (action == PcDpadUp) p.ly = kStickMax;
                else if (action == PcDpadDown) p.ly = -kStickMax;
            }
        }
        // WASD remains a convenient independent movement option in addition to remappable D-pad keys.
        if (key('A')) p.lx = -kStickMax;
        if (key('D')) p.lx = kStickMax;
        if (key('W')) p.ly = kStickMax;
        if (key('S')) p.ly = -kStickMax;
    }
    g_previous_controller_state = physical;
    if (menu_open) p = {};
    input_script(p);  // NX_INPUT_SCRIPT (input_script.cpp): unattended runs
    if (overlay::enabled()) {
        PadState view = nav;
        view.buttons |= p.buttons;
        const bool had = view.buttons != 0;
        overlay::filter_pad(view);  // Minus + Plus toggles; while open the menu takes the pad
        if (overlay::is_open() || (had && view.buttons == 0)) p = {};
    }
}

// ---- audio: XAudio2 -------------------------------------------------------------------------------------------

namespace {

std::atomic<uint32_t> g_queued{0};
AudioDone g_done = nullptr;

struct Voice : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
        auto* p = static_cast<std::pair<void*, std::vector<uint8_t>>*>(ctx);
        g_queued--;
        g_done(p->first);
        delete p;
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};

IXAudio2* g_xa = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
Voice g_callback;

}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(g_xa->CreateMasteringVoice(&master))) return false;
    g_master_voice = master;
    set_game_volume();
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 48000 * 4;
    if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wf, 0, 2.0f, &g_callback))) return false;
    g_voice->Start();
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_voice) return false;
    auto* p = new std::pair<void*, std::vector<uint8_t>>(
        ctx, std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes));
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = UINT32(bytes);
    xb.pAudioData = p->second.data();
    xb.pContext = p;
    g_queued++;
    g_voice->SubmitSourceBuffer(&xb);
    return true;
}

uint32_t audio_queued() { return g_queued; }

// ---- Vulkan ---------------------------------------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    return m ? reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr")) : nullptr;
}

const char* vulkan_surface_extension() { return VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(static_cast<VkInstance>(instance), "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = static_cast<HWND>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler, watchpoints, entry --------------------------------------------------------------------------

namespace {

// NX_WATCH=<hex address>: hardware write watchpoint (DR0, 8 bytes) on the main thread; each write (up to 30) logs
// the host module/offset and the guest context.
uint64_t g_watch = 0;

void arm_watch(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &ctx);
    ctx.Dr0 = g_watch;
    ctx.Dr7 = (1ull << 0) | (1ull << 16) | (3ull << 18);  // L0, R/W0 = write, LEN0 = 8 bytes
    SetThreadContext(thread, &ctx);
}

void dump_guest(nx::CPU& c) {
    using namespace nx;
    log("  guest lr %s  sp %llx  tpidr %llx  tpidrro %llx", describe(c.x[30]).c_str(), (unsigned long long)c.x[31],
        (unsigned long long)c.tpidr, (unsigned long long)c.tpidrro);
    for (int k = 0; k < 32; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    backtrace(c);
}

uint64_t exe_va(void* p) { return uint64_t(p) - uint64_t(GetModuleHandleW(nullptr)) + 0x140000000ull; }

LONG WINAPI watch_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch) return EXCEPTION_CONTINUE_SEARCH;
    nx::log("WATCH: %llx written (now %llx) at %s.exe+%llx (tools/symbolize.py)", (unsigned long long)g_watch,
            (unsigned long long)nx::ld<uint64_t>(g_watch), nx::game::kInfo.id, (unsigned long long)exe_va(e->ExceptionRecord->ExceptionAddress));
    dump_guest(nx::current_cpu());
    static int hits = 0;
    if (++hits >= 30) e->ContextRecord->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// "<module>+<offset>" for a host code address; our own exe as the linker's VA (tools/symbolize.py reads the map).
std::string host_where(uint64_t pc) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &m))
        return "?";
    char b[MAX_PATH + 32];
    if (m == GetModuleHandleW(nullptr)) {
        std::snprintf(b, sizeof b, "%s.exe+%llx", nx::game::kInfo.id, (unsigned long long)exe_va(reinterpret_cast<void*>(pc)));
        return b;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
    std::snprintf(b, sizeof b, "%s+%llx", name ? name + 1 : path, (unsigned long long)(pc - uint64_t(m)));
    return b;
}

// The host call stack at the fault, unwound from its context with the x64 unwind tables.
void host_backtrace(const CONTEXT& at) {
    CONTEXT c = at;
    for (int k = 0; k < 16 && c.Rip; k++) {
        nx::log("  host #%-2d %s", k, host_where(c.Rip).c_str());
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!f) {  // a leaf function: the return address is at rsp
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler_data, &frame, nullptr);
    }
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    uint64_t info = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    nx::log("CRASH: exception %08lx at %s (tools/symbolize.py for the exe), data address %llx", r->ExceptionCode,
            host_where(uint64_t(r->ExceptionAddress)).c_str(), (unsigned long long)info);
    host_backtrace(*e->ContextRecord);
    dump_guest(nx::current_cpu());
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// NX_THROW_TRACE=1: log the host call stack of every C++ throw that is not a guest longjmp (first chance).
LONG WINAPI throw_tracer(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::string s = "C++ throw, host stack:";
    for (USHORT k = 0; k < n; k++) {
        char b[32];
        std::snprintf(b, sizeof b, " %llx", (unsigned long long)exe_va(frames[k]));
        s += b;
    }
    nx::log("%s", s.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
    nx::plat::make_dirs(nx::plat::data_root());
    nx::plat::load_pc_settings();
    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine (memguard_win.cpp)
    if (!SDL_Init(SDL_INIT_GAMEPAD)) std::fprintf(stderr, "SDL gamepad input unavailable: %s\n", SDL_GetError());
    SetUnhandledExceptionFilter(crash_handler);
    if (const char* t = std::getenv("NX_THROW_TRACE"); t && *t == '1') AddVectoredExceptionHandler(1, throw_tracer);
    if (const char* w = std::getenv("NX_WATCH")) {
        g_watch = std::strtoull(w, nullptr, 16);
        AddVectoredExceptionHandler(1, watch_handler);
        arm_watch(GetCurrentThread());
        nx::log("watching writes to %llx", (unsigned long long)g_watch);
    }
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
    return 0;
}
