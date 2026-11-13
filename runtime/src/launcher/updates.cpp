// SuperWonders Update (launcher/updates.h): the launcher's UPDATES page, the check, and the install. After Savage Wind's SW
// Update (github.com/Revivedx/SavageWindRecomp, MIT: runtime/platform/windows_update.c, runtime/post_update.c), with its
// plain logic ported as launcher/update_core.c and its SHA-256 / P-256 as launcher/update_crypto.c.
//
// What it does, and the only things it does:
//   - asks GitHub for the list of releases of the repository in update.ini (next to the program) and picks the newest
//     one newer than this copy (VERSION) with this platform's archive: SuperWonders-v<version>-Windows-x64.zip,
//     SuperWonders-v<version>-Linux-x64.tar.gz. Pre-releases only for a copy that is one.
//   - downloads it and checks it against the SHA-256 GitHub publishes for it (required), and against its .sig when a
//     key is built in (SUPERWONDERS_UPDATE_PUBKEY, ECDSA P-256 X then Y in hex: tools/sign_update.py's --public)
//   - unpacks it to update/staging with the system's tar, and checks every file against the release's file list
//     (superwonders-manifest.txt: size, SHA-256, a safe relative path)
//   - moves the old files to update/backup and the new ones into place, keeping a journal: if anything fails, or the
//     install is cut off, the files go back as they were (at once, or at the next start)
//   - starts the new version's launcher, which builds the game code again if the new game-build kit needs it
// It never touches anything the old and new file lists don't name: the data folder (saves, settings, the game code
// built from the player's files), mods, and anything else a player keeps here are left alone.
//
// Off when there is no repository in update.ini (the default: nothing is published yet), and when a launcher installed
// this copy and updates it (update_core.c uc_launcher_installed, SUPERWONDERS_MANAGED=<name>, RETCOMM_HOME).
//
// Test hooks: SUPERWONDERS_UPDATE_REPO=<owner/name> (instead of update.ini), SUPERWONDERS_UPDATE_API=<base URL> (a local
// mock of api.github.com: tools/mock_release.py; an http URL is allowed only on 127.0.0.1 / localhost, and then the
// downloads may come from there too), SUPERWONDERS_UPDATE_AS=<version> (pretend to be that version),
// SUPERWONDERS_UPDATE_TEST=check|apply (the launcher checks, or checks and installs, at once).
#include "launcher/updates.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <nx/gamelib.h>
#include <nx/runtime.h>

#include "imgui.h"
#include "launcher/update_core.h"
#include "launcher/update_crypto.h"
#include "overlay/option_rows.h"
#include "overlay/overlay_ui.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace nx::launcher {

