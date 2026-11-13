// SuperWonders Launcher (launcher.h): state, the setup job, and the screen.
#include "launcher/launcher.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <nx/gamelib.h>
#include <nx/runtime.h>

#include "imgui.h"
#include "launcher/test_pad.h"
#include "launcher/updates.h"
#include "nvn/nvn_vk.h"
#include "overlay/option_rows.h"
#include "overlay/overlay.h"
#include "overlay/overlay_ui.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace nx::launcher {

namespace ui = overlay::ui;
namespace rows = overlay::rows;

namespace {

fs::path u8(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string str(const fs::path& p) {
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
bool file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
std::string read_line(const fs::path& p, const char* key) {  // "key value" lines (build-id.txt)
    std::ifstream f(p);
    std::string line, k = std::string(key) + " ";
    while (std::getline(f, line))
        if (line.rfind(k, 0) == 0) return line.substr(k.size());
    return {};
}

// ---- launcher.ini: what the player picked (in the data root) ----
struct Config {
    std::string exefs, romfs;
    int romfs_copy = 1;  // 1 copy the RomFS into the data root (the default, as Savage Wind does), 0 use it in place
};

fs::path config_path() { return u8(plat::data_root()) / "launcher.ini"; }

Config load_config() {
    Config c;
    std::ifstream f(config_path());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "exefs") c.exefs = v;
        else if (k == "romfs") c.romfs = v;
        else if (k == "romfs_copy") c.romfs_copy = v != "0";
    }
    return c;
}

void save_config(const Config& c) {
    plat::make_dirs(plat::data_root());
    std::ofstream f(config_path(), std::ios::trunc);
    f << "exefs=" << c.exefs << "\nromfs=" << c.romfs << "\nromfs_copy=" << c.romfs_copy << "\n";
}

// ---- the game-build kit: tools/setup_game.py and a Python to run it ----
struct Kit {
    std::string python, script;  // empty: none
    std::string id;              // game-build/kit-id.txt: what the release's game code is built with
};

Kit find_kit() {
    Kit k;
    const fs::path kit = u8(program_dir()) / "game-build";
#if defined(_WIN32)
    const fs::path py = kit / "python" / "python.exe";
#else
    const fs::path py = file(kit / "python" / "bin" / "python3") ? kit / "python" / "bin" / "python3" : fs::path("python3");
#endif
    if (file(kit / "setup_game.py")) {
        k.script = str(kit / "setup_game.py");
        k.python = py.is_absolute() ? str(py) : "python3";
        std::ifstream f(kit / "kit-id.txt");
        std::getline(f, k.id);
    }
    // a developer's build: NX_SETUP_SCRIPT=<checkout>/tools/setup_game.py, NX_SETUP_PYTHON=<a Python with the deps>
    if (const char* s = std::getenv("NX_SETUP_SCRIPT"); s && *s) k.script = s;
    if (const char* p = std::getenv("NX_SETUP_PYTHON"); p && *p) k.python = p;
    if (!k.script.empty() && k.python.empty()) k.python =
#if defined(_WIN32)
        "python";
#else
        "python3";
#endif
    return k;
}

// ---- is the game set up? ----
enum class Ready { Yes, NotSetUp, Rebuild };
struct Status {
    Ready ready = Ready::NotSetUp;
    std::string why;
};

Status check_status(const Kit& kit) {
    const fs::path d = u8(plat::data_root());
    Status s;
    const char* missing = nullptr;
    if (game_lib_path().empty()) missing = "The game code isn't built yet.";
    else if (!file(d / "analysis" / "main.bin") || !file(d / "analysis" / "sdk.bin")) missing = "The game's executable isn't read yet.";
    else if (!file(d / "shader_programs.bin") || !file(d / "spirv" / "results.tsv")) missing = "The shaders aren't translated yet.";
    else if (!fs::exists(d / "extract" / "romfs")) missing = "The game's files aren't set up yet.";
    if (missing) {
        s.why = missing;
        return s;
    }
    // A release whose kit builds the game code differently (an update): build it again before playing.
    const std::string built = read_line(d / "game" / "build-id.txt", "kit");
    if (!kit.id.empty() && !built.empty() && built != kit.id && game_lib_path() == str(d / "game" / NX_GAMELIB_FILE)) {
        s.ready = Ready::Rebuild;
        s.why = "This version of SuperWonders builds the game code differently.";
        return s;
    }
    s.ready = Ready::Yes;
    return s;
}

// ---- the setup job: tools/setup_game.py in a process of its own, its progress lines read here ----
class Job {
public:
    std::atomic<bool> running{false};
    std::atomic<float> progress{0};
    std::atomic<int> result{-1};  // exit code of the last run, -1 none yet
    std::mutex lock;
    std::string what;                // the current step
    std::vector<std::string> tail;   // the last lines (shown when it fails)
    uint64_t started_ns = 0;

