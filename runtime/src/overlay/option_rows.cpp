// Two-pane settings pages (see option_rows.h). Ported from Savage Wind's runtime/ui/option_rows.mm (MIT, Copyright (c)
// 2026 Revivedx and the Savage Wind Recomp contributors): row(), the arrows, the description pane and the left/right
// key ownership follow it; the eased highlight and the description cross-fade are new here.
#include "overlay/option_rows.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // SetKeyOwner, SetFocusID, NavCursorVisible

#include "overlay/overlay_ui.h"

namespace nx::overlay::rows {

namespace {

float g_k = 1;  // style scale (1 at 1080 p)
std::string g_desc_hover, g_desc_focus, g_desc_shown, g_desc_first, g_note;
float g_desc_alpha = 1;
bool g_next_disabled = false;
int g_indent = 0;
bool g_focus_req = false, g_take_first = false;

// Capitals for the Latin-1 letters the menu languages use (é -> É); ß and the rest stay.
std::string upper(const char* s) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        if (*p >= 'a' && *p <= 'z') out += char(*p - 32);
        else if (*p == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) out += char(0xC3), out += char(*++p - 0x20);
        else out += char(*p);
    }
    return out;
}

bool pressed(ImGuiKey a, ImGuiKey b, ImGuiKey c) {
    return ImGui::IsKeyPressed(a) || ImGui::IsKeyPressed(b) || ImGui::IsKeyPressed(c);
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left /
// right) or 0. arrows: ◀ value ▶ (a choice of several, or a stepper), the arrows click too.
int row(const char* label, const std::string& value, bool changed, const char* desc, bool up = true, bool arrows = false,
        bool highlight = false) {
    ImGui::PushID(label);
    if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * 28 * g_k);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 1.9f;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own border shows the selection)
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const ImGuiID id = ImGui::GetItemID();
    if (g_take_first) {
        g_take_first = false;
        ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY(0);
    }
    const bool off = g_next_disabled;
    g_next_disabled = false;
    const bool hov = ImGui::IsItemHovered();
    const bool foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    if (foc && !off) {
        if (pressed(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow)) step = -1;
        if (pressed(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow)) step = 1;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, id);
    }
    if (foc) g_desc_focus = desc ? desc : "";
    if (hov) g_desc_hover = desc ? desc : "";
    if (g_desc_first.empty() && desc) g_desc_first = desc;

    // the highlight eases in (and out) over about a tenth of a second
    const float t = ui::eased(id + 1, hov || foc ? 1.0f : 0.0f, 22.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 9 * g_k, pad = 16 * g_k, fs = ImGui::GetFontSize();
    const ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, ui::fade(ui::mix(IM_COL32(0, 0, 0, 46), IM_COL32(255, 255, 255, 24), t)), r);
    const ImU32 border = ui::mix(IM_COL32(255, 255, 255, off ? 18 : 34), off ? ((ui::theme().accent) & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 120) : ui::theme().accent, t);
    dl->AddRect(p, q, ui::fade(border), r, 0, (1.0f + t) * std::max(1.0f, g_k));
    if (highlight) {  // (a row waiting for something, e.g. a key press: a slow pulse in the accent colour)
        const float a = 0.25f + 0.2f * std::sin(float(ImGui::GetTime()) * 6.0f);
        dl->AddRectFilled(p, q, ui::fade(ui::theme().accent, a), r);
    }
    std::string text = up ? upper(label) : std::string(label);
    if (const size_t cut = text.find("##"); cut != std::string::npos) text.resize(cut);  // (the ID part)
    const float ty = p.y + (h - fs) * 0.5f;
    dl->AddText(ImVec2(p.x + pad, ty), ui::fade(off ? ui::theme().text_off : ui::theme().text), text.c_str());
    float right = q.x - pad;
    if (arrows && !value.empty()) {  // ◀ value ▶, drawn (no arrow glyphs needed); a click on one steps
        const float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        const float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;
        const ImU32 ac = off ? ui::theme().text_off : ui::mix(ui::mix(ui::theme().text_off, ui::theme().text_dim, 0.5f), ui::theme().accent, t);
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ui::fade(ac));
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ui::fade(ac));
        if (clicked && !off) {
            const float mx = ImGui::GetIO().MousePos.x;
            if (mx >= lx - gap && mx <= lx + aw + gap) step = -1;
            else if (mx >= rx - gap && mx <= rx + aw + gap) step = 1;
        }
        right = rx - gap;
    }
    if (!value.empty()) {
        const float vx = right - ImGui::CalcTextSize(value.c_str()).x;
        dl->AddText(ImVec2(vx, ty), ui::fade(off ? ui::theme().text_off : changed ? ui::theme().text : ui::theme().text_dim),
                    value.c_str());
    }
    ImGui::PopID();
    return step;
}

}  // namespace

void focus_first() { g_focus_req = true; }
void disable_next() { g_next_disabled = true; }
void indent(int step) { g_indent = std::max(0, g_indent + step); }
void page_note(const char* text) { g_note = text ? text : ""; }

void begin(const char* id, float height) {
    g_k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    static std::string page;  // (another page: its first row's description, not the last page's)
    if (page != id) page = id, g_desc_shown.clear(), g_desc_alpha = 1;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear(), g_desc_first.clear(), g_note.clear();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.54f, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 5 * g_k));  // (rows close)
}

void section(const char* title) {
    const std::string t = upper(title);
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 12 * g_k));
    ImGui::SetWindowFontScale(0.82f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::theme().accent2));
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
}

bool toggle(const char* label, bool* v, bool def, const char* desc) {
    if (!row(label, *v ? "On" : "Off", *v != def, desc)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool choice(const char* label, int* v, const char* const* names, int n, int def, const char* desc, bool wrap) {
    const int cur = *v >= 0 && *v < n ? *v : 0;
    const int step = row(label, names[cur], cur != def, desc, true, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : std::clamp(nv, 0, n - 1);
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool integer(const char* label, int* v, int lo, int hi, int step, int def, const char* fmt, const char* desc) {
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, *v);
    const int s = row(label, buf, *v != def, desc, true, true);
    if (!s) return false;
    int n = *v + (s == 2 ? step : s * step);
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = std::clamp(n, lo, hi);
    if (n == *v) return false;
    *v = n;
    return true;
}

bool action(const char* label, const char* value, const char* desc, bool up, bool highlight) {
    return row(label, value ? value : "", false, desc, up, false, highlight) == 2;
}

void end() {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    std::string want = !g_desc_hover.empty() ? g_desc_hover : !g_desc_focus.empty() ? g_desc_focus
                       : !g_desc_shown.empty() ? g_desc_shown : g_desc_first;
    if (want != g_desc_shown) g_desc_shown = want, g_desc_alpha = 0;
    g_desc_alpha = ui::approach(g_desc_alpha, 1.0f, 16.0f);
    ImGui::SameLine(0, 0);
    const ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const float gap = 26 * g_k;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + avail.y),
                                        ui::fade(ui::theme().accent, 0.45f), std::max(1.0f, g_k));
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_k, 6 * g_k));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    // (the new text rises a few pixels as it fades in)
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - ui::smoothstep(g_desc_alpha)) * 8 * g_k);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * g_desc_alpha);
    ImGui::SetWindowFontScale(1.12f);  // (the description reads at a glance)
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar();
    if (!g_note.empty()) {
        ImGui::Dummy(ImVec2(0, 10 * g_k));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::theme().accent));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(g_note.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopID();
}

}  // namespace nx::overlay::rows
