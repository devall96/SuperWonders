// Linux host (x86-64): memory (mmap at fixed addresses, MAP_FIXED_NOREPLACE), pthreads, an SDL3 window with a
// Vulkan surface made by SDL (Wayland or X11, whichever SDL picked), SDL3 gamepad + keyboard, SDL3 audio, a signal
// crash handler and main. Based on platform_mac.cpp.
//
// SDL wants its video calls on the main thread: main() starts the game (nx::boot) on its own thread and then runs
// the SDL event loop. Anything that touches the window is done there; the game's threads ask for it and wait. Input
// is sampled on the main thread into a snapshot read_pad() copies.
//
// Guest layout: the Windows/Android one (runtime.h: NX_BASE 0x40_0000_0000, heap 0x50, alias 0x58, stacks 0x68). A
// non-PIE or PIE executable, its libraries and the heap all live far from there on x86-64 Linux (47-bit user space,
// mmap top-down from 0x7fxx_xxxx_xxxx), so the fixed reservations do not collide.
//
// Data: NX_DATA_ROOT or games/<game>'s pc_data_root ("private/<game>"): analysis/{main,sdk}.bin, extract/romfs, user/.
// Settings: <data root>/pc-settings.ini through the overlay menu (F1, or Minus + Plus on a controller: overlay/), and
// NX_<KEY> environment variables, as on Windows and macOS.
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "nvn/nvn_vk.h"
#include "overlay/overlay.h"
#include "overlay/settings.h"
#include "platform.h"
#include "svc_am.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace nx {
void boot();
}

namespace nx::plat {

namespace {

pid_t g_main_tid = 0;

bool on_main_thread() { return pid_t(syscall(SYS_gettid)) == g_main_tid; }

// Work for the main thread (window creation), run between event polls.
std::mutex g_main_lock;
std::deque<std::function<void()>> g_main_work;

void run_on_main(std::function<void()> fn) {
    if (on_main_thread()) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard lk(g_main_lock);
        g_main_work.push_back([&] {
            fn();
            std::lock_guard l2(m);
            done = true;
            cv.notify_one();
        });
    }
    std::unique_lock lk(m);
    cv.wait(lk, [&] { return done; });
}

SDL_Window* g_window = nullptr;
std::atomic<bool> g_minimized{false};
std::atomic<uint32_t> g_pixel_w{0}, g_pixel_h{0};

std::mutex g_pad_lock;
PadState g_pad;
std::atomic<bool> g_fast_forward{false};  // Tab held (sampled on the main thread)

// Player settings (overlay/settings.h). Keyboard bindings are SDL scancodes. g_settings_lock guards g_settings; the
// bindings are copied into atomics for the input sampling.
std::mutex g_settings_lock;
overlay::Settings g_settings;
std::array<std::atomic<int>, overlay::kControlCount> g_key_binding{}, g_pad_binding{};
SDL_AudioStream* g_stream = nullptr;
constexpr uint64_t kActionBits[overlay::kControlCount] = {
    kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kStickL, kStickR, kLeft, kUp, kRight, kDown,
};

void post_to_main(std::function<void()> fn);  // runs on the main thread soon; does not wait

}  // namespace

// ---- memory ---------------------------------------------------------------------------------------------------

// MAP_FIXED_NOREPLACE (Linux 4.17+): exactly at addr or fail, never over an existing mapping. Older kernels ignore
// the flag and treat addr as a hint, so the result is checked as on macOS.
bool reserve(uint64_t addr, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (uint64_t(p) != addr) {
        munmap(p, size);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize()) - 1;
    uint64_t a = addr & ~page, e = (addr + size + page) & ~page;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

// Whole host pages are released (MADV_DONTNEED: they read back as zeros, as a freshly committed page does on
// Windows); the freed part of a page shared with live memory is zeroed instead. x86-64 pages are 4 KiB, like the
// guest's, so that case only comes with unaligned requests.
void decommit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize());
    uint64_t lo = (addr + page - 1) & ~(page - 1), hi = (addr + size) & ~(page - 1);
    if (lo >= hi) {
        std::memset(reinterpret_cast<void*>(addr), 0, size);
        return;
    }
    if (addr < lo) std::memset(reinterpret_cast<void*>(addr), 0, lo - addr);
    if (hi < addr + size) std::memset(reinterpret_cast<void*>(hi), 0, addr + size - hi);
    madvise(reinterpret_cast<void*>(lo), hi - lo, MADV_DONTNEED);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (stack_size + 0xFFFF) & ~size_t(0xFFFF));
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) {
    nx::nvn::flush_pipeline_cache();
    _exit(code);
}

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

// No vblank wait on Linux yet (SDL has none, and Vulkan present timing is not in use): hle_vi paces on its 60 Hz timer.
bool wait_vblank() { return false; }

