// Test play tooling: input keyed to the game's frames, record and replay, a game-state probe and a memory scanner.
// Docs: docs/PLAY-TOOLS.md. Design after Savage Wind (SAVAGEWIND_INPUT "frame:button:frames", the control socket's
// `mem scan` / `mem path` and named state cells: Revivedx/SavageWindRecomp docs/CONTROL.md) and the TotK recomp
// (TOTK_INPUT_SCRIPT / TOTK_INPUT_RECORD: game ticks, a clock that restarts when the world has loaded, recordings that
// replay as scripts: docs/controls.md "Scripted input").
//
// The clock is the game's own frame count (g_frame: the game's presents, one per game update), not wall time, so a
// slower PC or a shader compile does not move an input. Loads still take a different number of frames from run to
// run, so a script is cut into segments by anchors ("@course": the frame the player appears in a course): the frames
// of a segment count from its anchor, and a segment only starts when its anchor happens.
#include "play.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nx/runtime.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace nx::play {

namespace {

using namespace plat;

// ---- reading guest memory without faulting ------------------------------------------------------------------
// The scanner and the probe read memory the game may decommit at any time: ReadProcessMemory / process_vm_readv on
// our own process fail instead of faulting.
bool safe_read(uint64_t addr, void* out, size_t n) {
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
#else
    iovec local{out, n}, remote{reinterpret_cast<void*>(addr), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
#endif
}

struct Range {
    uint64_t lo, hi;
};

// Committed, readable ranges inside [lo, hi).
std::vector<Range> readable(uint64_t lo, uint64_t hi) {
    std::vector<Range> v;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi;
    for (uint64_t a = lo; a < hi;) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mi, sizeof mi)) break;
        uint64_t b = uint64_t(mi.BaseAddress), e = b + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mi.Protect)
            v.push_back({std::max(a, b), std::min(e, hi)});
        a = e;
    }
#else
    FILE* f = std::fopen("/proc/self/maps", "r");
    char line[512];
    while (f && std::fgets(line, sizeof line, f)) {
        unsigned long long b, e;
        char perm[8];
        if (std::sscanf(line, "%llx-%llx %7s", &b, &e, perm) != 3 || perm[0] != 'r') continue;
        if (e <= lo || b >= hi) continue;
        v.push_back({std::max<uint64_t>(b, lo), std::min<uint64_t>(e, hi)});
    }
    if (f) std::fclose(f);
#endif
    // Merge neighbours.
    std::vector<Range> m;
    for (const Range& r : v)
        if (!m.empty() && m.back().hi == r.lo) m.back().hi = r.hi;
        else m.push_back(r);
    return m;
}

// Where the game keeps its objects: the heap and the alias region (nn::os maps big blocks there).
std::vector<Range> data_ranges() {
    std::vector<Range> v = readable(kHeapBase, kHeapBase + kHeapMax), a = readable(kAliasBase, kAliasBase + kAliasSize);
    v.insert(v.end(), a.begin(), a.end());
    return v;
}

// ---- the script ---------------------------------------------------------------------------------------------

enum Cmd { kPress, kSnap, kKeep, kDump, kMark, kBot };

struct Item {
    int64_t at = 0, len = 1, every = 0;
    int count = 1;
    Cmd cmd = kPress;
    uint64_t bits = 0;
    bool lset = false, rset = false;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    std::string arg;
};

struct Segment {
    std::string anchor = "boot";  // boot, course, map, goal, death, frame
    int64_t value = 0;            // frame=N: the frame; others: frames to wait after the event
    std::vector<Item> items;
};

struct Button {
    const char* name;
    uint64_t bits;
    int32_t lx, ly;
};
const Button kButtons[] = {
    {"A", kA, 0, 0},           {"B", kB, 0, 0},           {"X", kX, 0, 0},
    {"Y", kY, 0, 0},           {"L", kL, 0, 0},           {"R", kR, 0, 0},
    {"ZL", kZL, 0, 0},         {"ZR", kZR, 0, 0},         {"PLUS", kPlus, 0, 0},
    {"MINUS", kMinus, 0, 0},   {"LS", kStickL, 0, 0},     {"RS", kStickR, 0, 0},
    {"LEFT", kLeft, -kStickMax, 0}, {"RIGHT", kRight, kStickMax, 0}, {"UP", kUp, 0, kStickMax},
    {"DOWN", kDown, 0, -kStickMax},
    // The D-pad alone (no stick): the world map and menus treat them alike, the course does not.
    {"DLEFT", kLeft, 0, 0},    {"DRIGHT", kRight, 0, 0},  {"DUP", kUp, 0, 0},
    {"DDOWN", kDown, 0, 0},
};

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (true) {
        size_t e = s.find(sep, pos);
        v.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return v;
}