namespace {

namespace rows = overlay::rows;
namespace ui = overlay::ui;

#if defined(_WIN32)
constexpr const char* kSuffix = "-Windows-x64.zip";
#else
constexpr const char* kSuffix = "-Linux-x64.tar.gz";
#endif
constexpr const char* kManifest = "superwonders-manifest.txt";

fs::path u8(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string str(const fs::path& p) {
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
std::string env(const char* n) {
    const char* e = std::getenv(n);
    return e ? e : "";
}
bool read_text(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// ---- where things are ----
fs::path prog() { return u8(program_dir()); }
fs::path work() { return prog() / "update"; }

std::string current_version() {
    std::string v = env("SUPERWONDERS_UPDATE_AS");
    return v.empty() ? NX_VERSION_STRING : v;
}

std::string repo() {
    std::string r = env("SUPERWONDERS_UPDATE_REPO");
    if (!r.empty()) return r;
    std::ifstream f(prog() / "update.ini");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("repo=", 0) == 0) r = line.substr(5);
    }
    return r;
}

bool loopback(const std::string& url) {
    for (const char* p : {"http://127.0.0.1:", "http://127.0.0.1/", "http://localhost:", "http://localhost/"})
        if (url.rfind(p, 0) == 0) return true;
    return false;
}

std::string api_base() {
    std::string a = env("SUPERWONDERS_UPDATE_API");
    if (!a.empty() && loopback(a)) return a;
    if (!a.empty()) log("update: SUPERWONDERS_UPDATE_API=%s is not a local address: ignored", a.c_str());
    return "https://api.github.com";
}

// Downloads only from GitHub over HTTPS, or from the local mock when the API is one.
bool allowed_url(const std::string& url) {
    if (loopback(api_base()) && loopback(url)) return true;
    for (const char* p : {"https://github.com/", "https://objects.githubusercontent.com/",
                          "https://release-assets.githubusercontent.com/", "https://api.github.com/"})
        if (url.rfind(p, 0) == 0) return true;
    return false;
}

// ---- a launcher installed this copy? ----
std::string managed_by() {
    std::string m = env("SUPERWONDERS_MANAGED");
    if (!m.empty() && m != "0") return m;
    const fs::path dir = prog(), up = dir.parent_path();
    auto has = [](const char* d, const char* name) {
        std::error_code ec;
        return fs::exists(u8(d) / name, ec);
    };
    std::string retro = env("RETCOMM_HOME");
    if (!retro.empty()) {
        std::string d = str(dir), r = str(u8(retro) / "apps");
        std::replace(d.begin(), d.end(), '\\', '/'), std::replace(r.begin(), r.end(), '\\', '/');
        if (d.rfind(r + "/", 0) == 0) return "Retro Launcher";
    }
    const std::string rl = env("RECOMPLAUNCHER");
    const char* who = uc_launcher_installed(str(dir).c_str(), up == dir ? "" : str(up).c_str(), has, rl.c_str());
    return who ? who : "";
}

// ---- HTTP: GET into a string or a file, with progress ----
#if defined(_WIN32)
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(1, n)) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// to: a file, or null for `body`. false: why says what went wrong.
bool http_get(const std::string& url, std::string* body, const fs::path* to, std::atomic<float>* progress,
              uint64_t expect, std::string& why) {
    std::wstring w = wide(url);
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return why = "bad address " + url, false;
    HINTERNET s = WinHttpOpen(L"SuperWonders-Update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    const bool tls = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         tls ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    DWORD status = 0, size = sizeof status;
    bool ok = r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                                      DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr) &&
              WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                  &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (!ok) why = "no connection (error " + std::to_string(GetLastError()) + ")";
    else if (status != 200) ok = false, why = "the server answered " + std::to_string(status);
    std::ofstream out;
    if (ok && to) {
        out.open(*to, std::ios::binary | std::ios::trunc);
        if (!out) ok = false, why = "can't write " + str(*to);
    }
    uint64_t got = 0;
    for (DWORD n = 0; ok && WinHttpQueryDataAvailable(r, &n) && n;) {
        std::string chunk(n, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(r, chunk.data(), n, &read)) {
            ok = false, why = "the download was cut off";
            break;
        }
        chunk.resize(read);
        got += read;
        if (to) out.write(chunk.data(), std::streamsize(chunk.size()));
        else body->append(chunk);
        if (progress && expect) *progress = float(double(got) / double(expect));
    }
    for (HINTERNET h : {r, c, s})
        if (h) WinHttpCloseHandle(h);
    return ok;
}

int run_wait(const std::vector<std::string>& args) {
    std::wstring cmd;
    for (const std::string& a : args) cmd += (cmd.empty() ? L"\"" : L" \"") + wide(a) + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    return int(code);
}

void start_detached(const std::vector<std::string>& args) {
    std::wstring cmd;
    for (const std::string& a : args) cmd += (cmd.empty() ? L"\"" : L" \"") + wide(a) + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, wide(str(prog())).c_str(), &si, &pi))
        CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
}