    void start(const std::vector<std::string>& args) {
        if (running) return;
        if (worker.joinable()) worker.join();
        {
            std::lock_guard lk(lock);
            what = "Starting";
            tail.clear();
        }
        progress = 0, result = -1, running = true, cancelled = false;
        started_ns = plat::monotonic_ns();
        worker = std::thread([this, args] { run(args); });
    }
    void cancel() {
        cancelled = true;
#if defined(_WIN32)
        if (job) TerminateJobObject(job, 1);
#else
        if (pid > 0) kill(-pid, SIGTERM);
#endif
    }
    bool was_cancelled() const { return cancelled; }
    ~Job() {
        if (running) cancel();
        if (worker.joinable()) worker.join();
    }

private:
    std::thread worker;
    std::atomic<bool> cancelled{false};
#if defined(_WIN32)
    HANDLE job = nullptr;
#else
    pid_t pid = -1;
#endif

    void line(std::string l) {
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        if (l.empty()) return;
        float f;
        char text[512] = "";
        if (std::sscanf(l.c_str(), "progress %f %511[^\n]", &f, text) >= 1) {
            progress = std::clamp(f, 0.0f, 1.0f);
            std::lock_guard lk(lock);
            if (*text) what = text;
            return;
        }
        log("launcher: setup: %s", l.c_str());
        std::lock_guard lk(lock);
        tail.push_back(l);
        if (tail.size() > 12) tail.erase(tail.begin());
    }