int32_t stick_value(const std::string& s) {
    if (s.find('.') != std::string::npos) return int32_t(std::lround(std::clamp(std::atof(s.c_str()), -1.0, 1.0) * kStickMax));
    return std::clamp(int32_t(std::atol(s.c_str())), -kStickMax, kStickMax);
}

// One item: "<at>[/<every>x<count>]:<BUTTONS>:<frames>[:<lx>,<ly>[,<rx>,<ry>]]", BUTTONS joined by '+' or "NONE";
// scanner steps "<at>:SNAP", "<at>:KEEP:inc|dec|same|changed", "<at>:DUMP"; "<at>:MARK:<text>" writes a marker.
bool parse_item(const std::string& text, Item& it) {
    std::vector<std::string> f = split(text, ':');
    if (f.size() < 2) return false;
    std::string at = f[0];
    size_t slash = at.find('/');
    if (slash != std::string::npos) {
        size_t x = at.find('x', slash);
        it.every = std::atoll(at.substr(slash + 1).c_str());
        it.count = x == std::string::npos ? 1 : std::max(1, std::atoi(at.substr(x + 1).c_str()));
        at = at.substr(0, slash);
    }
    it.at = std::atoll(at.c_str());
    const std::string b = f[1];
    if (b == "SNAP") return it.cmd = kSnap, true;
    if (b == "DUMP") return it.cmd = kDump, true;
    if (b == "KEEP" || b == "MARK" || b == "BOT") {
        it.cmd = b == "KEEP" ? kKeep : b == "MARK" ? kMark : kBot;
        it.arg = f.size() > 2 ? f[2] : "";
        return true;
    }
    if (f.size() > 2) it.len = std::max<int64_t>(1, std::atoll(f[2].c_str()));
    if (b != "NONE" && b != "-")
        for (const std::string& name : split(b, '+')) {
            bool found = false;
            for (const Button& k : kButtons)
                if (name == k.name) {
                    it.bits |= k.bits, found = true;
                    if (k.lx) it.lx = k.lx, it.lset = true;
                    if (k.ly) it.ly = k.ly, it.lset = true;
                }
            if (!found) {
                log("play: unknown button '%s' in '%s'", name.c_str(), text.c_str());
                return false;
            }
        }
    if (f.size() > 3) {
        std::vector<std::string> s = split(f[3], ',');
        if (s.size() >= 2) it.lx = stick_value(s[0]), it.ly = stick_value(s[1]), it.lset = true;
        if (s.size() >= 4) it.rx = stick_value(s[2]), it.ry = stick_value(s[3]), it.rset = true;
    }
    return true;
}

std::vector<Segment> parse_script(const std::string& all) {
    std::vector<Segment> v(1);
    std::string norm;  // comments out first, then ';' as a line break
    for (const std::string& line : split(all, '\n')) {
        const size_t hash = line.find('#');
        norm += (hash == std::string::npos ? line : line.substr(0, hash)) + "\n";
    }
    std::replace(norm.begin(), norm.end(), ';', '\n');
    for (std::string line : split(norm, '\n')) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '@') {  // @course, @course+30, @map, @goal, @death, @frame=1200
            Segment s;
            std::string a = line.substr(1);
            size_t plus = a.find('+'), eq = a.find('=');
            if (eq != std::string::npos) s.value = std::atoll(a.substr(eq + 1).c_str()), a = a.substr(0, eq);
            else if (plus != std::string::npos) s.value = std::atoll(a.substr(plus + 1).c_str()), a = a.substr(0, plus);
            s.anchor = a;
            v.push_back(s);
            continue;
        }
        Item it;
        if (parse_item(line, it)) v.back().items.push_back(it);
        else log("play: cannot read script item '%s'", line.c_str());
    }
    return v;
}

