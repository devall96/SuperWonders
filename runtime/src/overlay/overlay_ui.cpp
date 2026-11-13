// The overlay's look (see overlay_ui.h). Style sizes follow Savage Wind's menu (runtime/ui/menu.mm style_init, MIT);
// the colours are SuperWonders' own and the font is Nunito (SIL OFL 1.1, third_party/fonts/Nunito).
#include "overlay/overlay_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>

#include "imgui_internal.h"  // ImGuiStorage of the current window

#include <nx/runtime.h>
#include "platform.h"

namespace nx::overlay::ui {

namespace {

// Nunito SemiBold and ExtraBold, in the program (no files to ship or find). #embed: C23, a Clang and GCC 15
// extension in C++.
#if defined(__has_embed)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#if __has_embed("../../../third_party/fonts/Nunito/Nunito-SemiBold.ttf")
#define NX_NUNITO 1
const unsigned char kBodyFont[] = {
#embed "../../../third_party/fonts/Nunito/Nunito-SemiBold.ttf"
};
const unsigned char kTitleFont[] = {
#embed "../../../third_party/fonts/Nunito/Nunito-ExtraBold.ttf"
};
#endif
#pragma clang diagnostic pop
#endif

// Fonts with the menu languages' letters, if the embedded ones are missing (ImGui's own font has ASCII only).
const char* const kSystemFonts[] = {
#if defined(_WIN32)
    "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
};

// Baked large and drawn smaller through FontGlobalScale (sharp up to 4K).
constexpr float kBodyBake = 40.0f, kTitleBake = 72.0f;
constexpr float kBody1080 = 29.0f;  // the menu rows at 1080 p (Savage Wind's size)

ImFont* g_body = nullptr;
ImFont* g_title = nullptr;

// Latin-1 plus the punctuation the four menu languages use (’ – — … •) and a few symbols.
const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201E, 0x2022, 0x2026, 0x2190, 0x2193, 0};

struct Notice {
    std::string text;
    uint64_t at_ns;
};
std::mutex g_notice_lock;
std::deque<Notice> g_notices;
constexpr double kNoticeSeconds = 3.2;

}  // namespace

ImU32 fade(ImU32 c, float a) { return ImGui::GetColorU32(c, std::clamp(a, 0.0f, 1.0f)); }

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 A = ImGui::ColorConvertU32ToFloat4(a), B = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(
        {A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t, A.z + (B.z - A.z) * t, A.w + (B.w - A.w) * t});
}

namespace {

// "Wonder Night": hot pink and sky cyan on deep indigo, after Wonder's colourful menus. The panels are dark and
// saturated so the text reads over any scene.
const Theme kTheme = {
    "Wonder Night", IM_COL32(255, 94, 168, 255), IM_COL32(86, 214, 255, 255), IM_COL32(250, 248, 255, 255),
    IM_COL32(208, 202, 230, 255), IM_COL32(116, 108, 142, 255), IM_COL32(22, 18, 54, 242), IM_COL32(28, 22, 66, 250),
    IM_COL32(10, 8, 30, 255), IM_COL32(44, 24, 88, 255), IM_COL32(110, 232, 140, 255), IM_COL32(255, 206, 72, 255),
    IM_COL32(255, 96, 112, 255)};

}  // namespace

const Theme& theme() { return kTheme; }

namespace {
ImVec4 v4(ImU32 c, float a = -1) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    if (a >= 0) v.w = a;
    return v;
}
}  // namespace