std::string tar_program() {
    char sys[MAX_PATH] = "C:\\Windows\\System32";
    GetSystemDirectoryA(sys, MAX_PATH);
    return std::string(sys) + "\\tar.exe";  // (Windows 10 and later: bsdtar, which unpacks zips too)
}
#else
int run_wait(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void start_detached(const std::vector<std::string>& args) {
    if (fork() == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
}

// The system's curl (as Savage Wind's AppImage updater does): there is no HTTP library in the program.
bool http_get(const std::string& url, std::string* body, const fs::path* to, std::atomic<float>* progress,
              uint64_t expect, std::string& why) {
    const fs::path out = to ? *to : work() / "response.json";
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::atomic<bool> done{false};
    std::thread watch([&] {  // progress: the file's size against the size GitHub gave
        while (!done && progress && expect) {
            std::error_code e2;
            const uint64_t n = fs::exists(out, e2) ? fs::file_size(out, e2) : 0;
            *progress = float(double(n) / double(expect));
            usleep(100000);
        }
    });
    const int code = run_wait({"curl", "-fsSL", "--proto", "=https,http", "--max-time", "1800", "-H",
                               "Accept: application/vnd.github+json", "-A", "SuperWonders-Update", "-o", str(out), url});
    done = true;
    watch.join();
    if (code != 0) {
        why = code == 127 || code < 0 ? "curl isn't installed" : "the download failed (curl exit " + std::to_string(code) + ")";
        return false;
    }
    if (body && !read_text(out, *body)) return why = "can't read the answer", false;
    return true;
}

std::string tar_program() { return "tar"; }
#endif

// ---- the state the page shows ----
enum class St { Off, Managed, Idle, Checking, Current, Available, Applying, Restarting, Failed };
struct State {
    std::mutex lock;
    St st = St::Idle;
    std::string message;  // what happened / why
    std::string managed;
    UcRelease rel{};
    std::atomic<float> progress{0};
    std::string step;
    std::thread worker;
    bool auto_check = true;  // launcher.ini update_check
};
State g;

void set(St st, const std::string& msg = "") {
    std::lock_guard lk(g.lock);
    g.st = st, g.message = msg;
    log("update: %s", msg.empty() ? "state changed" : msg.c_str());
}

// ---- the journal: what moved where, so a cut-off install can be undone ----
fs::path journal_path() { return work() / "journal.txt"; }

void undo(const std::vector<std::string>& lines) {
    std::error_code ec;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string& l = *it;
        if (l.size() < 3) continue;
        const fs::path rel = u8(l.substr(2));
        if (l[0] == 'n') fs::remove(prog() / rel, ec);                                   // a new file placed: removed
        if (l[0] == 'b') fs::rename(work() / "backup" / rel, prog() / rel, ec);         // an old one moved: put back
    }
}

std::vector<std::string> read_journal() {
    std::vector<std::string> lines;
    std::ifstream f(journal_path());
    for (std::string l; std::getline(f, l);)
        if (!l.empty()) lines.push_back(l);
    return lines;
}

// At start: finish or undo an install (post_update.c in Savage Wind).
void tidy_at_start() {
    std::error_code ec;
    if (!fs::exists(journal_path(), ec)) return;
    std::vector<std::string> lines = read_journal();
    if (!lines.empty() && lines.back() == "done") {
        fs::remove_all(work(), ec);  // the new version runs: the old files and the download go
        log("update: the update to %s finished; removed the previous version's files", NX_VERSION_STRING);
    } else {
        undo(lines);  // cut off halfway (a crash, a power cut): the previous version back
        fs::remove_all(work(), ec);
        log("update: an update was cut off; the previous version was put back (%zu steps undone)", lines.size());
    }
}

// ---- the check ----
void check_now() {
    const std::string r = repo();
    if (r.empty()) return set(St::Off, "no release repository is set for this build (update.ini)");
    set(St::Checking, "checking " + r);
    std::string json, why;
    const std::string url = api_base() + "/repos/" + r + "/releases?per_page=30";
    if (!http_get(url, &json, nullptr, nullptr, 0, why)) return set(St::Failed, "couldn't check for updates: " + why);
    UcRelease rel{};
    int skipped = 0;
#ifdef SUPERWONDERS_UPDATE_PUBKEY
    const bool need_sig = true;
#else
    const bool need_sig = false;
#endif
    switch (uc_pick_release(json.c_str(), current_version().c_str(), kSuffix, need_sig, &rel, &skipped)) {
        case UC_PICK_BAD_JSON: return set(St::Failed, "the answer wasn't GitHub's list of releases");
        case UC_PICK_CURRENT:
            return set(St::Current, skipped ? "a newer release has no checksum" + std::string(need_sig ? " or signature" : "") +
                                                  ", so it isn't offered"
                                            : "this is the newest version");
        case UC_PICK_UPDATE: {
            std::lock_guard lk(g.lock);
            g.rel = rel;
        }
            return set(St::Available, std::string("version ") + rel.version + " is available");
    }
}

// ---- the install ----
bool sha256_hex(const fs::path& p, char out[65]) {
    uint8_t h[32];
    if (!uc_sha256_file(str(p).c_str(), h)) return false;
    for (int i = 0; i < 32; i++) std::snprintf(out + 2 * i, 3, "%02x", h[i]);
    return true;
}

bool fail(const std::string& why) {
    set(St::Failed, why);
    // (nothing was swapped yet when this is called before the install: the download and what it unpacked go)
    std::error_code ec;
    if (!fs::exists(journal_path(), ec)) fs::remove_all(work(), ec);
    return false;
}

bool apply_now() {
    UcRelease rel;
    {
        std::lock_guard lk(g.lock);
        rel = g.rel;
    }
    std::error_code ec;
    fs::remove_all(work(), ec);
    fs::create_directories(work() / "staging", ec);
    const fs::path archive = work() / rel.zip_name;

    // 1. download, check the checksum (and the signature, when a key is built in)
    g.step = "Downloading", g.progress = 0;
    set(St::Applying, std::string("downloading ") + rel.zip_name);
    if (!allowed_url(rel.zip_url)) return fail("the release's file isn't on GitHub: " + std::string(rel.zip_url));
    std::string why;
    if (!http_get(rel.zip_url, nullptr, &archive, &g.progress, rel.zip_size, why)) return fail("download: " + why);
    g.step = "Checking", g.progress = 1;
    char hex[65];
    if (!sha256_hex(archive, hex) || std::strcmp(hex, rel.digest) != 0)
        return fail("the download doesn't match its checksum: nothing was changed");
#ifdef SUPERWONDERS_UPDATE_PUBKEY
    {
        std::string sig;
        const fs::path sig_file = work() / "release.sig";
        uint8_t pub[64], s[64], h[32];
        if (!allowed_url(rel.sig_url) || !http_get(rel.sig_url, nullptr, &sig_file, nullptr, 0, why) ||
            !read_text(sig_file, sig))
            return fail("couldn't download the release's signature");
        while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r')) sig.pop_back();
        if (!uc_hex_decode(SUPERWONDERS_UPDATE_PUBKEY, pub, 64) || !uc_hex_decode(sig.c_str(), s, 64) ||
            !uc_sha256_file(str(archive).c_str(), h) || !uc_p256_verify(h, s, pub))
            return fail("the release's signature doesn't check out: nothing was changed");
    }
#endif

    // 2. unpack, and check every file against the release's own list
    g.step = "Unpacking";
    if (run_wait({tar_program(), "-xf", str(archive), "-C", str(work() / "staging")}) != 0)
        return fail("couldn't unpack " + std::string(rel.zip_name));
    const fs::path staged = work() / "staging" / "SuperWonders";
    std::string text;
    UcManifest next{}, prev{};
    char err[256] = "";
    if (!read_text(staged / kManifest, text) || !uc_manifest_parse(text.c_str(), &next, err, sizeof err))
        return fail("the release's file list is missing or wrong: " + std::string(err));
    g.step = "Checking files";
    for (int i = 0; i < next.count; i++) {
        const UcFile& f = next.files[i];
        const fs::path p = staged / u8(f.path);
        if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != f.size || !sha256_hex(p, hex) ||
            std::strcmp(hex, f.sha256) != 0) {
            uc_manifest_free(&next);
            return fail(std::string("a file in the release doesn't match its list: ") + f.path);
        }
        g.progress = float(i + 1) / float(next.count);
    }
    const bool have_prev = read_text(prog() / kManifest, text) && uc_manifest_parse(text.c_str(), &prev, err, sizeof err);

    // 3. the swap, journalled: old files to backup/, new ones into place (files in use are renamed, which Windows allows)
    g.step = "Installing", g.progress = 0;
    std::ofstream journal(journal_path(), std::ios::trunc);
    std::vector<std::string> done;
    auto note = [&](char kind, const std::string& rel_path) {
        done.push_back(std::string(1, kind) + " " + rel_path);
        journal << done.back() << "\n";
        journal.flush();
    };
    auto move_aside = [&](const std::string& rel_path) {
        const fs::path from = prog() / u8(rel_path);
        if (!fs::exists(from, ec)) return true;
        const fs::path to = work() / "backup" / u8(rel_path);
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (ec) return false;
        note('b', rel_path);
        return true;
    };
    std::vector<std::string> names;
    for (int i = 0; i < next.count; i++) names.push_back(next.files[i].path);
    names.push_back(kManifest);
    bool ok = true;
    for (size_t i = 0; ok && i < names.size(); i++) {
        const std::string& n = names[i];
        if (!move_aside(n)) {
            ok = fail("couldn't move " + n + " aside (is SuperWonders running twice?)");
            break;
        }
        const fs::path to = prog() / u8(n);
        fs::create_directories(to.parent_path(), ec);
        fs::rename(staged / u8(n), to, ec);
        if (ec) ok = fail("couldn't put " + n + " in place");
        else note('n', n);
        g.progress = float(i + 1) / float(names.size());
    }
    // what the previous version shipped and this one doesn't: aside too (never anything the lists don't name)
    if (ok && have_prev)
        for (int i = 0; i < prev.count; i++)
            if (!uc_manifest_find(&next, prev.files[i].path) && !move_aside(prev.files[i].path))
                log("update: couldn't move %s aside; left in place", prev.files[i].path);
    uc_manifest_free(&next);
    if (have_prev) uc_manifest_free(&prev);
    if (!ok) {
        journal.close();
        undo(done);
        fs::remove_all(work(), ec);
        return false;  // (fail() set the message; the previous version is back)
    }
    journal << "done\n";
    journal.close();
    log("update: installed %s over %s", rel.version, NX_VERSION_STRING);
    set(St::Restarting, std::string("installed ") + rel.version + ": restarting");
    return true;
}