std::string read_file(const std::string& path) {
    FILE* f = fopen_utf8(path, "rb");
    if (!f) return "";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

// ---- state shared by the hooks ------------------------------------------------------------------------------

std::mutex g_lock;
std::vector<Segment> g_script;
size_t g_seg = 0;            // the active segment
int64_t g_seg_start = 0;     // the game frame its frames count from
bool g_script_on = false;
std::FILE* g_csv = nullptr;  // NX_PROBE
uint64_t g_t0 = 0;
PadState g_last_pad;         // what the game got on the last pad read
uint64_t g_last_pad_frame = UINT64_MAX;
uint32_t g_pad_reads = 0;    // pad reads this frame
std::string g_open;          // the first file opened this frame
uint32_t g_opens = 0;
std::string g_marks;         // MARK items this frame

// Game events, by the frame they happened (0: not yet).
enum Event { kEvTitle, kEvMap, kEvCourse, kEvGo, kEvGoal, kEvDeath, kEvCount };
const char* kEventNames[kEvCount] = {"title", "map", "course", "go", "goal", "death"};
int64_t g_event_frame[kEvCount] = {};
std::string g_pending_events;  // fired since the last CSV row
std::vector<int> g_rec_pending;  // events the recorder has not written yet
std::string g_scene;   // "course" / "map" / "" from the files the game loads
std::string g_course;  // the course or world it loaded

// ---- the recorder (NX_INPUT_RECORD=<path>) ------------------------------------------------------------------
// The pad the game got, per game frame, written as script items: one item per run of frames with the same state,
// and an anchor line when an anchor event happens, so the file replays as a script (NX_INPUT_REPLAY=<path>).
struct Recorder {
    std::FILE* f = nullptr;
    PadState run;
    int64_t run_from = -1, seg_start = 0, last = -1;
    static bool same(const PadState& a, const PadState& b) {
        return a.buttons == b.buttons && a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
    }
    void flush(int64_t to) {
        if (run_from < 0 || !f || to <= run_from) return;
        if (run.buttons || run.lx || run.ly || run.rx || run.ry) {
            std::string b;
            for (const Button& k : kButtons)
                if (!k.lx && !k.ly && (run.buttons & k.bits) && std::strncmp(k.name, "D", 1) != 0) b += std::string(b.empty() ? "" : "+") + k.name;
            for (const char* d : {"DLEFT", "DRIGHT", "DUP", "DDOWN"})
                for (const Button& k : kButtons)
                    if (!std::strcmp(k.name, d) && (run.buttons & k.bits)) b += std::string(b.empty() ? "" : "+") + d;
            std::fprintf(f, "%lld:%s:%lld", (long long)(run_from - seg_start), b.empty() ? "NONE" : b.c_str(), (long long)(to - run_from));
            if (run.lx || run.ly || run.rx || run.ry) {
                std::fprintf(f, ":%d,%d", run.lx, run.ly);
                if (run.rx || run.ry) std::fprintf(f, ",%d,%d", run.rx, run.ry);
            }
            std::fputc('\n', f);
            std::fflush(f);
        }
        run_from = -1;
    }
    void frame(int64_t fr, const PadState& p) {
        if (!f) return;
        if (run_from >= 0 && same(run, p)) return;
        flush(fr);
        run = p, run_from = fr;
    }
    // The new segment starts at frame `fr`, the first pad read after frame() saw the event (as on replay); a held
    // state goes on in it from its frame 0.
    void anchor(const char* name, int64_t fr) {
        if (!f) return;
        const bool held = run_from >= 0;
        flush(fr);
        std::fprintf(f, "@%s\n", name);
        std::fflush(f);
        seg_start = fr;
        if (held) run_from = fr;
    }
};
Recorder g_rec;

// Anything to do at all (otherwise the hooks return at once).
bool active() {
    static const bool on = [] {
        for (const char* k : {"NX_INPUT_FRAMES", "NX_INPUT_REPLAY", "NX_PROBE", "NX_PROBE_SCAN"})
            if (const char* e = std::getenv(k); e && *e) return true;
        const char* r = std::getenv("NX_INPUT_RECORD");
        return r && *r && std::strcmp(r, "1") != 0;
    }();
    return on;
}

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    g_t0 = monotonic_ns();
    std::string text;
    // NX_INPUT_FRAMES="<items>" or "@<file>" (when that file exists: "@course" alone is an anchor).
    if (const char* e = std::getenv("NX_INPUT_FRAMES"); e && *e) {
        const std::string file = e[0] == '@' ? read_file(e + 1) : "";
        text = !file.empty() ? file : e;
    }
    if (const char* e = std::getenv("NX_INPUT_REPLAY"); e && *e) {
        std::string r = read_file(e);
        if (r.empty()) log("play: NX_INPUT_REPLAY %s: empty or missing", e);
        text += "\n" + r;
    }
    if (!text.empty()) {
        g_script = parse_script(text);
        g_script_on = true;
        size_t n = 0;
        for (const Segment& s : g_script) n += s.items.size();
        log("play: frame script, %zu segments, %zu items", g_script.size(), n);
    }
    if (const char* e = std::getenv("NX_INPUT_RECORD"); e && *e && std::strcmp(e, "1") != 0) {
        g_rec.f = fopen_utf8(e, "w");
        if (g_rec.f) std::fprintf(g_rec.f, "# SuperWonders input recording: game frames (docs/PLAY-TOOLS.md)\n"), log("play: recording input to %s", e);
    }
    if (const char* e = std::getenv("NX_PROBE"); e && *e) {
        g_csv = fopen_utf8(e, "w");
        if (g_csv)
            std::fprintf(g_csv, "frame,t_ms,seg,rel,pad_reads,buttons,lx,ly,scene,course,player,px,py,pz,vx,vy,event,opens,open,mark\n");
    }
}

