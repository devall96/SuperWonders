// The startup shader-compile screen, drawn by the overlay into frames nvn_vk.cpp presents while the startup shader
// preparation runs (prep_progress). The card follows Savage Wind's "Precompiling shaders" progress card
// (github.com/Revivedx/SavageWindRecomp, runtime/ui/launcher.mm, MIT): a title, a rounded bar, the step and
// percentage under it and a note. Here the bar glides to each new value with a moving sheen, an estimate of the time
// left is shown, and the whole screen fades out over the first frames of the game.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "imgui.h"

#include "overlay/overlay_internal.h"
#include "overlay/overlay_ui.h"

namespace nx::overlay {

namespace {

std::atomic<uint64_t> g_done{0}, g_total{0};
std::atomic<int> g_kind{0};
std::atomic<bool> g_visible{false};
std::atomic<uint64_t> g_started_ns{0};   // the first visible call of this run
std::atomic<uint64_t> g_finished_ns{0};  // when it ended (the fade-out runs from here)
constexpr double kFadeSeconds = 0.6;

double since(uint64_t t_ns) { return t_ns ? double(plat::monotonic_ns() - t_ns) / 1e9 : 0.0; }

}  // namespace

void shader_progress(uint64_t done, uint64_t total, bool visible, int kind) {
    g_done = done, g_total = total, g_kind = kind;
    if (visible && !g_visible) g_started_ns = plat::monotonic_ns(), g_finished_ns = 0;
    if (!visible && g_visible) g_finished_ns = plat::monotonic_ns();
    g_visible = visible;
}

bool progress_wanted() {
    if (g_visible) return true;
    const uint64_t end = g_finished_ns;
    return end && since(end) < kFadeSeconds;
}

void draw_progress(float height) {
    const bool visible = g_visible;
    const float k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    const float a = visible ? 1.0f : 1.0f - ui::smoothstep(float(since(g_finished_ns) / kFadeSeconds));
    if (a <= 0) return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float t = float(ImGui::GetTime());
    // the backdrop: deep violet, lighter toward the middle, with a slow glow drifting across
    const ui::Theme& th = ui::theme();
    const ImU32 top = ui::fade(th.back_top, a), mid = ui::fade(th.back_mid, a);
    dl->AddRectFilledMultiColor({0, 0}, {ds.x, ds.y * 0.5f}, top, top, mid, mid);
    dl->AddRectFilledMultiColor({0, ds.y * 0.5f}, ds, mid, mid, top, top);
    {
        const float gx = ds.x * (0.5f + 0.32f * std::sin(t * 0.35f)), gy = ds.y * 0.45f, r = ds.y * 0.55f;
        // a radial gradient: a triangle fan from a lit centre to a clear rim
        const ImU32 in = ui::fade((th.accent2 & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 34), a), out = in & ~IM_COL32_A_MASK;
        constexpr int kSeg = 64;
        dl->PrimReserve(kSeg * 3, kSeg + 1);
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
        dl->PrimWriteVtx({gx, gy}, uv, in);
        for (int i = 0; i < kSeg; ++i) {
            const float ang = float(i) / kSeg * 6.2831853f;
            dl->PrimWriteVtx({gx + std::cos(ang) * r, gy + std::sin(ang) * r}, uv, out);
        }
        for (int i = 0; i < kSeg; ++i) {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % kSeg));
        }
    }
    const uint64_t done = g_done, total = g_total;
    static float shown = 0;
    const float frac = total ? std::clamp(float(double(done) / double(total)), 0.0f, 1.0f) : 0.0f;
    shown = visible ? ui::approach(shown, frac, 5.0f) : ui::approach(shown, 1.0f, 12.0f);
    if (shown > frac + 0.001f && visible) shown = frac;  // (a new run starts at 0)

    ImFont* tf = ui::title();
    ImFont* bf = ui::body();
    const char* title = g_kind == 0 ? "Compiling shaders" : "Preparing pipelines";
    const char* const notes[] = {"This happens once, after an update or a new graphics driver.",
                                 "The game starts by itself when it is done."};
    const float tfs = 52 * k, sfs = 26 * k, line_h = bf->CalcTextSizeA(sfs, FLT_MAX, 0, "A").y;
    const ImVec2 ts = tf->CalcTextSizeA(tfs, FLT_MAX, 0, title);
    float pw = std::min(ds.x * 0.6f, 900 * k);
    for (const char* n : notes) pw = std::max(pw, bf->CalcTextSizeA(sfs, FLT_MAX, 0, n).x + 96 * k);
    pw = std::min(std::max(pw, ts.x + 96 * k), ds.x - 32 * k);
    const float bar_y = 40 * k + ts.y + 30 * k;
    const float ph = bar_y + 24 * k + 26 * k + line_h + 18 * k + 2 * line_h + 44 * k;
    const ImVec2 p0((ds.x - pw) * 0.5f, (ds.y - ph) * 0.5f), p1(p0.x + pw, p0.y + ph);
    dl->AddRectFilled({p0.x + 6 * k, p0.y + 10 * k}, {p1.x + 6 * k, p1.y + 14 * k}, ui::fade(IM_COL32(0, 0, 0, 90), a),
                      16 * k);  // (a soft shadow)
    dl->AddRectFilled(p0, p1, ui::fade(th.card, a), 14 * k);
    dl->AddRect(p0, p1, ui::fade(th.accent, a * 0.55f), 14 * k, 0, 1.5f * k);
    dl->AddText(tf, tfs, {(p0.x + p1.x - ts.x) * 0.5f, p0.y + 40 * k}, ui::fade(th.accent, a), title);
    const ImVec2 ba(p0.x + 48 * k, p0.y + bar_y), bb(p1.x - 48 * k, p0.y + bar_y + 24 * k);
    ui::progress_bar(dl, ba, bb, shown, a);
    char line[160];
    const int pct = int(shown * 100.0f + 0.5f);
    const double elapsed = since(g_started_ns);
    if (visible && done > 0 && total > done && elapsed > 3.0 && double(done) / double(total) > 0.02) {
        const double left = elapsed * double(total - done) / double(done);
        if (left >= 90) std::snprintf(line, sizeof line, "%llu / %llu   %d%%   about %d min left", (unsigned long long)done,
                                      (unsigned long long)total, pct, int(left / 60 + 0.5));
        else std::snprintf(line, sizeof line, "%llu / %llu   %d%%   less than %d s left", (unsigned long long)done,
                           (unsigned long long)total, pct, std::max(10, int(std::ceil(left / 10) * 10)));
    } else {
        std::snprintf(line, sizeof line, "%llu / %llu   %d%%", (unsigned long long)done, (unsigned long long)total, pct);
    }
    const ImVec2 ls = bf->CalcTextSizeA(sfs, FLT_MAX, 0, line);
    dl->AddText(bf, sfs, {(p0.x + p1.x - ls.x) * 0.5f, bb.y + 26 * k}, ui::fade(th.text_dim, a), line);
    for (int i = 0; i < 2; ++i) {
        const ImVec2 ns = bf->CalcTextSizeA(sfs, FLT_MAX, 0, notes[i]);
        dl->AddText(bf, sfs, {(p0.x + p1.x - ns.x) * 0.5f, bb.y + 26 * k + (float(i) + 1) * line_h + 18 * k},
                    ui::fade(ui::mix(th.text_off, th.text_dim, 0.4f), a), notes[i]);
    }
}

}  // namespace nx::overlay