void yield() { sched_yield(); }

// Linux thread names are at most 15 bytes; longer ones make pthread_setname_np fail with ERANGE.
void set_thread_name(const char* name) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", name);
    pthread_setname_np(pthread_self(), buf);
}

int64_t current_thread_id() { return int64_t(syscall(SYS_gettid)); }

bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }  // use perf

uint64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }

uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    if (size > uint64_t(SSIZE_MAX)) return 0;  // the game passes -1 as "everything": fread read nothing
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r && *r ? r : nx::game::kInfo.pc_data_root;
}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}

// ---- window ---------------------------------------------------------------------------------------------------

bool headless() {
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void* create_window() {
    run_on_main([] {
        if (g_window) return;
        int w = 1280, h = 720;
        bool full = false;
        {
            std::lock_guard lk(g_settings_lock);
            w = g_settings.width, h = g_settings.height, full = g_settings.fullscreen;
        }
        std::string r = setting("resolution");
        if (!r.empty()) std::sscanf(r.c_str(), "%dx%d", &w, &h);
        g_window = SDL_CreateWindow(nx::game::kInfo.title, w, h,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN |
                                        (headless() ? SDL_WINDOW_HIDDEN : 0));
        if (!g_window) {
            log_line(SDL_GetError());
            return;
        }
        if (setting("fullscreen").empty() ? full : setting("fullscreen") != "0") SDL_SetWindowFullscreen(g_window, true);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        g_pixel_w = uint32_t(pw), g_pixel_h = uint32_t(ph);
    });
    return g_window;  // the SDL_Window*: the Vulkan surface is made from it (SDL_Vulkan_CreateSurface)
}

void release_window(void*) {}
uint64_t window_generation() { return 1; }
bool window_minimized() { return g_minimized; }

void shader_compile_progress(uint64_t, uint64_t, bool) {}
void overlay_game_interval(int interval) { overlay::game_interval(interval); }
void overlay_presented(uint64_t now_ns) { overlay::presented(now_ns); }

void window_size(uint32_t& w, uint32_t& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }
float default_render_scale() { return 1.0f; }

// ---- input: first SDL gamepad + keyboard (sampled on the main thread) -----------------------------------------

static void sample_input() {
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
            SDL_GAMEPAD_BUTTON_SOUTH,      SDL_GAMEPAD_BUTTON_EAST,       SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_NORTH,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_INVALID,    SDL_GAMEPAD_BUTTON_INVALID,  // the triggers are axes
            SDL_GAMEPAD_BUTTON_START,      SDL_GAMEPAD_BUTTON_BACK,       SDL_GAMEPAD_BUTTON_LEFT_STICK,
            SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
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

// ---- audio: SDL3 stream, s16 stereo 48 kHz, fed from the queue of submitted buffers ----------------------------

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
AudioDone g_done = nullptr;
std::mutex g_audio;
std::deque<Pending> g_pending;

void SDLCALL audio_cb(void*, SDL_AudioStream* s, int additional, int) {
    uint8_t buf[4096];
    while (additional > 0) {
        int n = std::min<int>(additional, sizeof buf);
        int filled = 0;
        std::vector<void*> finished;
        {
            std::lock_guard lk(g_audio);
            while (filled < n && !g_pending.empty()) {
                Pending& p = g_pending.front();
                size_t take = std::min(size_t(n - filled), p.size - p.pos);
                std::memcpy(buf + filled, p.data + p.pos, take);
                filled += int(take);
                p.pos += take;
                if (p.pos == p.size) {
                    finished.push_back(p.ctx);
                    g_pending.pop_front();
                }
            }
        }
        if (filled < n) std::memset(buf + filled, 0, size_t(n - filled));  // underrun: silence
        SDL_PutAudioStreamData(s, buf, n);
        for (void* ctx : finished) g_done(ctx);
        additional -= n;
    }
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 48000};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!g_stream) {
        log("audio: no device (%s)", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_stream) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused)
        SDL_PauseAudioStreamDevice(g_stream);
    else
        SDL_ResumeAudioStreamDevice(g_stream);
}

// ---- settings (the overlay menu) ----------------------------------------------------------------------------------