// ---- the probe: where the player is ---------------------------------------------------------------------------
// The player object is found by its vtable (the main module sits at a fixed address, so its vtables do too) and the
// position read at a fixed offset in it. Found once with the scanner below (docs/PLAY-TOOLS.md, "Finding the
// player"); NX_PROBE_PLAYER="vt=<hex>,pos=<hex>[,vel=<hex>]" overrides the built-in layout for another version.
struct Layout {
    uint64_t vt = 0;
    uint32_t pos = 0, vel = 0;
};
Layout g_layout;

// The player's track for the bot: x and y of the last frames (valid ones only), newest at g_track_n - 1.
constexpr int kTrack = 64;
float g_track_x[kTrack], g_track_y[kTrack];
int64_t g_track_n = 0;
bool g_player_valid = false;

// The bot (script item "<at>:BOT[:period=40,hold=22,dash=1]"): from that frame to the end of its segment it plays
// for you, from the probe: right (dashing with Y), a jump (B held `hold` frames; A does not jump in a course) every `period` frames on the ground and
// at once when x has not moved for 12 frames. Crude, but what it presses is recorded (NX_INPUT_RECORD) like a person's
// play, and the recording replays without it: the way to make a course route (docs/PLAY-TOOLS.md, "Making a route").
struct Bot {
    int period = 40, hold = 22;
    bool dash = true;
    int64_t jump_from = -1000, ground_since = -1;
};
Bot g_bot;
const Item* g_bot_item = nullptr;

void bot_config(const std::string& arg) {
    g_bot = Bot{};
    for (const std::string& kv : split(arg, ',')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(kv.substr(0, eq));
        const int v = std::atoi(kv.substr(eq + 1).c_str());
        if (k == "period") g_bot.period = std::max(1, v);
        else if (k == "hold") g_bot.hold = std::max(1, v);
        else if (k == "dash") g_bot.dash = v != 0;
    }
}

void bot_pad(PadState& p, int64_t fr) {
    p.buttons |= kRight | (g_bot.dash ? kY : 0);
    p.lx = kStickMax;
    if (!g_player_valid || g_track_n < 16) return;
    auto x = [](int back) { return g_track_x[(g_track_n - 1 - back) % kTrack]; };
    auto y = [](int back) { return g_track_y[(g_track_n - 1 - back) % kTrack]; };
    const bool ground = std::fabs(y(0) - y(1)) < 1e-4f && std::fabs(y(1) - y(2)) < 1e-4f;
    if (!ground) g_bot.ground_since = -1;
    else if (g_bot.ground_since < 0) g_bot.ground_since = fr;
    if (fr < g_bot.jump_from + g_bot.hold) {
        p.buttons |= kB;
        return;
    }
    if (!ground || fr < g_bot.jump_from + g_bot.hold + 3) return;  // B released for a few frames before the next
    const bool stuck = x(0) - x(12) < 0.05f;
    if (stuck || fr >= g_bot.jump_from + g_bot.period) {
        g_bot.jump_from = fr;
        p.buttons |= kB;
    }
}
std::atomic<uint64_t> g_player{0};      // the object, found by the search thread
// Where it was last: the heap is laid out the same in every run, so a new scene's player object is usually at the same
// address. Checked every frame (one read) before a search is asked for. Starts at the course 1-1 address.
uint64_t g_last_player = 0x503a5c23d8;
bool g_go_armed = false;  // after a course start, until the player first moves (the "go" event)
std::atomic<bool> g_search_wanted{false};
std::atomic<uint64_t> g_searches{0};

Layout layout() {
    // Built in, for Wonder 1.0.0 (found with tools/play/routes/scan-player.frames, docs/PLAY-TOOLS.md): the course
    // player object has the vtable main+0x96e400 (one live object; player 2's slot is another class) and its
    // position (x, y, z; blocks, y up) at +0xc00.
    Layout l{kModuleBase + 0x96e400, 0xc00, 0};
    if (const char* e = std::getenv("NX_PROBE_PLAYER")) {
        for (const std::string& kv : split(e, ',')) {
            size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(kv.substr(0, eq));
            uint64_t v = std::strtoull(kv.substr(eq + 1).c_str(), nullptr, 16);
            if (k == "vt") l.vt = v;
            else if (k == "pos") l.pos = uint32_t(v);
            else if (k == "vel") l.vel = uint32_t(v);
        }
    }
    return l;
}