    void run(std::vector<std::string> args) {
        int code = 1;
#if defined(_WIN32)
        auto wide = [](const std::string& s) {
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring w(size_t(std::max(1, n)) - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
            return w;
        };
        std::wstring cmd;
        for (const std::string& a : args) {
            std::wstring w = wide(a);
            cmd += (cmd.empty() ? L"\"" : L" \"") + w + L"\"";  // (no argument here ends in a backslash or has quotes)
        }
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        // A job object: cancelling ends the whole tree (the compiler and translator processes too).
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                           nullptr, &si, &pi)) {
            AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(wr);
            wr = nullptr;
            std::string buf;
            char chunk[4096];
            DWORD n = 0;
            while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) {
                buf.append(chunk, n);
                for (size_t nl; (nl = buf.find('\n')) != std::string::npos; buf.erase(0, nl + 1)) line(buf.substr(0, nl));
            }
            line(buf);
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD ec = 1;
            GetExitCodeProcess(pi.hProcess, &ec);
            code = int(ec);
            CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
        } else {
            line("error: couldn't start " + args[0] + " (error " + std::to_string(GetLastError()) + ")");
        }
        if (wr) CloseHandle(wr);
        CloseHandle(rd);
        CloseHandle(job);
        job = nullptr;
#else
        int fds[2];
        if (pipe(fds) == 0) {
            pid = fork();
            if (pid == 0) {
                setpgid(0, 0);  // its own process group: cancelling ends the whole tree
                dup2(fds[1], 1), dup2(fds[1], 2);
                close(fds[0]), close(fds[1]);
                std::vector<char*> argv;
                for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
                argv.push_back(nullptr);
                execvp(argv[0], argv.data());
                std::fprintf(stderr, "error: couldn't start %s\n", argv[0]);
                _exit(127);
            }
            close(fds[1]);
            std::string buf;
            char chunk[4096];
            for (ssize_t n; (n = read(fds[0], chunk, sizeof chunk)) > 0;) {
                buf.append(chunk, size_t(n));
                for (size_t nl; (nl = buf.find('\n')) != std::string::npos; buf.erase(0, nl + 1)) line(buf.substr(0, nl));
            }
            line(buf);
            close(fds[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
            pid = -1;
        }
#endif
        result = cancelled ? 2 : code;
        running = false;
    }
};

// ---- the launcher's state (the screen runs on the presenting thread, run() on the game's) ----
enum Page { PagePlay, PageFiles, PageSettings, PageUpdates, PageQuit, kPages };
const char* const kPageNames[kPages] = {"Play", "Game files", "Settings", "Updates", "Quit"};

struct State {
    Kit kit;
    Config config;
    Status status;
    std::atomic<int> page{PagePlay};
    std::atomic<bool> play{false}, quit{false};
    bool config_mode = false;
    Job job;
    std::mutex pick_lock;
    std::string picked_exefs, picked_romfs;  // from the folder picker's thread
    uint64_t shown_ns = 0;
    std::string note;  // a line under the status (an error of the folder picker, a finished setup...)
    bool demo_progress = false;  // NX_LAUNCHER_TOUR's last step: the setup card, not running anything
    // The controller (Savage Wind's launcher: the page list and the page are two places the cursor can be; A or right
    // goes into the page, B back to the list, L / R change the page, + plays). One request at a time, next frame.
    bool focus_list = true;   // the cursor to the selected page's name
    int follow_hold = 0;      // frames the page doesn't follow the cursor (L / R moved it; the cursor catches up)
    bool in_list = true;      // where it is now
    uint64_t pad_before = 0;  // the pad's buttons last frame (edges)
    // The folder browser (a list a controller can walk; the system's picker needs a mouse): 0 closed, 1 for the
    // ExeFS, 2 for the RomFS. "" as the folder: the drives (Windows) or / (elsewhere).
    int browse = 0;
    std::string browse_dir;
    std::vector<std::string> browse_list;  // its subfolders, read when it changes
    std::string browse_listed;
};
State* g = nullptr;

void refresh_status() {
    g->status = check_status(g->kit);
}

// A folder for a row's value, fitted to the room the row leaves beside its label (in pixels, at the current font and
// scale): the whole path when it fits, else its last parts after "...", else the end of its last part. Measured, so
// a narrow window, a large display scale or a long path can't make the value run over the label.
std::string fit_path(const std::string& path, const char* label) {
    std::string s = path;
    for (char& c : s) c = c == '\\' ? '/' : c;
    std::string up = label;
    for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    const float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(up.c_str()).x -
                       4 * ImGui::GetStyle().FramePadding.x - 3 * ImGui::GetFontSize();  // (padding, the gap)
    auto fits = [&](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x <= room; };
    if (fits(s)) return s;
    for (size_t at = s.find('/'); at != std::string::npos; at = s.find('/', at + 1)) {
        const std::string t = "..." + s.substr(at);
        if (fits(t)) return t;
    }
    for (size_t cut = 1; cut < s.size(); ++cut) {  // not even the last part fits: its end
        const std::string t = "..." + s.substr(cut);
        if (fits(t)) return t;
    }
    return "...";
}

bool folder_has(const std::string& dir, const char* name) {
    std::error_code ec;
    return !dir.empty() && fs::exists(u8(dir) / name, ec);
}

void start_setup() {
    if (g->kit.script.empty()) {
        g->note = "This build of SuperWonders has no game-build folder, so it can't set the game up.";
        return;
    }
    save_config(g->config);
    g->job.start({g->kit.python, "-u", g->kit.script, "setup", "--exefs", g->config.exefs, "--romfs", g->config.romfs,
                  "--data-root", plat::data_root(), "--romfs-mode", g->config.romfs_copy ? "copy" : "link"});
    log("launcher: setup started (ExeFS %s, RomFS %s, %s)", g->config.exefs.c_str(), g->config.romfs.c_str(),
        g->config.romfs_copy ? "copy" : "link");
}

// ---- drawing ----
float g_k = 1;

void backdrop(ImDrawList* dl, ImVec2 ds) {
    const ui::Theme& th = ui::theme();
    dl->AddRectFilledMultiColor({0, 0}, {ds.x, ds.y * 0.5f}, th.back_top, th.back_top, th.back_mid, th.back_mid);
    dl->AddRectFilledMultiColor({0, ds.y * 0.5f}, ds, th.back_mid, th.back_mid, th.back_top, th.back_top);
    // a slow glow drifting behind the pages (the compile screen's, overlay_progress.cpp)
    const float t = float(ImGui::GetTime());
    const float gx = ds.x * (0.62f + 0.18f * std::sin(t * 0.3f)), gy = ds.y * 0.42f, r = ds.y * 0.6f;
    const ImU32 in = (th.accent2 & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 30), out = in & ~IM_COL32_A_MASK;
    constexpr int kSeg = 64;
    dl->PrimReserve(kSeg * 3, kSeg + 1);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx({gx, gy}, uv, in);
    for (int i = 0; i < kSeg; ++i) {
        const float a = float(i) / kSeg * 6.2831853f;
        dl->PrimWriteVtx({gx + std::cos(a) * r, gy + std::sin(a) * r}, uv, out);
    }
    for (int i = 0; i < kSeg; ++i) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % kSeg));
    }
}