void apply_style(float height) {
    static float made_for = -1;
    const float k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    if (std::fabs(made_for - k) < 0.005f) return;
    made_for = k;
    const Theme& t = theme();
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    // (sizes: Savage Wind's menu at 1080 p)
    s.WindowRounding = 14, s.FrameRounding = 8, s.GrabRounding = 8, s.TabRounding = 8, s.ChildRounding = 10;
    s.PopupRounding = 10, s.ScrollbarRounding = 8;
    s.WindowPadding = {28, 24}, s.FramePadding = {14, 8}, s.ItemSpacing = {14, 12}, s.ItemInnerSpacing = {10, 8};
    s.WindowBorderSize = 0, s.PopupBorderSize = 1, s.ChildBorderSize = 0, s.TabBorderSize = 0;
    s.ScrollbarSize = 14, s.TabBarBorderSize = 1;
    s.DisabledAlpha = 0.45f;
    ImVec4* c = s.Colors;
    const ImVec4 accent = v4(t.accent), lift = v4(mix(t.panel, t.accent2, 0.22f), 1), lift2 = v4(mix(t.panel, t.accent2, 0.34f), 1);
    c[ImGuiCol_WindowBg] = v4(t.panel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v4(t.card, 0.98f);
    c[ImGuiCol_Border] = v4(t.accent, 0.35f);
    c[ImGuiCol_Text] = v4(t.text);
    c[ImGuiCol_TextDisabled] = v4(t.text_off);
    c[ImGuiCol_FrameBg] = v4(mix(t.panel, t.accent2, 0.12f), 1);
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_Header] = lift;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = lift2;
    c[ImGuiCol_Button] = v4(mix(t.panel, t.accent2, 0.14f), 1);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TabSelected] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);  // (the menu draws its own sliding underline)
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab], c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_NavCursor] = c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_ScrollbarGrabActive] = v4(t.accent, 0.7f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.24f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    s.ScaleAllSizes(k);
}

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_body = g_title = nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
#ifdef NX_NUNITO
    cfg.FontDataOwnedByAtlas = false;  // (static data)
    g_body = io.Fonts->AddFontFromMemoryTTF((void*)kBodyFont, int(sizeof kBodyFont), kBodyBake, &cfg, kRanges);
    g_title = io.Fonts->AddFontFromMemoryTTF((void*)kTitleFont, int(sizeof kTitleFont), kTitleBake, &cfg, kRanges);
    if (g_body) nx::log("overlay: font Nunito (embedded)");
#endif
    cfg.FontDataOwnedByAtlas = true;
    for (const char* path : kSystemFonts) {
        if (g_body) break;
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            g_body = io.Fonts->AddFontFromFileTTF(path, kBodyBake, &cfg, kRanges);
            if (g_body) g_title = io.Fonts->AddFontFromFileTTF(path, kTitleBake, &cfg, kRanges), nx::log("overlay: font %s", path);
        }
    }
    if (!g_body) {
        ImFontConfig c;
        c.SizePixels = kBodyBake;
        g_body = io.Fonts->AddFontDefault(&c);
    }
    if (!g_title) g_title = g_body;
    io.FontDefault = g_body;
}

ImFont* body() { return g_body; }
ImFont* title() { return g_title; }

float font_scale(float height) { return kBody1080 * std::clamp(height / 1080.0f, 0.5f, 3.0f) / kBodyBake; }

float approach(float current, float target, float rate) {
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    const float t = 1.0f - std::exp(-rate * dt);
    const float next = current + (target - current) * t;
    return std::fabs(target - next) < 0.002f ? target : next;
}

float eased(ImGuiID id, float target, float rate) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float* v = st->GetFloatRef(id, target);
    *v = approach(*v, target, rate);
    return *v;
}

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// ---- notices ----

void notify(const char* text) {
    if (!text || !*text) return;
    std::lock_guard lk(g_notice_lock);
    for (Notice& n : g_notices)  // the same notice again: shown longer, not twice
        if (n.text == text) {
            n.at_ns = plat::monotonic_ns();
            return;
        }
    g_notices.push_back({text, plat::monotonic_ns()});
    while (g_notices.size() > 3) g_notices.pop_front();
}

bool notices_active() {
    std::lock_guard lk(g_notice_lock);
    const uint64_t now = plat::monotonic_ns();
    while (!g_notices.empty() && double(now - g_notices.front().at_ns) / 1e9 > kNoticeSeconds) g_notices.pop_front();
    return !g_notices.empty();
}

// Savage Wind's "Picture saved" notice (menu.mm photo_overlay_draw): a dark card in the bottom-right corner that fades
// out over its last half second; here it also slides in from the right.
void draw_notices(float height) {
    std::deque<Notice> list;
    {
        std::lock_guard lk(g_notice_lock);
        list = g_notices;
    }
    if (list.empty()) return;
    const float k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = body();
    const float fs = ImGui::GetFontSize() * 0.9f, pad = 16 * k;
    float y = ds.y - 28 * k;
    const uint64_t now = plat::monotonic_ns();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const float age = float(double(now - it->at_ns) / 1e9);
        const float in = smoothstep(age / 0.22f);
        const float out = age > kNoticeSeconds - 0.5f ? 1.0f - smoothstep((age - (kNoticeSeconds - 0.5f)) / 0.5f) : 1.0f;
        const float a = std::min(in, out);
        const ImVec2 ts = f->CalcTextSizeA(fs, 640 * k, 640 * k, it->text.c_str());
        const float w = ts.x + 2 * pad + 10 * k, h = ts.y + 2 * pad * 0.8f;
        const float x = ds.x - 28 * k - w + (1.0f - in) * 40 * k;
        const ImVec2 p0(x, y - h), p1(x + w, y);
        dl->AddRectFilled(p0, p1, fade(theme().card, a * 0.95f), 10 * k);
        dl->AddRectFilled(p0, ImVec2(p0.x + 5 * k, p1.y), fade(theme().accent, a), 10 * k, ImDrawFlags_RoundCornersLeft);
        dl->AddText(f, fs, ImVec2(p0.x + pad + 5 * k, p0.y + pad * 0.8f), fade(theme().text, a), it->text.c_str(), nullptr,
                    640 * k);
        y -= h + 10 * k;
    }
}