// Every object with the vtable, on a background thread (a 3 GB heap takes a while; the game never waits on it).
void search_thread() {
    std::vector<uint64_t> buf(1 << 17);
    while (true) {
        while (!g_search_wanted.exchange(false)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::vector<uint64_t> found;
        for (const Range& r : data_ranges())
            for (uint64_t a = r.lo; a < r.hi; a += buf.size() * 8) {
                size_t n = size_t(std::min<uint64_t>(buf.size() * 8, r.hi - a));
                if (!safe_read(a, buf.data(), n)) continue;
                for (size_t k = 0; k < n / 8; k++)
                    if (buf[k] == g_layout.vt) found.push_back(a + k * 8);
            }
        // More than one object (the other players' slots, a copy for the goal demo): the one whose position moves
        // is picked by frame() from these; the first is a start.
        static std::mutex m;
        g_searches++;
        uint64_t pick = 0;
        for (uint64_t o : found) {
            float p[3];
            if (safe_read(o + g_layout.pos, p, sizeof p) && std::isfinite(p[0]) && std::isfinite(p[1]) && (p[0] != 0 || p[1] != 0)) {
                pick = o;
                break;
            }
        }
        g_player = pick;
        if (pick) g_last_player = pick;
        log("play: probe found %zu objects with vtable %llx, using %llx", found.size(), (unsigned long long)g_layout.vt, (unsigned long long)pick);
    }
}

// ---- the memory scanner (NX_PROBE_SCAN=<out.txt>) ------------------------------------------------------------
// Finds a value that moves with the input, like a memory scanner: SNAP keeps the data memory; KEEP:inc keeps the
// 32-bit floats that went up since the last SNAP (dec down, same unchanged, changed either); DUMP writes what is left,
// with the module pointers (vtables) before each and the references to the object around it.
struct Cand {
    uint64_t addr;
    float v;
};
struct Snap {
    Range r;
    std::vector<uint8_t> bytes;
};
std::vector<Snap> g_snap;
std::vector<Cand> g_cand;
bool g_cand_on = false;

bool keep(const std::string& how, float was, float now) {
    if (!std::isfinite(now) || std::fabs(now) > 1e6f) return false;
    if (how == "inc") return now > was + 1e-4f;
    if (how == "dec") return now < was - 1e-4f;
    if (how == "same") return now == was;
    if (how == "changed") return now != was;
    return false;
}

void scan_step(const Item& it) {
    const uint64_t t = monotonic_ns();
    if (it.cmd == kSnap) {
        if (g_cand_on) {
            for (Cand& c : g_cand) safe_read(c.addr, &c.v, 4);
        } else {
            g_snap.clear();
            uint64_t total = 0;
            for (const Range& r : data_ranges()) {
                Snap s{r, std::vector<uint8_t>(size_t(r.hi - r.lo))};
                if (safe_read(r.lo, s.bytes.data(), s.bytes.size())) total += s.bytes.size(), g_snap.push_back(std::move(s));
                else {  // a part was decommitted meanwhile: page by page
                    for (uint64_t a = r.lo; a < r.hi; a += 0x10000) {
                        Snap p{{a, std::min(r.hi, a + 0x10000)}, std::vector<uint8_t>(size_t(std::min<uint64_t>(0x10000, r.hi - a)))};
                        if (safe_read(a, p.bytes.data(), p.bytes.size())) total += p.bytes.size(), g_snap.push_back(std::move(p));
                    }
                }
            }
            log("play: scan SNAP at frame %llu: %.0f MB", (unsigned long long)g_frame.load(), double(total) / 1e6);
        }
    } else if (it.cmd == kKeep) {
        if (!g_cand_on) {
            std::vector<uint8_t> now(0x100000);
            for (const Snap& s : g_snap)
                for (size_t off = 0; off < s.bytes.size(); off += now.size()) {
                    size_t n = std::min(now.size(), s.bytes.size() - off);
                    if (!safe_read(s.r.lo + off, now.data(), n)) continue;
                    const float* a = reinterpret_cast<const float*>(s.bytes.data() + off);
                    const float* b = reinterpret_cast<const float*>(now.data());
                    for (size_t k = 0; k < n / 4; k++)
                        if (keep(it.arg, a[k], b[k])) g_cand.push_back({s.r.lo + off + k * 4, b[k]});
                }
            g_snap.clear();
            g_snap.shrink_to_fit();
            g_cand_on = true;
        } else {
            std::vector<Cand> v;
            for (const Cand& c : g_cand) {
                float now;
                if (safe_read(c.addr, &now, 4) && keep(it.arg, c.v, now)) v.push_back({c.addr, now});
            }
            g_cand.swap(v);
        }
        log("play: scan KEEP:%s at frame %llu: %zu left (%.0f ms)", it.arg.c_str(), (unsigned long long)g_frame.load(), g_cand.size(),
            double(monotonic_ns() - t) / 1e6);
    } else if (it.cmd == kDump) {
        const char* path = std::getenv("NX_PROBE_SCAN");
        FILE* f = fopen_utf8(path, "w");
        if (!f) return;
        std::fprintf(f, "# scan at frame %llu: %zu candidates\n", (unsigned long long)g_frame.load(), g_cand.size());
        // Per candidate, the 8-aligned pointers into the main module in the 0x1000 bytes before it: the vtables of the
        // object holding it (and of its parts). Then one pass over the data counts the objects with each vtable: the
        // player's class has one live object, a copy's class (bones, collision, effects) many.
        const uint64_t mod_lo = kModuleBase, mod_hi = kModuleBase + 0x10000000;
        struct Vt {
            uint64_t cand, off, vt;
        };
        std::vector<Vt> vts;
        std::vector<uint64_t> back(0x200);
        for (size_t i = 0; i < g_cand.size() && i < 2000; i++) {
            const uint64_t from = (g_cand[i].addr & ~7ull) - 0x1000;
            if (!safe_read(from, back.data(), 0x1000)) continue;
            for (int k = 0x1ff; k >= 0; k--)
                if (back[size_t(k)] >= mod_lo && back[size_t(k)] < mod_hi && (back[size_t(k)] & 7) == 0)
                    vts.push_back({g_cand[i].addr, g_cand[i].addr - (from + uint64_t(k) * 8), back[size_t(k)]});
        }
        std::vector<uint64_t> keys;
        for (const Vt& v : vts) keys.push_back(v.vt);
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        std::vector<uint32_t> count(keys.size());
        std::vector<std::vector<uint64_t>> where(keys.size());
        std::vector<uint64_t> buf(1 << 17);
        for (const Range& r : data_ranges())
            for (uint64_t a = r.lo; a < r.hi; a += buf.size() * 8) {
                const size_t n = size_t(std::min<uint64_t>(buf.size() * 8, r.hi - a));
                if (!safe_read(a, buf.data(), n)) continue;
                for (size_t k = 0; k < n / 8; k++) {
                    if (buf[k] < mod_lo || buf[k] >= mod_hi) continue;
                    auto it2 = std::lower_bound(keys.begin(), keys.end(), buf[k]);
                    if (it2 == keys.end() || *it2 != buf[k]) continue;
                    const size_t j = size_t(it2 - keys.begin());
                    if (count[j]++ < 8) where[j].push_back(a + k * 8);
                }
            }
        auto cnt = [&](uint64_t vt) { return count[size_t(std::lower_bound(keys.begin(), keys.end(), vt) - keys.begin())]; };
        for (size_t i = 0; i < g_cand.size() && i < 2000; i++) {
            float v[3] = {};
            safe_read(g_cand[i].addr, v, sizeof v);
            std::fprintf(f, "cand %llx = %g %g %g\n", (unsigned long long)g_cand[i].addr, v[0], v[1], v[2]);
            for (const Vt& w : vts)
                if (w.cand == g_cand[i].addr && cnt(w.vt) <= 16)
                    std::fprintf(f, "  -%llx: vt main+%llx, %u objects\n", (unsigned long long)w.off, (unsigned long long)(w.vt - mod_lo), cnt(w.vt));
        }
        std::fprintf(f, "\n# vtables with at most 4 objects, and the float at each candidate offset in each object\n");
        for (size_t j = 0; j < keys.size(); j++) {
            if (count[j] > 4) continue;
            std::fprintf(f, "vt main+%llx: %u objects:", (unsigned long long)(keys[j] - mod_lo), count[j]);
            for (uint64_t o : where[j]) std::fprintf(f, " %llx", (unsigned long long)o);
            std::fputc('\n', f);
            for (const Vt& w : vts)
                if (w.vt == keys[j])
                    for (uint64_t o : where[j]) {
                        float p[3] = {};
                        safe_read(o + w.off, p, sizeof p);
                        std::fprintf(f, "  off %llx in %llx: %g %g %g\n", (unsigned long long)w.off, (unsigned long long)o, p[0], p[1], p[2]);
                    }
        }
        std::fclose(f);
        log("play: scan DUMP to %s", path);
    }
}

void fire(Event e) {
    const int64_t fr = int64_t(g_frame.load());
    g_event_frame[e] = fr;
    g_pending_events += std::string(g_pending_events.empty() ? "" : "|") + kEventNames[e];
    g_rec_pending.push_back(e);  // written by frame(), where the segment starts
    log("play: event %s at frame %lld (%s)", kEventNames[e], (long long)fr, g_course.c_str());
}

}  // namespace