void worker(bool apply) {
    check_now();
    bool restart = false;
    {
        std::lock_guard lk(g.lock);
        apply = apply && g.st == St::Available;
    }
    if (apply) restart = apply_now();
    if (restart) {
        // The new version's launcher (--config): it finishes the update and builds the game code if it needs to.
#if defined(_WIN32)
        start_detached({str(prog() / "wonder.exe"), "--config"});
#else
        start_detached({str(prog() / "wonder"), "--config"});
#endif
        plat::sleep_ns(300'000'000);
        plat::exit_process(0);
    }
}

void start(bool apply) {
    {
        std::lock_guard lk(g.lock);
        if (g.st == St::Checking || g.st == St::Applying || g.st == St::Restarting || g.st == St::Off ||
            g.st == St::Managed)
            return;
    }
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread(worker, apply);
}

}  // namespace

void updates_init(bool config_mode) {
    tidy_at_start();
    g.managed = managed_by();
    if (!g.managed.empty()) {
        set(St::Managed, g.managed + " installed this copy and keeps it up to date");
        return;
    }
    if (repo().empty()) {
        set(St::Off, "no release repository is set for this build (update.ini)");
        return;
    }
    const std::string test = env("SUPERWONDERS_UPDATE_TEST");
    if (test == "apply") start(true);
    else if (test == "check" || config_mode) start(false);  // (the configuration screen checks in the background)
}