namespace {

std::mutex g_main_post_lock;
std::deque<std::function<void()>> g_main_posted;

void post_to_main(std::function<void()> fn) {
    std::lock_guard lk(g_main_post_lock);
    g_main_posted.push_back(std::move(fn));
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

void load_player_settings() {
    overlay::Settings s;
    const int keys[overlay::kControlCount] = {
        SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_Q, SDL_SCANCODE_E,
        SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_M, SDL_SCANCODE_N,
        SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    };
    for (size_t a = 0; a < overlay::kControlCount; ++a)
        s.keyboard_bindings[a] = keys[a], s.controller_bindings[a] = overlay::default_controller_binding(a, 0);
    bool found = overlay::load_settings(settings_path(), s);
    if (const char* e = std::getenv("NX_MEM_LIMIT_GB"); e && std::atof(e) > 0) s.memory_limit_gb = std::atoi(e);
    apply_settings(s, false);
    overlay::Host host;
    host.apply = apply_settings;
    host.key_name = key_name;
    // Wayland has no "always on top" for clients; X11 does.
    host.can_always_on_top = std::strcmp(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "", "wayland") != 0;
    overlay::init(host, s);
    log("platform: settings %s (%s)", found ? "loaded" : "defaults", settings_path().c_str());
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

// ---- Vulkan (the system loader, through SDL; the surface extension is the one SDL's video driver needs) ----------

void* vulkan_get_instance_proc_addr() {
    static void* gipa = [] {
        void* p = nullptr;
        run_on_main([&] {
            if (!SDL_Vulkan_LoadLibrary(nullptr)) {
                log_line(SDL_GetError());
                return;
            }
            p = reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        });
        return p;
    }();
    return gipa;
}

// VK_KHR_wayland_surface or VK_KHR_xlib_surface (whichever SDL's video driver uses); the runtime adds
// VK_KHR_surface itself.
const char* vulkan_surface_extension() {
    static const char* ext = [] {
        const char* found = "VK_KHR_xlib_surface";
        run_on_main([&] {
            Uint32 n = 0;
            const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
            for (Uint32 k = 0; names && k < n; k++)
                if (std::strcmp(names[k], VK_KHR_SURFACE_EXTENSION_NAME) != 0) found = names[k];
        });
        return found;
    }();
    return ext;
}

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void*) {
    VkSurfaceKHR s = VK_NULL_HANDLE;
    bool ok = false;
    run_on_main([&] {
        ok = SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(native_window), static_cast<VkInstance>(instance),
                                      nullptr, &s);
        if (!ok) log("vulkan: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    });
    *surface = reinterpret_cast<uint64_t>(s);
    return ok ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

void crash_handler(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && nx::plat::write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    const auto& m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    Dl_info d{};
    uint64_t pc = uint64_t(m.gregs[REG_RIP]);
    dladdr(reinterpret_cast<void*>(pc), &d);
    nx::log("CRASH: signal %d at %s+%llx, data address %llx", sig, d.dli_fname ? d.dli_fname : "?",
            (unsigned long long)(pc - uint64_t(d.dli_fbase)), (unsigned long long)uint64_t(info->si_addr));
    nx::CPU& c = nx::current_cpu();
    nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
    for (int k = 0; k < 32; k += 4)
        nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
                (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    nx::backtrace(c);
    signal(sig, SIG_DFL);
    raise(sig);
}

void game_thread() {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

}  // namespace

int main() {
    nx::plat::g_main_tid = pid_t(syscall(SYS_gettid));

    // The crash handler runs on its own stack: a guest stack overflow must still be reported.
    static std::vector<uint8_t> alt(1 << 20);
    stack_t ss{};
    ss.ss_sp = alt.data();
    ss.ss_size = alt.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    nx::log("platform: Linux, SDL video driver %s", SDL_GetCurrentVideoDriver());

    nx::plat::make_dirs(nx::plat::data_root());
    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine
    nx::plat::load_player_settings();

    // The game on its own thread, with a big stack (recompiled calls nest deeply); the main thread keeps SDL's
    // event loop.
    nx::plat::start_thread([](void*) { game_thread(); nx::plat::exit_process(0); }, nullptr, 64ull << 20);

    using namespace nx::plat;
    namespace overlay = nx::overlay;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                nx::log("platform: quit requested (window closed)");
                exit_process(0);
            case SDL_EVENT_WINDOW_MINIMIZED:
                g_minimized = true;
                break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
                g_minimized = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_pixel_w = uint32_t(e.window.data1), g_pixel_h = uint32_t(e.window.data2);
                break;
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
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                if (overlay::wants_mouse() && g_window) {
                    float d = SDL_GetWindowPixelDensity(g_window);
                    overlay::mouse_move(e.motion.x * d, e.motion.y * d);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (overlay::wants_mouse()) {
                    int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                    overlay::mouse_button(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (overlay::wants_mouse()) overlay::mouse_wheel(e.wheel.y);
                break;
            default:
                break;
            }
        }
        std::deque<std::function<void()>> work, posted;
        {
            std::lock_guard lk(g_main_lock);
            work.swap(g_main_work);
        }
        {
            std::lock_guard lk(g_main_post_lock);
            posted.swap(g_main_posted);
        }
        for (auto& fn : work) fn();
        for (auto& fn : posted) fn();
        sample_input();
        SDL_Delay(2);
    }
}