void file_opened(const std::string& path) {
    if (!active()) return;
    std::lock_guard lk(g_lock);
    if (g_opens++ == 0) g_open = path;
    // Scenes, by what the game loads (NX_TRACE_FS runs, docs/PLAY-TOOLS.md): a scene starts loading with
    // Stage/StageLoadInfo/<scene>.game__stage__StageLoadInfo.bgyml (Title, World001, Course001_Course...) and is
    // playable when its first music or ambience stream opens (Sound/Resource/Stream/BGM_* or Env_*). A death plays
    // BGM_Jingle_PlayerDown; the course starts again at the next stream.
    static std::string armed;  // the scene that is loading
    const size_t sli = path.find("/StageLoadInfo/");
    if (sli != std::string::npos) {
        std::string name = path.substr(sli + 15);
        name = name.substr(0, name.find('.'));
        g_course = name;
        armed = name == "Title" ? "title" : name.rfind("World", 0) == 0 ? "map" : name.rfind("Course", 0) == 0 ? "course" : "";
        g_scene = "loading";
        g_player = 0, g_track_n = 0;  // (the old scene's objects are freed)
        return;
    }
    const size_t st = path.find("/Stream/");
    if (st == std::string::npos) return;
    const std::string stream = path.substr(st + 8);
    if (stream.rfind("BGM_Jingle", 0) == 0) log("play: jingle %s at frame %llu", stream.c_str(), (unsigned long long)g_frame.load());
    if (stream.rfind("BGM_Jingle_PlayerDown", 0) == 0) {
        fire(kEvDeath);
        armed = "course";
        g_player = 0, g_track_n = 0;
        return;
    }
    if (stream.rfind("BGM_Jingle_", 0) == 0 && (stream.find("Goal") != std::string::npos || stream.find("Clear") != std::string::npos)) {
        fire(kEvGoal);
        return;
    }
    // The music, not the ambience: a course's Env_ stream opens with its "Welcome" card, ~200 frames before the
    // player can move; its BGM_Course_ stream opens as the card goes.
    const char* want = armed == "title" ? "BGM_Title" : armed == "map" ? "BGM_World" : armed == "course" ? "BGM_Course" : nullptr;
    if (want && stream.rfind(want, 0) == 0) {  // (a course load first switches the map's music: BGM_World_01)
        g_scene = armed;
        fire(armed == "title" ? kEvTitle : armed == "map" ? kEvMap : kEvCourse);
        if (armed == "course") g_go_armed = true;
        armed.clear();
    }
}

