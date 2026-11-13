// The overlay's look, shared by the menu, the metrics panel, notices and the shader-compile screen: colours, fonts,
// style sizes and small animation helpers. Modelled on Savage Wind's menu (github.com/Revivedx/SavageWindRecomp,
// MIT: runtime/ui/menu.mm style_init and load_fonts, runtime/ui/launcher.mm's progress card), with Wonder's purple in
// place of its green. Everything here runs on the presenting thread, inside an ImGui frame, unless it says otherwise.
#pragma once

#include <cstdint>

#include "imgui.h"

namespace nx::overlay::ui {

// ---- palette: SuperWonders' own colours, bright like Wonder's menus (not Savage Wind's green and gold) ----
struct Theme {
    const char* name;
    ImU32 accent;   // focus borders, the title, bars, the tab underline
    ImU32 accent2;  // section titles, the second highlight
    ImU32 text, text_dim, text_off;
    ImU32 panel;       // the menu window
    ImU32 card;        // notices, the compile card
    ImU32 back_top, back_mid;  // the compile screen's backdrop
    ImU32 good, warn, bad;     // the metrics panel's numbers
};
const Theme& theme();  // "Wonder Night" (overlay_ui.cpp)

// A colour with its alpha times `a` (0..1) and the style's Alpha (the menu's fade), for hand-drawn shapes.
ImU32 fade(ImU32 c, float a = 1.0f);
ImU32 mix(ImU32 a, ImU32 b, float t);

// The style, for an image `height` pixels tall (sizes are Savage Wind's at 1080 p, scaled). Cheap when unchanged.
void apply_style(float height);
// The fonts (Nunito, embedded; system fonts if that fails). Call once, before the first frame.
void load_fonts();
ImFont* body();   // rows, text
ImFont* title();  // headings
// FontGlobalScale for an image `height` pixels tall: the body font at about 29 px at 1080 p.
float font_scale(float height);

// ---- animation ----
// `current` moved toward `target`: about 1 - e^-1 of the way every 1/rate seconds (frame-rate independent).
float approach(float current, float target, float rate);
// A value kept per ImGui ID (in the current window's storage) that eases toward `target`.
float eased(ImGuiID id, float target, float rate);
float smoothstep(float t);

// ---- notices (any thread) ----
// A short notice in the bottom-right corner for about three seconds (e.g. "Applies after restarting the game").
void notify(const char* text);
bool notices_active();
void draw_notices(float height);

// ---- progress ----
// A progress bar from a to b (its height is b.y - a.y), `frac` 0..1 filled in the accent colour, with round ends and a
// soft sheen sweeping along the filled part every 1.6 s: the bar's full height, fading out before the round ends.
void progress_bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, float alpha = 1.0f);

// ---- controller hints ----
// A button badge (a round chip with a short label, e.g. "A", "L", "+") at `pos`, `h` tall; returns its width.
float badge(ImDrawList* dl, ImVec2 pos, float h, const char* label, float alpha = 1.0f);
// A row of "[badge] text" hints, centred on `centre_x`, its middle at y.
struct Hint {
    const char* badge;  // null: text only
    const char* text;
};
void hints(ImDrawList* dl, float centre_x, float y, float h, const Hint* list, int n, float alpha = 1.0f);

}  // namespace nx::overlay::ui