bool updates_busy() {
    std::lock_guard lk(g.lock);
    return g.st == St::Checking || g.st == St::Applying || g.st == St::Restarting;
}

void page_updates(float k) {
    St st;
    std::string msg, managed;
    UcRelease rel;
    {
        std::lock_guard lk(g.lock);
        st = g.st, msg = g.message, managed = g.managed, rel = g.rel;
    }
    rows::begin("##updates", ImGui::GetContentRegionAvail().y);
    rows::section("SuperWonders Update");
    rows::action("This version", current_version().c_str(), "The version of SuperWonders you have.", true);
    switch (st) {
        case St::Off:
            rows::disable_next();
            rows::action("Updates", "Not set up", "This build has no release repository set (update.ini next to the "
                                                  "program), so it doesn't check for updates.",
                         true);
            break;
        case St::Managed: {
            const std::string d = managed + " installed this copy of SuperWonders and keeps it up to date, so "
                                            "SuperWonders Update stays out of its way: two updaters on one folder "
                                            "would fight over its files.";
            rows::disable_next();
            rows::action("Updates", managed.c_str(), d.c_str(), true);
            break;
        }
        case St::Checking:
            rows::disable_next();
            rows::action("Checking...", "", "Asking GitHub for the newest release.", true);
            break;
        case St::Available: {
            const std::string v = std::string("Version ") + rel.version, size = std::to_string(rel.zip_size >> 20) + " MB";
            const std::string d = "Downloads " + std::string(rel.zip_name) + " (" + size + "), checks it, installs it "
                                  "over this copy and restarts. Your saves, settings, mods and the game set up from your "
                                  "files stay as they are; the game code is built again only if the new version needs it.";
            if (rows::action("Update now", v.c_str(), d.c_str(), true, true)) start(true);
            if (rel.page_url[0] && rows::action("What's new", "", "The release's notes, in your browser.", true))
                plat::open_folder(rel.page_url);
            break;
        }
        case St::Applying:
        case St::Restarting: {
            rows::end();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
            ImGui::TextUnformatted(st == St::Restarting ? "Restarting..." : g.step.c_str());
            const ImVec2 a(p.x, p.y + 50 * k), b(p.x + avail.x * 0.54f, p.y + 74 * k);
            ui::progress_bar(dl, a, b, g.progress);
            return;
        }
        default:
            if (rows::action("Check for updates", "", (msg.empty() ? std::string("Ask GitHub for the newest release.")
                                                                   : "Last check: " + msg + ".").c_str(),
                             true))
                start(false);
            break;
    }
    if (st == St::Failed || st == St::Current) rows::page_note(msg.c_str());
    rows::end();
}

}  // namespace nx::launcher