void pad(PadState& p) {
    if (!active()) return;
    std::lock_guard lk(g_lock);
    init();
    const uint64_t fr = g_frame.load();
    if (g_script_on && g_seg < g_script.size()) {
        const int64_t rel = int64_t(fr) - g_seg_start;
        for (const Item& it : g_script[g_seg].items) {
            for (int k = 0; it.cmd == kPress && k < it.count; k++) {
                const int64_t s = it.at + k * it.every;
                if (rel < s || rel >= s + it.len) continue;
                p.buttons |= it.bits;
                if (it.lset) p.lx = it.lx, p.ly = it.ly;
                if (it.rset) p.rx = it.rx, p.ry = it.ry;
            }
            if (it.cmd == kBot && rel >= it.at) {
                if (g_bot_item != &it) g_bot_item = &it, bot_config(it.arg);
                bot_pad(p, int64_t(fr));
            }
        }
    }
    if (fr != g_last_pad_frame) {
        g_last_pad_frame = fr;
        g_rec.frame(int64_t(fr), p);
    }
    g_pad_reads++;
    g_last_pad = p;
}

void frame() {
    if (!active()) return;
    std::lock_guard lk(g_lock);
    init();
    static bool started = false;
    if (!started) {
        started = true;
        g_layout = layout();
        if (g_layout.vt) std::thread(search_thread).detach();
    }
    const int64_t fr = int64_t(g_frame.load());

    // The player.
    float pos[3] = {}, vel[2] = {};
    uint64_t player = g_player.load();
    bool valid = false;
    const bool course_scene = g_scene == "course" || (g_scene == "loading" && g_course.rfind("Course", 0) == 0);
    if (g_layout.vt && !player && course_scene && g_last_player) {
        uint64_t vt = 0;
        if (safe_read(g_last_player, &vt, 8) && vt == g_layout.vt) g_player = player = g_last_player;
    }
    if (g_layout.vt && player) {
        uint64_t vt = 0;
        valid = safe_read(player, &vt, 8) && vt == g_layout.vt && safe_read(player + g_layout.pos, pos, sizeof pos) &&
                std::isfinite(pos[0]);
        if (g_layout.vel) safe_read(player + g_layout.vel, vel, sizeof vel);
    }
    g_player_valid = valid;
    // NX_PROBE_OBJ=<file>: the player object's bytes around the probe's object, every frame (to find more fields:
    // docs/PLAY-TOOLS.md, "Finding the player"). Binary: u64 frame, then 0x3000 bytes from object - 0x1000.
    static FILE* obj = [] {
        const char* e = std::getenv("NX_PROBE_OBJ");
        return e && *e ? fopen_utf8(e, "wb") : nullptr;
    }();
    if (obj && valid) {
        static std::vector<uint8_t> b(0x3000);
        if (safe_read(player - 0x1000, b.data(), b.size())) {
            const uint64_t f64 = uint64_t(fr);
            std::fwrite(&f64, 8, 1, obj), std::fwrite(b.data(), 1, b.size(), obj);
        }
    }
    if (valid) g_track_x[g_track_n % kTrack] = pos[0], g_track_y[g_track_n % kTrack] = pos[1], g_track_n++;
    // Gone and not where it was: search, in a course or its loading only (the search reads the whole heap on its own
    // thread), at most every 2 s.
    static int64_t next_search = 0;
    if (g_layout.vt && !valid && course_scene && fr >= next_search) g_search_wanted = true, next_search = fr + 120;
    // "go": after a course (re)start, the first frame the player moves after standing still for 10 frames. The game's
    // own moment (control starts, the input already held), unlike the stream that marks the course start, which comes
    // a frame or two earlier or later from run to run.
    static float still_x = 0;
    static int still = 0;
    if (!valid) still = 0;
    else if (g_go_armed && still >= 10 && pos[0] != still_x) fire(kEvGo), g_go_armed = false;
    else still = pos[0] == still_x ? still + 1 : 0, still_x = pos[0];

    // Anchors: the next segment starts when its event happened after the current one started.
    if (g_script_on)
        while (g_seg + 1 < g_script.size()) {
            const Segment& s = g_script[g_seg + 1];
            int64_t at = -1;
            if (s.anchor == "frame") at = fr >= s.value ? fr : -1;
            else if (s.anchor == "boot") at = fr;
            else
                for (int e = 0; e < kEvCount; e++)
                    if (s.anchor == kEventNames[e] && g_event_frame[e] && g_event_frame[e] >= g_seg_start && fr >= g_event_frame[e] + s.value)
                        at = std::max(g_event_frame[e] + s.value, fr);  // (earlier frames' pads were read already)
            if (at < 0) break;
            g_seg++;
            g_seg_start = at;
            log("play: segment %zu (@%s) starts at frame %lld", g_seg, s.anchor.c_str(), (long long)at);
        }

    // Scanner and marker steps of the active segment, once, at their frame.
    if (g_script_on && g_seg < g_script.size()) {
        const int64_t rel = fr - g_seg_start;
        for (const Item& it : g_script[g_seg].items) {
            if (it.cmd == kPress || it.cmd == kBot || it.at != rel) continue;
            if (it.cmd == kMark) g_marks += (g_marks.empty() ? "" : "|") + it.arg, log("play: mark %s at frame %lld", it.arg.c_str(), (long long)fr);
            else scan_step(it);
        }
    }

    if (g_csv) {
        const std::string ev = g_pending_events;
        std::string open = g_open;
        std::replace(open.begin(), open.end(), ',', ';');
        std::fprintf(g_csv, "%lld,%.3f,%zu,%lld,%u,%llx,%d,%d,%s,%s,%llx,%.3f,%.3f,%.3f,%.4f,%.4f,%s,%u,%s,%s\n", (long long)fr,
                     double(monotonic_ns() - g_t0) / 1e6, g_seg, (long long)(fr - g_seg_start), g_pad_reads,
                     (unsigned long long)g_last_pad.buttons, g_last_pad.lx, g_last_pad.ly, g_scene.c_str(), g_course.c_str(),
                     (unsigned long long)(valid ? player : 0), pos[0], pos[1], pos[2], vel[0], vel[1], ev.c_str(), g_opens,
                     open.c_str(), g_marks.c_str());
        if (fr % 60 == 0) std::fflush(g_csv);
    }
    // Anchors start at the first pad read after this point, in the recording and on replay alike ("play: anchor"
    // lines give that frame; tools/play/probe_to_frames.py rebuilds a recording from them).
    for (int e : g_rec_pending) {
        g_rec.anchor(kEventNames[e], fr);
        log("play: anchor %s from frame %lld", kEventNames[e], (long long)fr);
    }
    g_rec_pending.clear();
    g_pad_reads = 0, g_opens = 0, g_open.clear(), g_marks.clear(), g_pending_events.clear();
}

}  // namespace nx::play