// ---- controller hints ----

void progress_bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, float alpha) {
    const Theme& th = theme();
    const float h = b.y - a.y, r = h * 0.5f;
    dl->AddRectFilled(a, b, fade(IM_COL32(255, 255, 255, 25), alpha), r);
    float fx = a.x + (b.x - a.x) * std::clamp(frac, 0.0f, 1.0f);
    if (fx <= a.x + 1) return;
    fx = std::max(fx, a.x + h);  // (at least its two round ends)
    dl->AddRectFilled(a, {fx, b.y}, fade(th.accent, alpha), r);
    // The sheen: a band a quarter of the filled part wide (at least 4 bar heights), the bar's full height. Its
    // brightness is worked out per slice, so it is soft in the middle, clear at its edges, and fades out over the round
    // ends instead of showing a square edge there.
    const float lo = a.x + r * 0.5f, hi = fx - r * 0.5f;
    if (hi - lo < 4) return;
    const float w = std::max(4 * h, (fx - a.x) * 0.25f);
    const float t = float(ImGui::GetTime());
    const float centre = lo - w * 0.5f + std::fmod(t / 1.6f, 1.0f) * (hi - lo + w);
    auto light = [&](float x) {
        const float band = std::max(0.0f, 1.0f - std::fabs(x - centre) / (w * 0.5f));
        const float ends = std::clamp(std::min(x - lo, hi - x) / r, 0.0f, 1.0f);
        return smoothstep(band) * smoothstep(ends);
    };
    const float x0 = std::max(lo, centre - w * 0.5f), x1 = std::min(hi, centre + w * 0.5f);
    if (x1 <= x0) return;
    constexpr int kSlices = 16;
    const ImU32 lit = IM_COL32(255, 248, 220, 120);
    for (int i = 0; i < kSlices; ++i) {
        const float sa = x0 + (x1 - x0) * float(i) / kSlices, sb = x0 + (x1 - x0) * float(i + 1) / kSlices;
        const ImU32 ca = fade(lit, alpha * light(sa)), cb = fade(lit, alpha * light(sb));
        // (full height inside the straight part; over the round ends the fade has it near clear already)
        const float inset_a = std::max(0.0f, r - std::min(sa - a.x, fx - sa)) * 0.9f;
        const float inset_b = std::max(0.0f, r - std::min(sb - a.x, fx - sb)) * 0.9f;
        const float inset = std::max(inset_a, inset_b);
        dl->AddRectFilledMultiColor({sa, a.y + inset}, {sb, b.y - inset}, ca, cb, cb, ca);
    }
}

float badge(ImDrawList* dl, ImVec2 pos, float h, const char* label, float alpha) {
    ImFont* f = body();
    const float fs = h * 0.62f;
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, label);
    const float w = std::max(h, ts.x + h * 0.6f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), fade(theme().text, alpha), h * 0.5f);
    dl->AddText(f, fs, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), fade(theme().panel | IM_COL32_A_MASK, alpha),
                label);
    return w;
}

void hints(ImDrawList* dl, float centre_x, float y, float h, const Hint* list, int n, float alpha) {
    ImFont* f = body();
    const float fs = h * 0.78f, gap = h * 0.35f, spacing = h * 1.1f;
    float total = 0;
    for (int i = 0; i < n; ++i) {
        float bw = 0;
        if (list[i].badge) {
            const ImVec2 ts = f->CalcTextSizeA(h * 0.62f, FLT_MAX, 0, list[i].badge);
            bw = std::max(h, ts.x + h * 0.6f) + gap;
        }
        total += bw + f->CalcTextSizeA(fs, FLT_MAX, 0, list[i].text).x + (i + 1 < n ? (*list[i].text ? spacing : 0) : 0);
    }
    float x = centre_x - total * 0.5f;
    for (int i = 0; i < n; ++i) {
        if (list[i].badge) x += badge(dl, ImVec2(x, y - h * 0.5f), h, list[i].badge, alpha) + gap;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, list[i].text);
        dl->AddText(f, fs, ImVec2(x, y - ts.y * 0.5f), fade(theme().text_dim, alpha), list[i].text);
        x += ts.x + (*list[i].text ? spacing : 0);  // (an empty text: the next badge belongs with this one)
    }
}

}  // namespace nx::overlay::ui