// The setup's progress: a card with the gliding bar and its sheen (the compile screen's look), in `area`.
void progress_card(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const ui::Theme& th = ui::theme();
    const float k = g_k;
    static float shown = 0;
    const float frac = g->job.progress;
    shown = g->job.running ? ui::approach(shown, frac, 5.0f) : frac;
    std::string what;
    {
        std::lock_guard lk(g->job.lock);
        what = g->job.what;
    }
    ImFont* tf = ui::title();
    ImFont* bf = ui::body();
    const float tfs = 46 * k, sfs = 26 * k;
    dl->AddRectFilled(p0, p1, th.card, 14 * k);
    dl->AddRect(p0, p1, ui::fade(th.accent, 0.55f), 14 * k, 0, 1.5f * k);
    const char* title = "Setting up your game";
    const ImVec2 ts = tf->CalcTextSizeA(tfs, FLT_MAX, 0, title);
    dl->AddText(tf, tfs, {(p0.x + p1.x - ts.x) * 0.5f, p0.y + 36 * k}, th.accent, title);
    const ImVec2 ba(p0.x + 48 * k, p0.y + 36 * k + ts.y + 30 * k), bb(p1.x - 48 * k, ba.y + 24 * k);
    ui::progress_bar(dl, ba, bb, shown);
    char line[640];
    const double secs = double(plat::monotonic_ns() - g->job.started_ns) / 1e9;
    std::snprintf(line, sizeof line, "%s   %d%%   %d:%02d", what.c_str(), int(shown * 100 + 0.5f), int(secs) / 60,
                  int(secs) % 60);
    const ImVec2 ls = bf->CalcTextSizeA(sfs, FLT_MAX, 0, line);
    dl->AddText(bf, sfs, {(p0.x + p1.x - ls.x) * 0.5f, bb.y + 26 * k}, th.text_dim, line);
    const char* note = "This happens once, and again only after an update that needs it. It takes about 15 minutes.";
    const ImVec2 ns = bf->CalcTextSizeA(sfs * 0.9f, FLT_MAX, 0, note);
    dl->AddText(bf, sfs * 0.9f, {(p0.x + p1.x - ns.x) * 0.5f, bb.y + 26 * k + ls.y + 16 * k},
                ui::mix(th.text_off, th.text_dim, 0.4f), note);
}

void page_play() {
    rows::begin("##play", ImGui::GetContentRegionAvail().y);
    rows::section("SuperWonders");
    const bool ready = g->status.ready == Ready::Yes;
    if (!ready) rows::disable_next();
    if (rows::action("Play", ready ? "Start" : "Set up first",
                     ready ? "Start Super Mario Bros. Wonder." : "Your game files aren't set up yet: open Game files.",
                     true, ready))
        g->play = true;
    if (!ready && rows::action("Set up game files", "", "Pick your ExeFS and RomFS folders and set the game up."))
        g->page = PageFiles, rows::focus_first();
    rows::section("About");
    rows::action("Version", NX_VERSION_STRING,
                 "SuperWonders, a static recompilation of Super Mario Bros. Wonder (1.0.0) for PC. Nothing of the game "
                 "ships with it: it's set up from your own copy.",
                 true);
    rows::end();
}

// ---- the folder browser ----
std::string home_folder() {
    const char* h = std::getenv(
#if defined(_WIN32)
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    return h && *h ? h : "";
}

void browse_open(int which) {
    const std::string& now = which == 1 ? g->config.exefs : g->config.romfs;
    std::error_code ec;
    std::string start = !now.empty() && fs::is_directory(u8(now), ec) ? str(u8(now).parent_path()) : home_folder();
    g->browse = which, g->browse_dir = start, g->browse_listed = "\x01";
    rows::focus_first();
    log("launcher: folder browser for the %s, from %s", which == 1 ? "ExeFS" : "RomFS", start.c_str());
}

void browse_close() {
    g->browse = 0;
    rows::focus_first();
}

void browse_read() {
    if (g->browse_listed == g->browse_dir) return;
    g->browse_listed = g->browse_dir;
    g->browse_list.clear();
    if (g->browse_dir.empty()) {  // the top: the drives, or the root
#if defined(_WIN32)
        const DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; ++d)
            if (drives & (1u << d)) g->browse_list.push_back(std::string(1, char('A' + d)) + ":/");
#else
        g->browse_list.push_back("/");
#endif
        return;
    }
    std::error_code ec;
    for (auto it = fs::directory_iterator(u8(g->browse_dir), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        const std::string n = str(it->path().filename());
        if (n.empty() || n[0] == '.' || n[0] == '$') continue;  // (hidden and system folders)
        g->browse_list.push_back(n);
        if (g->browse_list.size() >= 2000) break;
    }
    std::sort(g->browse_list.begin(), g->browse_list.end(), [](const std::string& a, const std::string& b) {
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[i]));
            if (x != y) return x < y;
        }
        return a.size() < b.size();
    });
}

void page_browse() {
    browse_read();
    const bool exefs = g->browse == 1;
    const std::string& dir = g->browse_dir;
    rows::begin("##browse", ImGui::GetContentRegionAvail().y);
    rows::section(exefs ? "Choose your ExeFS folder" : "Choose your RomFS folder");
    const bool ok = exefs ? folder_has(dir, "main") && folder_has(dir, "sdk") : folder_has(dir, "Shader");
    const std::string where = dir.empty() ? std::string("This PC") : fit_path(dir, "Use this folder");
    const std::string use_desc = std::string(ok ? "Use this folder." : exefs ? "Not an ExeFS folder: it has no main and sdk."
                                                                             : "Not a RomFS folder: it has no Shader folder.") +
                                 (dir.empty() ? std::string() : "\n\n" + dir);
    if (!ok) rows::disable_next();
    if (rows::action("Use this folder", where.c_str(), use_desc.c_str(), true, ok)) {
        (exefs ? g->config.exefs : g->config.romfs) = dir;
        save_config(g->config);
        log("launcher: %s folder %s", exefs ? "ExeFS" : "RomFS", dir.c_str());
        browse_close();
        rows::end();
        return;
    }
    if (!dir.empty() && rows::action("Up one folder", "", "The folder above this one.", true)) {
        const fs::path p = u8(dir);
        g->browse_dir = p.has_parent_path() && p.parent_path() != p ? str(p.parent_path()) : "";
        if (g->browse_dir == dir) g->browse_dir.clear();  // (a drive's root: up to the drives)
        rows::focus_first();
    }
    if (rows::action("Home folder", "", "Your home folder.", true)) g->browse_dir = home_folder(), rows::focus_first();
    rows::section(dir.empty() ? "Drives" : "Folders here");
    if (g->browse_list.empty()) rows::action("(no folders here)", "", "This folder has no folders in it.", false);
    std::string go;
    for (const std::string& n : g->browse_list)
        if (rows::action(n.c_str(), "", "Open this folder.", false)) go = n;
    if (!go.empty()) {
        g->browse_dir = dir.empty() ? go : str(u8(dir) / u8(go));
        rows::focus_first();
    }
    rows::section("Other ways");
    if (rows::action("The system's folder picker", "", "Choose the folder in the system's own window (a mouse helps).",
                     true))
        plat::pick_folder(exefs ? "Your ExeFS folder (main, sdk)" : "Your RomFS folder", dir, [exefs](std::string p) {
            std::lock_guard lk(g->pick_lock);
            (exefs ? g->picked_exefs : g->picked_romfs) = p;
        });
    if (rows::action("Cancel", "", "Back, without changing the folder (B).", true)) browse_close();
    rows::end();
}

void page_files() {
    if (g->browse) return page_browse();
    rows::begin("##files", ImGui::GetContentRegionAvail().y);
    rows::section("Your game files");
    const std::string exefs = g->config.exefs.empty() ? "Choose..." : fit_path(g->config.exefs, "ExeFS folder");
    const std::string exefs_desc =
        "The ExeFS folder of your own copy of the game (version 1.0.0). It holds main and sdk." +
        (g->config.exefs.empty() ? std::string() : "\n\nNow: " + g->config.exefs);
    if (rows::action("ExeFS folder", exefs.c_str(), exefs_desc.c_str(), true)) browse_open(1);
    const std::string romfs = g->config.romfs.empty() ? "Choose..." : fit_path(g->config.romfs, "RomFS folder");
    const std::string romfs_desc = "The RomFS folder of your own copy of the game." +
                                   (g->config.romfs.empty() ? std::string() : "\n\nNow: " + g->config.romfs);
    if (rows::action("RomFS folder", romfs.c_str(), romfs_desc.c_str(), true)) browse_open(2);
    static const char* const kModes[] = {"Use it in place", "Copy it"};
    rows::choice("RomFS", &g->config.romfs_copy, kModes, 2, 1,
                 "Copy it: SuperWonders keeps its own copy (about 3.6 GB), so it keeps working if you move or delete "
                 "yours. Use it in place: no copy, but SuperWonders needs your RomFS folder where it is.");
    rows::section("Set up");
    const bool exefs_ok = folder_has(g->config.exefs, "main") && folder_has(g->config.exefs, "sdk");
    const bool romfs_ok = folder_has(g->config.romfs, "Shader");
    const char* why = !exefs_ok ? "Choose your ExeFS folder first (it holds main and sdk)."
                      : !romfs_ok ? "Choose your RomFS folder first (it holds Shader, Layout, Effect...)."
                                  : "Reads your game files and builds the game code and the shaders from them, in this "
                                    "PC's SuperWonders data folder. Steps that are already done are skipped.";
    if (!exefs_ok || !romfs_ok) rows::disable_next();
    if (rows::action(g->status.ready == Ready::Yes ? "Set up again" : "Set up game files", "", why, true,
                     exefs_ok && romfs_ok && g->status.ready != Ready::Yes))
        start_setup();
    if (rows::action("Open the data folder", "", "Where SuperWonders keeps your saves, settings and the game it built.",
                     true))
        plat::open_folder(plat::data_root());
    rows::end();
}

void page_settings() {
    rows::begin("##settings", ImGui::GetContentRegionAvail().y);
    rows::section("Settings");
    if (rows::action("Open settings", "", "Display, graphics, audio, controls and the overlay: the same settings as "
                                          "the in-game menu (F1, or Minus + Plus held). Saved in pc-settings.ini.",
                     true, true))
        overlay::toggle();
    rows::end();
}

void page_quit() {
    rows::begin("##quit", ImGui::GetContentRegionAvail().y);
    rows::section("Quit");
    if (rows::action("Quit SuperWonders", "", "Close the launcher.", true)) g->quit = true;
    rows::end();
}

void screen(float w, float h) {
    if (!g) return;
    if (g->follow_hold > 0) g->follow_hold--;
    g_k = std::clamp(h / 1080.0f, 0.5f, 3.0f);
    const float k = g_k;
    const ui::Theme& th = ui::theme();
    {
        std::lock_guard lk(g->pick_lock);  // folders the picker returned
        if (!g->picked_exefs.empty() || !g->picked_romfs.empty()) g->browse = 0;  // (the system's picker answered)
        if (!g->picked_exefs.empty()) g->config.exefs = g->picked_exefs, g->picked_exefs.clear(), save_config(g->config);
        if (!g->picked_romfs.empty()) g->config.romfs = g->picked_romfs, g->picked_romfs.clear(), save_config(g->config);
    }
    static bool was_running = false;
    if (was_running && !g->job.running) {  // a setup finished
        refresh_status();
        const int r = g->job.result;
        g->note = r == 0 ? "Your game is set up." : g->job.was_cancelled() ? "Setup cancelled." : "Setup failed.";
        log("launcher: setup %s (exit %d)", r == 0 ? "finished" : "failed", r);
        if (r == 0 && !g->config_mode) g->play = true;  // (a first setup goes straight into the game)
        if (r == 0) g->page = PagePlay, rows::focus_first();
    }
    was_running = g->job.running;

    const ImVec2 ds{w, h};
    backdrop(ImGui::GetBackgroundDrawList(), ds);
    // fades and rises in, as the menu does
    const float reveal = ui::smoothstep(std::min(1.0f, float(double(plat::monotonic_ns() - g->shown_ns) / 0.35e9)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, reveal);
    ImGui::SetNextWindowPos({0, (1 - reveal) * 20 * k});
    ImGui::SetNextWindowSize(ds);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus;
    // The launcher has the controller whenever the settings menu is closed (it closes: back where it was).
    static bool menu_was_open = false;
    const bool menu_open = overlay::is_open();
    if (!menu_open && (menu_was_open || !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))) ImGui::SetNextWindowFocus();
    if (menu_was_open && !menu_open) {
        if (g->in_list) g->focus_list = true;
        else rows::focus_first();
    }
    menu_was_open = menu_open;
    ImGui::Begin("##launcher", nullptr, f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float margin = 56 * k, nav_w = std::max(300 * k, w * 0.24f);

    // the title
    ImGui::SetCursorPos({margin, margin * 0.8f});
    ImGui::PushFont(ui::title());
    ImGui::SetWindowFontScale(1.25f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.accent));
    ImGui::TextUnformatted("SUPERWONDERS");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    ImGui::SetCursorPosX(margin);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_dim));
    ImGui::TextUnformatted(g->config_mode ? "Super Mario Bros. Wonder  -  Configuration" : "Super Mario Bros. Wonder");
    ImGui::PopStyleColor();
    const float top = ImGui::GetCursorPosY() + 40 * k;

    // the pages: a column on the left, the highlight easing in under the selected one
    const bool busy = g->job.running || g->demo_progress;
    ImGui::SetCursorPos({margin, top});
    ImGui::BeginGroup();
    bool list_focused = false;
    for (int i = 0; i < kPages; ++i) {
        ImGui::PushID(i);
        const bool sel = g->page == i;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float row_h = 64 * k;
        const float a = ui::eased(ImGui::GetID("hl"), sel ? 1.0f : 0.0f, 14.0f);
        if (a > 0.01f) {
            dl->AddRectFilled(at, {at.x + nav_w, at.y + row_h}, ui::fade(th.accent, 0.16f * a), 10 * k);
            dl->AddRectFilled(at, {at.x + 5 * k, at.y + row_h}, ui::fade(th.accent, a), 3 * k);
        }
        if (busy && i != g->page) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::fade(th.accent, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::fade(th.accent, 0.20f));
        ImGui::PushFont(ui::title());
        ImGui::SetWindowFontScale(0.62f);
        char label[64];
        std::snprintf(label, sizeof label, "   %s", kPageNames[i]);
        for (char* c = label; *c; ++c) *c = char(std::toupper(static_cast<unsigned char>(*c)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sel ? th.text : th.text_dim));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));  // (in the middle of the highlight)
        if (sel && g->focus_list && !menu_open) ImGui::SetKeyboardFocusHere(), g->focus_list = false;
        const bool clicked = ImGui::Selectable(label, sel, ImGuiSelectableFlags_None, {nav_w, row_h});
        ImGui::PopStyleVar();
        const bool focused = ImGui::IsItemFocused();
        list_focused |= focused;
        if (focused && i != g->page && !g->follow_hold)  // (the page follows the cursor)
            g->page = i, g->browse = 0, log("launcher: page %s", kPageNames[i]);
        if (clicked) {  // A (or a click): into the page
            if (i != g->page) g->page = i, g->browse = 0, log("launcher: page %s", kPageNames[i]);
            rows::focus_first();
        }
        if (focused && !menu_open) {  // the cursor, always visible: an outline on the page's name
            const ImVec2 b0 = ImGui::GetItemRectMin(), b1 = ImGui::GetItemRectMax();
            dl->AddRect(b0, b1, ui::fade(th.accent, 0.9f), 10 * k, 0, 2.5f * k);
        }
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        if (busy && i != g->page) ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndGroup();
    g->in_list = list_focused;

    // the status, lower left
    {
        const Ready r = g->status.ready;
        const ImU32 c = r == Ready::Yes ? th.good : r == Ready::Rebuild ? th.warn : th.bad;
        const char* s = r == Ready::Yes ? "Game files ready" : r == Ready::Rebuild ? "Game code needs building again"
                                                                                    : "Game files not set up";
        const float y = h - margin - 80 * k;
        dl->AddCircleFilled({margin + 8 * k, y + 15 * k}, 7 * k, c);
        ImGui::SetCursorPos({margin + 26 * k, y});
        ImGui::TextUnformatted(s);
        ImGui::SetCursorPosX(margin + 26 * k);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_off));
        ImGui::PushTextWrapPos(margin + nav_w);
        ImGui::TextUnformatted(!g->note.empty() ? g->note.c_str() : g->status.why.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    // the page, on the right
    const float px = margin + nav_w + 48 * k, pw = w - px - margin, ph = h - top - margin - 70 * k;
    ImGui::SetCursorPos({px, top});
    if (busy || (g->job.result > 0 && !g->job.was_cancelled() && g->page == PageFiles)) {
        ImGui::BeginChild("##job", {pw, ph}, ImGuiChildFlags_NavFlattened);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float card_h = 300 * k;
        if (busy) {
            progress_card(dl, p0, {p0.x + pw, p0.y + card_h});
            ImGui::SetCursorPosY(card_h + 30 * k);
            rows::begin("##cancel", ph - card_h - 30 * k);
            if (rows::action("Cancel", "", "Stop the setup. What it finished is kept, and it carries on from there "
                                           "next time.", true))
                g->job.cancel();
            rows::end();
        } else {  // the last setup failed: what it said
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.bad));
            ImGui::TextUnformatted("The setup stopped with an error:");
            ImGui::PopStyleColor();
            std::vector<std::string> tail;
            {
                std::lock_guard lk(g->job.lock);
                tail = g->job.tail;
            }
            ImGui::PushTextWrapPos(pw);
            for (const std::string& l : tail) ImGui::TextUnformatted(l.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy({0, 20 * k});
            rows::begin("##failed", 200 * k);
            if (rows::action("Back", "", "Check the folders and try again.", true)) g->job.result = -1;
            rows::end();
        }
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##page", {pw, ph}, ImGuiChildFlags_NavFlattened);
        switch (g->page.load()) {
            case PagePlay: page_play(); break;
            case PageFiles: page_files(); break;
            case PageSettings: page_settings(); break;
            case PageUpdates: page_updates(g_k); break;
            default: page_quit(); break;
        }
        ImGui::EndChild();
    }

    // the controls, along the bottom
    const ui::Hint hints[] = {{"A", "Select"}, {"B", "Back"}, {"L", ""}, {"R", "Pages"}, {"+", "Play"}};
    ui::hints(dl, w * 0.5f, h - margin * 0.7f, 34 * k, hints, 5);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // The controller, by the player's own bindings (the pad as the overlay saw it: NpadButton bits), and the keys.
    const uint64_t pad = overlay::pad_buttons(), pressed = pad & ~g->pad_before;
    g->pad_before = pad;
    if (menu_open) return;
    const bool right = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
    const bool back = (pressed & plat::kB) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (list_focused && right) rows::focus_first();  // right: into the page
    else if (!list_focused && back) {                // B: the browser closes, else back to the page list
        if (g->browse) browse_close();
        else g->focus_list = true;
    }
    if (!busy && (pressed & (plat::kL | plat::kR))) {  // L / R: the page before / after
        const int step = (pressed & plat::kR) ? 1 : kPages - 1;
        g->page = (g->page + step) % kPages, g->browse = 0, g->focus_list = true, g->follow_hold = 3;
        log("launcher: page %s", kPageNames[g->page.load()]);
    }
    if (!busy && g->status.ready == Ready::Yes && ((pressed & plat::kPlus) || ImGui::IsKeyPressed(ImGuiKey_F5, false)))
        g->play = true;  // +: play
}

bool wanted(const std::vector<std::string>& args, bool& config_mode) {
    auto has = [&](const char* a) { return std::find(args.begin(), args.end(), a) != args.end(); };
    config_mode = has("--config");
    if (has("--play")) return false;
    auto env_on = [](const char* n) {
        const char* e = std::getenv(n);
        return e && *e && std::strcmp(e, "0") != 0;
    };
    if (env_on("NX_NO_LAUNCHER") || env_on("NX_HEADLESS") || env_on("NX_INPUT_SCRIPT") || env_on("NX_INPUT_REPLAY"))
        return config_mode && !env_on("NX_HEADLESS");
    return true;
}

}  // namespace

void run() {
    if (!overlay::enabled()) return;
    bool config_mode = false;
    if (!wanted(plat::program_args(), config_mode)) return;
    static State state;
    g = &state;
    g->kit = find_kit();
    g->config = load_config();
    g->config_mode = config_mode;
    refresh_status();
    updates_init(config_mode);
    if (!config_mode && g->status.ready == Ready::Yes) return;  // set up: straight into the game
    log("launcher: shown (%s; %s; kit %s)", config_mode ? "configuration" : "setup",
        g->status.why.empty() ? "ready" : g->status.why.c_str(), g->kit.script.empty() ? "none" : g->kit.script.c_str());
    if (!config_mode) g->page = g->status.ready == Ready::Yes ? PagePlay : PageFiles;
    g->shown_ns = plat::monotonic_ns();
    overlay::set_screen(&screen);

    // Test hooks (documented here, as Savage Wind's are where they're defined):
    //   NX_LAUNCHER_TOUR=<n>   each page for n frames, then the settings menu over it, then quits (with
    //                          NX_DUMP_PRESENTED=1 NX_DUMP_PREP=<n>: the last frame of each to frames/9NNNNN.ppm)
    //   NX_LAUNCHER_SETUP=1    starts Set up game files at once, with the folders in launcher.ini (then plays, unless
    //                          --config: the configuration screen stays, as after any setup there)
    const int tour = [] {
        const char* e = std::getenv("NX_LAUNCHER_TOUR");
        return e ? std::max(0, std::atoi(e)) : 0;
    }();
    if (const char* e = std::getenv("NX_LAUNCHER_SETUP"); e && *e == '1') start_setup();
    // An update brought a kit that builds the game code differently, and the folders are known: build it again at
    // once, as Savage Wind does after an update, instead of asking for the folders again.
    else if (g->status.ready == Ready::Rebuild && folder_has(g->config.exefs, "main") &&
             folder_has(g->config.romfs, "Shader"))
        g->page = PageFiles, start_setup();
    uint64_t frames = 0;
    test_pad_start();  // (NX_TEST_PAD: a scripted controller, launcher/test_pad.h)
    for (;;) {
        const uint64_t t0 = plat::monotonic_ns();
        test_pad_frame(frames);
        plat::PadState pad;
        plat::read_pad(pad);  // (feeds the overlay's controller navigation: overlay::filter_pad)
        if (tour) {
            const int step = int(frames / uint64_t(tour));
            if (step < kPages - 1) g->page = step;  // (not Quit)
            if (step == kPages - 1 && !overlay::is_open()) g->page = PageSettings, overlay::toggle();
            if (step == kPages) {  // the setup card at 62%, its sheen sweeping
                if (overlay::is_open()) overlay::toggle();
                g->page = PageFiles, g->demo_progress = true, g->job.progress = 0.62f;
                if (!g->job.started_ns) g->job.started_ns = plat::monotonic_ns();
                std::lock_guard lk(g->job.lock);
                g->job.what = "Translating the shaders";
            }
            if (step > kPages && !updates_busy()) plat::exit_process(0);  // (an update finishes first)
        }
        if (!nvn::present_ui_frame()) plat::sleep_ns(16'000'000);
        frames++;
        if (g->quit) {
            log("launcher: quit");
            plat::exit_process(0);
        }
        if (g->play && !g->job.running) {
            refresh_status();
            if (g->status.ready == Ready::Yes) break;
            g->play = false;
        }
        const uint64_t spent = plat::monotonic_ns() - t0;
        if (spent < 16'000'000) plat::sleep_ns(int64_t(16'000'000 - spent));
    }
    log("launcher: play (%llu frames shown)", (unsigned long long)frames);
    if (overlay::is_open()) overlay::toggle();
    overlay::set_screen(nullptr);
}

}  // namespace nx::launcher
