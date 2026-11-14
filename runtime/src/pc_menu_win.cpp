// A small native, translucent quick-settings panel owned by the game window.
#include "pc_menu_win.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <windowsx.h>

namespace nx::plat {
namespace {

constexpr int kWidth = 560;
constexpr int kHeight = 620;
constexpr int kSliderLeft = 58;
constexpr int kSliderRight = 502;
constexpr int kSliderY = 218;
constexpr int kTabTop = 83;
constexpr int kTabBottom = 123;
constexpr int kLanguageLeft = 382;
constexpr int kLanguageRight = 526;
constexpr int kLanguageTop = 31;
constexpr int kLanguageBottom = 66;
constexpr int kLanguageItemHeight = 27;
constexpr int kResolution[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
constexpr const wchar_t* kInternalResolution[][7] = {
    {L"Original / Dynamic", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160", L"Native (screen)"},
    {L"Originale / dinamica", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160", L"Nativa (schermo)"},
    {L"Original / Dynamisch", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160", L"Nativ (Bildschirm)"},
    {L"Originale / dynamique", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160", L"Native (écran)"},
};
constexpr const wchar_t* kLanguageNames[] = {L"English", L"Italiano", L"Deutsch", L"Français"};
constexpr int kFrameCaps[] = {30, 60, 90, 120, 144};

struct Labels {
    const wchar_t* title;
    const wchar_t* close;
    const wchar_t* video;
    const wchar_t* audio;
    const wchar_t* controls;
    const wchar_t* overlay;
    const wchar_t* window_size;
    const wchar_t* internal_resolution;
    const wchar_t* fullscreen_on;
    const wchar_t* fullscreen_off;
    const wchar_t* topmost_on;
    const wchar_t* topmost_off;
    const wchar_t* volume;
    const wchar_t* mute;
    const wchar_t* unmute;
    const wchar_t* game_language;
    const wchar_t* game_language_restart;
    const wchar_t* control_preset;
    const wchar_t* standard_desc;
    const wchar_t* nintendo_desc;
    const wchar_t* restart_note;
    const wchar_t* frame_cap;
    const wchar_t* system;
    const wchar_t* memory_limit;
    const wchar_t* low_vram;
    const wchar_t* restart_low_vram;
};

constexpr Labels kLabels[] = {
    {L"Quick Settings", L"F1 or Esc to close", L"Video", L"Audio", L"Controls", L"Overlay", L"Window / output size",
     L"Internal resolution", L"Enable fullscreen", L"Exit fullscreen", L"Always on top: On", L"Always on top: Off",
      L"Game volume", L"Mute audio", L"Unmute audio", L"Game language", L"Restart the game to apply the language.", L"Button layout",
     L"A: bottom (A / Cross)     B: right (B / Circle)", L"A: right (B / Circle)     B: bottom (A / Cross)",
     L"Internal resolution applies after restarting the game.", L"Frame rate cap", L"System", L"Memory cleanup threshold", L"Low VRAM profile", L"Restart the game to apply the texture profile."},
    {L"Impostazioni", L"F1 o Esc per chiudere", L"Video", L"Audio", L"Controlli", L"Overlay", L"Dimensione finestra / output",
     L"Risoluzione interna", L"Attiva schermo intero", L"Esci da schermo intero", L"Sempre in primo piano: sì",
      L"Sempre in primo piano: no", L"Volume gioco", L"Disattiva audio", L"Riattiva audio", L"Lingua del gioco", L"Riavvia il gioco per applicare la lingua.", L"Disposizione tasti",
     L"A: in basso (A / Croce)     B: a destra (B / Cerchio)", L"A: a destra (B / Cerchio)     B: in basso (A / Croce)",
     L"La risoluzione interna si applica al riavvio.", L"Limite FPS", L"Sistema", L"Soglia pulizia memoria", L"Profilo VRAM ridotta", L"Riavvia il gioco per applicare il profilo texture."},
    {L"Einstellungen", L"F1 oder Esc zum Schließen", L"Video", L"Audio", L"Steuerung", L"Overlay", L"Fenster- / Ausgabegröße",
     L"Interne Auflösung", L"Vollbild aktivieren", L"Vollbild beenden", L"Immer im Vordergrund: Ein",
      L"Immer im Vordergrund: Aus", L"Spiel-Lautstärke", L"Ton stummschalten", L"Ton einschalten", L"Spielsprache", L"Spiel zum Übernehmen neu starten.", L"Tastenbelegung",
     L"A: unten (A / Kreuz)     B: rechts (B / Kreis)", L"A: rechts (B / Kreis)     B: unten (A / Kreuz)",
     L"Interne Auflösung wird nach Neustart übernommen.", L"FPS-Begrenzung", L"System", L"Speicherbereinigungsschwelle", L"Niedriger VRAM-Verbrauch", L"Spiel neu starten, um das Texturprofil anzuwenden."},
    {L"Paramètres", L"F1 ou Échap pour fermer", L"Vidéo", L"Audio", L"Commandes", L"Overlay", L"Taille fenêtre / sortie",
     L"Résolution interne", L"Activer le plein écran", L"Quitter le plein écran", L"Toujours au premier plan : oui",
      L"Toujours au premier plan : non", L"Volume du jeu", L"Couper le son", L"Rétablir le son", L"Langue du jeu", L"Redémarrez le jeu pour appliquer la langue.", L"Disposition des touches",
     L"A : bas (A / Croix)     B : droite (B / Cercle)", L"A : droite (B / Cercle)     B : bas (A / Croix)",
     L"La résolution interne s'applique après redémarrage.", L"Limite d’images/s", L"Système", L"Seuil de nettoyage mémoire", L"Profil VRAM réduit", L"Redémarrez pour appliquer le profil de textures."},
};

constexpr const wchar_t* kOverlayLabels[4][6] = {
    {L"Metrics overlay", L"FPS", L"1% lows", L"Game speed", L"Frametime graph", L"Overlay position"},
    {L"Overlay metriche", L"FPS", L"Minimi 1%", L"Velocità gioco", L"Grafico frametime", L"Posizione overlay"},
    {L"Metrik-Overlay", L"FPS", L"1%-Tiefstwerte", L"Spieltempo", L"Frametime-Diagramm", L"Overlay-Position"},
    {L"Superposition métriques", L"FPS", L"1 % les plus bas", L"Vitesse du jeu", L"Graphique frametime", L"Position de l’overlay"},
};
constexpr const wchar_t* kToggleLabels[4][2] = {
    {L"Off", L"On"}, {L"Disattivo", L"Attivo"}, {L"Aus", L"Ein"}, {L"Désactivé", L"Activé"},
};
constexpr const wchar_t* kOverlayPositions[4][4] = {
    {L"Top left", L"Top right", L"Bottom left", L"Bottom right"},
    {L"In alto a sinistra", L"In alto a destra", L"In basso a sinistra", L"In basso a destra"},
    {L"Oben links", L"Oben rechts", L"Unten links", L"Unten rechts"},
    {L"En haut à gauche", L"En haut à droite", L"En bas à gauche", L"En bas à droite"},
};
constexpr const wchar_t* kControlHeaders[4][2] = {
    {L"Keyboard", L"Controller"}, {L"Tastiera", L"Controller"}, {L"Tastatur", L"Controller"},
    {L"Clavier", L"Manette"},
};
constexpr const wchar_t* kControlActions[kPcControlCount] = {
    L"A", L"B", L"X", L"Y", L"L", L"R", L"ZL", L"ZR", L"Plus", L"Minus", L"L Stick", L"R Stick",
    L"D-pad Left", L"D-pad Up", L"D-pad Right", L"D-pad Down",
};
constexpr const wchar_t* kPhysicalNames[kPcControlCount] = {
    L"A / Cross", L"B / Circle", L"X / Square", L"Y / Triangle", L"LB / L1", L"RB / R1", L"LT / L2",
    L"RT / R2", L"Start / +", L"Back / -", L"L3 / LS", L"R3 / RS", L"D-pad Left", L"D-pad Up",
    L"D-pad Right", L"D-pad Down",
};

enum class Section { Video, Audio, Controls, Overlay, System };
enum class CaptureDevice { None, Keyboard, Controller };

HWND g_owner = nullptr;
HWND g_menu = nullptr;
PcMenuState g_state;
PcMenuApply g_apply = nullptr;
std::atomic<bool> g_open{false};
std::atomic<int> g_controller_preset{0};
Section g_section = Section::Video;
bool g_dragging_volume = false;
bool g_language_dropdown = false;
bool g_game_language_dropdown = false;
bool g_overlay_position_dropdown = false;
CaptureDevice g_capture_device = CaptureDevice::None;
int g_capture_action = -1;
constexpr UINT kCaptureControllerMessage = WM_APP + 41;

const Labels& labels() { return kLabels[std::clamp(g_state.language, 0, 3)]; }

void notify(bool persist) {
    if (g_apply) g_apply(g_state, persist);
    if (g_menu) InvalidateRect(g_menu, nullptr, FALSE);
}

void set_volume_from_x(int x) {
    const int clamped = std::clamp(x, kSliderLeft, kSliderRight);
    g_state.volume = (clamped - kSliderLeft) * 100 / (kSliderRight - kSliderLeft);
    notify(false);
}

void paint_text(HDC dc, const wchar_t* value, RECT r, COLORREF color, int size, UINT align = DT_LEFT) {
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, value, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    DeleteObject(font);
}

void fill(HDC dc, RECT r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

void rounded(HDC dc, RECT r, COLORREF color, COLORREF outline, int radius = 10) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void paint_tab(HDC dc, RECT r, const wchar_t* text, bool selected) {
    rounded(dc, r, selected ? RGB(52, 84, 126) : RGB(35, 43, 61),
            selected ? RGB(102, 174, 255) : RGB(68, 81, 105), 9);
    paint_text(dc, text, r, RGB(238, 243, 252), 15, DT_CENTER);
}

void paint_arrow(HDC dc, int y, const wchar_t* value) {
    paint_text(dc, value, RECT{285, y - 2, 407, y + 32}, RGB(245, 247, 255), 15, DT_CENTER);
    rounded(dc, RECT{416, y, 454, y + 32}, RGB(45, 55, 75), RGB(92, 108, 136));
    rounded(dc, RECT{466, y, 504, y + 32}, RGB(45, 55, 75), RGB(92, 108, 136));
    paint_text(dc, L"<", RECT{416, y, 454, y + 32}, RGB(245, 247, 255), 19, DT_CENTER);
    paint_text(dc, L">", RECT{466, y, 504, y + 32}, RGB(245, 247, 255), 19, DT_CENTER);
}

void paint_video(HDC dc) {
    const Labels& text = labels();
    paint_text(dc, text.window_size, RECT{44, 148, 385, 176}, RGB(214, 222, 238), 16);
    wchar_t resolution[80];
    std::swprintf(resolution, 80, L"%d x %d", g_state.width, g_state.height);
    paint_arrow(dc, 183, resolution);

    paint_text(dc, text.internal_resolution, RECT{44, 235, 385, 263}, RGB(214, 222, 238), 16);
    paint_arrow(dc, 270, kInternalResolution[std::clamp(g_state.language, 0, 3)]
                                           [std::clamp(g_state.internal_resolution, 0, 6)]);

    paint_text(dc, text.frame_cap, RECT{44, 300, 385, 328}, RGB(214, 222, 238), 16);
    wchar_t cap[32];
    std::swprintf(cap, 32, L"%d FPS", g_state.frame_cap);
    paint_arrow(dc, 332, cap);

    rounded(dc, RECT{44, 375, 516, 419}, RGB(35, 43, 61), RGB(68, 81, 105));
    paint_text(dc, g_state.fullscreen ? text.fullscreen_off : text.fullscreen_on, RECT{58, 375, 502, 419},
               RGB(230, 235, 245), 15, DT_CENTER);
    rounded(dc, RECT{44, 430, 516, 474}, RGB(35, 43, 61), RGB(68, 81, 105));
    paint_text(dc, g_state.always_on_top ? text.topmost_on : text.topmost_off, RECT{58, 430, 502, 474},
               RGB(230, 235, 245), 15, DT_CENTER);
    paint_text(dc, text.restart_note, RECT{35, 493, 525, 522}, RGB(155, 169, 192), 12, DT_CENTER);
}

void paint_audio(HDC dc) {
    const Labels& text = labels();
    paint_text(dc, text.volume, RECT{50, 160, 415, 192}, RGB(214, 222, 238), 17);
    wchar_t volume[24];
    std::swprintf(volume, 24, L"%d%%", g_state.volume);
    paint_text(dc, volume, RECT{420, 160, 510, 192}, RGB(245, 247, 255), 16, DT_RIGHT);
    fill(dc, RECT{kSliderLeft, kSliderY - 2, kSliderRight, kSliderY + 2}, RGB(83, 98, 124));
    const int knob_x = kSliderLeft + (kSliderRight - kSliderLeft) * g_state.volume / 100;
    fill(dc, RECT{kSliderLeft, kSliderY - 2, knob_x, kSliderY + 2}, RGB(102, 174, 255));
    HBRUSH knob = CreateSolidBrush(RGB(230, 240, 255));
    HGDIOBJ old = SelectObject(dc, knob);
    Ellipse(dc, knob_x - 8, kSliderY - 8, knob_x + 8, kSliderY + 8);
    SelectObject(dc, old);
    DeleteObject(knob);

    rounded(dc, RECT{44, 272, 516, 320}, RGB(35, 43, 61), RGB(68, 81, 105));
    paint_text(dc, g_state.muted ? text.unmute : text.mute, RECT{58, 272, 502, 320},
               g_state.muted ? RGB(255, 190, 145) : RGB(230, 235, 245), 15, DT_CENTER);

    rounded(dc, RECT{44, 340, 516, 384}, RGB(35, 43, 61), RGB(68, 81, 105));
    paint_text(dc, text.game_language, RECT{58, 340, 285, 384}, RGB(230, 235, 245), 14);
    paint_text(dc, kLanguageNames[std::clamp(g_state.game_language, 0, 3)], RECT{298, 340, 478, 384},
               RGB(238, 243, 252), 14, DT_RIGHT | DT_VCENTER);
    paint_text(dc, g_game_language_dropdown ? L"▴" : L"▾", RECT{480, 340, 502, 384},
               RGB(238, 243, 252), 14, DT_CENTER | DT_VCENTER);
    paint_text(dc, text.game_language_restart, RECT{35, 395, 525, 422}, RGB(155, 169, 192), 12, DT_CENTER);
}

void paint_controls(HDC dc) {
    const bool standard = std::clamp(g_state.controller_preset, 0, 1) == 0;
    rounded(dc, RECT{44, 143, 272, 173}, standard ? RGB(43, 70, 105) : RGB(35, 43, 61),
            standard ? RGB(102, 174, 255) : RGB(68, 81, 105), 8);
    paint_text(dc, L"Xbox / DualSense", RECT{50, 143, 266, 173}, RGB(238, 243, 252), 13, DT_CENTER);
    rounded(dc, RECT{288, 143, 516, 173}, !standard ? RGB(43, 70, 105) : RGB(35, 43, 61),
            !standard ? RGB(102, 174, 255) : RGB(68, 81, 105), 8);
    paint_text(dc, L"Nintendo", RECT{294, 143, 510, 173}, RGB(238, 243, 252), 13, DT_CENTER);
    const int language = std::clamp(g_state.language, 0, 3);
    paint_text(dc, kControlHeaders[language][0], RECT{155, 177, 327, 198}, RGB(177, 194, 218), 12, DT_CENTER);
    paint_text(dc, kControlHeaders[language][1], RECT{331, 177, 515, 198}, RGB(177, 194, 218), 12, DT_CENTER);
    for (size_t i = 0; i < kPcControlCount; ++i) {
        const int top = 200 + int(i) * 22;
        if (i & 1) fill(dc, RECT{38, top, 522, top + 21}, RGB(27, 34, 49));
        paint_text(dc, kControlActions[i], RECT{46, top, 148, top + 21}, RGB(218, 227, 241), 13);

        const bool capture_key = g_capture_device == CaptureDevice::Keyboard && g_capture_action == int(i);
        rounded(dc, RECT{155, top + 1, 327, top + 20}, capture_key ? RGB(70, 55, 35) : RGB(35, 43, 61),
                capture_key ? RGB(255, 190, 105) : RGB(68, 81, 105), 5);
        wchar_t key_name[64] = L"Unbound";
        if (capture_key) {
            wcscpy_s(key_name, L"Press a key... (Esc cancels)");
        } else if (g_state.keyboard_bindings[i] > 0) {
            const UINT scan = MapVirtualKeyW(UINT(g_state.keyboard_bindings[i]), MAPVK_VK_TO_VSC);
            if (!GetKeyNameTextW(LONG(scan << 16), key_name, int(std::size(key_name))))
                std::swprintf(key_name, std::size(key_name), L"Key %d", g_state.keyboard_bindings[i]);
        }
        paint_text(dc, key_name, RECT{159, top + 1, 323, top + 20}, RGB(235, 241, 251), 12, DT_CENTER);

        const bool capture_pad = g_capture_device == CaptureDevice::Controller && g_capture_action == int(i);
        rounded(dc, RECT{331, top + 1, 515, top + 20}, capture_pad ? RGB(70, 55, 35) : RGB(35, 43, 61),
                capture_pad ? RGB(255, 190, 105) : RGB(68, 81, 105), 5);
        const int binding = (g_state.controller_custom_mask & (1u << i))
                                ? g_state.controller_bindings[i]
                                : pc_default_controller_binding(i, g_state.controller_preset);
        const wchar_t* pad_name = capture_pad ? L"Press a button..." :
                                  binding >= 0 && binding < int(kPcControlCount) ? kPhysicalNames[binding] : L"Unbound";
        paint_text(dc, pad_name, RECT{335, top + 1, 511, top + 20}, RGB(235, 241, 251), 12, DT_CENTER);
    }
}

void paint_overlay_toggle(HDC dc, RECT r, int label_index, bool active) {
    rounded(dc, r, active ? RGB(43, 70, 105) : RGB(35, 43, 61),
            active ? RGB(102, 174, 255) : RGB(68, 81, 105), 9);
    const int language = std::clamp(g_state.language, 0, 3);
    paint_text(dc, kOverlayLabels[language][label_index], RECT{r.left + 14, r.top, r.right - 115, r.bottom},
               RGB(230, 235, 245), 15);
    paint_text(dc, kToggleLabels[language][active ? 1 : 0], RECT{r.right - 108, r.top, r.right - 14, r.bottom},
               active ? RGB(145, 207, 255) : RGB(170, 181, 198), 14, DT_RIGHT);
}

void paint_overlay(HDC dc) {
    paint_overlay_toggle(dc, RECT{44, 144, 516, 190}, 0, g_state.metrics_enabled);
    paint_overlay_toggle(dc, RECT{44, 199, 516, 241}, 1, g_state.metric_fps);
    paint_overlay_toggle(dc, RECT{44, 247, 516, 289}, 2, g_state.metric_one_percent_low);
    paint_overlay_toggle(dc, RECT{44, 295, 516, 337}, 3, g_state.metric_game_speed);
    paint_overlay_toggle(dc, RECT{44, 343, 516, 385}, 4, g_state.metric_frametime_graph);
    const int language = std::clamp(g_state.language, 0, 3);
    paint_text(dc, kOverlayLabels[language][5], RECT{44, 395, 516, 421}, RGB(214, 222, 238), 15);
    rounded(dc, RECT{44, 426, 516, 470}, RGB(35, 43, 61), RGB(92, 108, 136), 8);
    paint_text(dc, kOverlayPositions[language][std::clamp(g_state.overlay_position, 0, 3)],
               RECT{58, 426, 482, 470}, RGB(238, 243, 252), 15, DT_CENTER);
    paint_text(dc, g_overlay_position_dropdown ? L"▴" : L"▾", RECT{480, 426, 502, 470}, RGB(238, 243, 252), 14,
               DT_CENTER);
}

void paint_paragraph(HDC dc, const wchar_t* value, RECT r, COLORREF color, int size) {
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, value, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(dc, old);
    DeleteObject(font);
}

int memory_limit_choice(int current, int direction) {
    constexpr int choices[] = {2, 4, 6, 8, 10, 12, 16, 24, 28, 32};
    int index = 3;
    for (int i = 0; i < int(std::size(choices)); ++i) if (choices[i] == current) index = i;
    index = (index + direction + int(std::size(choices))) % int(std::size(choices));
    return choices[index];
}

void paint_system(HDC dc) {
    const Labels& text = labels();
    MEMORYSTATUSEX status{sizeof(status)};
    const bool known = GlobalMemoryStatusEx(&status) != FALSE;
    const int installed_gb = known ? int((status.ullTotalPhys + (1ull << 30) - 1) >> 30) : 0;
    const int recommended = known ? std::max(2, installed_gb - 4) : 0;
    paint_text(dc, text.memory_limit, RECT{44, 148, 390, 176}, RGB(214, 222, 238), 16);
    wchar_t value[48];
    std::swprintf(value, 48, L"%d GB", g_state.memory_limit_gb);
    paint_arrow(dc, 183, value);
    wchar_t guidance[180];
    if (known) std::swprintf(guidance, 180, L"Detected system RAM: %d GB. Suggested starting limit: %d GB (leaves about 4 GB for Windows and other apps).",
                            installed_gb, recommended);
    else wcscpy_s(guidance, L"System RAM could not be detected. Leave several GB for Windows and other apps.");
    paint_paragraph(dc, guidance, RECT{48, 232, 510, 302}, RGB(170, 187, 210), 14);
    paint_paragraph(dc, L"When usage passes this threshold, Wonder asks Vulkan to release its unused reusable-image cache and keeps running. This is not a hard cap and cannot free textures the game is actively using. It is not a direct GPU VRAM limit.",
                    RECT{48, 306, 510, 382}, RGB(145, 162, 187), 13);
    rounded(dc, RECT{44, 397, 516, 443}, g_state.low_vram_profile ? RGB(43, 70, 105) : RGB(35, 43, 61),
            g_state.low_vram_profile ? RGB(102, 174, 255) : RGB(68, 81, 105));
    paint_text(dc, text.low_vram, RECT{58, 397, 394, 443}, RGB(230, 235, 245), 15);
    paint_text(dc, kToggleLabels[std::clamp(g_state.language, 0, 3)][g_state.low_vram_profile ? 1 : 0],
               RECT{400, 397, 502, 443}, RGB(145, 207, 255), 14, DT_RIGHT);
    paint_paragraph(dc, L"When ASTC is not supported by the GPU, store decoded ASTC textures as BC3 instead of RGBA8. This can greatly reduce texture memory; image quality may be slightly lower. Restart required.",
                    RECT{48, 455, 510, 520}, RGB(145, 162, 187), 13);
    paint_text(dc, text.restart_low_vram, RECT{35, 535, 525, 560}, RGB(210, 185, 135), 12, DT_CENTER);
}

void paint(HWND h) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    fill(dc, RECT{0, 0, kWidth, kHeight}, RGB(20, 25, 38));
    rounded(dc, RECT{10, 10, kWidth - 10, kHeight - 10}, RGB(24, 30, 45), RGB(70, 82, 105), 18);

    const Labels& text = labels();
    paint_text(dc, text.title, RECT{32, 26, 365, 68}, RGB(245, 247, 255), 24);
    rounded(dc, RECT{kLanguageLeft, kLanguageTop, kLanguageRight, kLanguageBottom}, RGB(35, 43, 61), RGB(92, 108, 136));
    paint_text(dc, kLanguageNames[std::clamp(g_state.language, 0, 3)],
               RECT{kLanguageLeft + 8, kLanguageTop, kLanguageRight - 28, kLanguageBottom}, RGB(238, 243, 252), 14,
               DT_CENTER);
    paint_text(dc, g_language_dropdown ? L"▴" : L"▾", RECT{kLanguageRight - 28, kLanguageTop,
               kLanguageRight - 6, kLanguageBottom}, RGB(238, 243, 252), 14, DT_CENTER);

    paint_tab(dc, RECT{12, kTabTop, 116, kTabBottom}, text.video, g_section == Section::Video);
    paint_tab(dc, RECT{120, kTabTop, 224, kTabBottom}, text.audio, g_section == Section::Audio);
    paint_tab(dc, RECT{228, kTabTop, 332, kTabBottom}, text.controls, g_section == Section::Controls);
    paint_tab(dc, RECT{336, kTabTop, 440, kTabBottom}, text.overlay, g_section == Section::Overlay);
    paint_tab(dc, RECT{444, kTabTop, 548, kTabBottom}, text.system, g_section == Section::System);

    if (g_section == Section::Video) paint_video(dc);
    else if (g_section == Section::Audio) paint_audio(dc);
    else if (g_section == Section::Controls) paint_controls(dc);
    else if (g_section == Section::Overlay) paint_overlay(dc);
    else paint_system(dc);

    paint_text(dc, text.close, RECT{35, 586, 525, 608}, RGB(140, 154, 177), 12, DT_CENTER);

    if (g_language_dropdown) {
        for (int i = 0; i < int(std::size(kLanguageNames)); ++i) {
            const int top = kLanguageBottom + 2 + i * kLanguageItemHeight;
            rounded(dc, RECT{kLanguageLeft, top, kLanguageRight, top + kLanguageItemHeight},
                    i == g_state.language ? RGB(52, 84, 126) : RGB(35, 43, 61), RGB(92, 108, 136), 6);
            paint_text(dc, kLanguageNames[i], RECT{kLanguageLeft + 8, top, kLanguageRight - 8, top + kLanguageItemHeight},
                       RGB(238, 243, 252), 14, DT_CENTER);
        }
    }
    if (g_overlay_position_dropdown && g_section == Section::Overlay) {
        const int language = std::clamp(g_state.language, 0, 3);
        for (int i = 0; i < 4; ++i) {
            const int top = 472 + i * 27;
            rounded(dc, RECT{44, top, 516, top + 27}, i == g_state.overlay_position ? RGB(52, 84, 126) : RGB(35, 43, 61),
                    RGB(92, 108, 136), 5);
            paint_text(dc, kOverlayPositions[language][i], RECT{52, top, 508, top + 27}, RGB(238, 243, 252), 13,
                       DT_CENTER);
        }
    }
    if (g_game_language_dropdown && g_section == Section::Audio) {
        for (int i = 0; i < int(std::size(kLanguageNames)); ++i) {
            const int top = 386 + i * kLanguageItemHeight;
            rounded(dc, RECT{44, top, 516, top + kLanguageItemHeight},
                    i == g_state.game_language ? RGB(52, 84, 126) : RGB(35, 43, 61), RGB(92, 108, 136), 6);
            paint_text(dc, kLanguageNames[i], RECT{52, top, 508, top + kLanguageItemHeight},
                       RGB(238, 243, 252), 14, DT_CENTER | DT_VCENTER);
        }
    }
    EndPaint(h, &ps);
}

void set_controller_preset(int preset) {
    g_state.controller_preset = std::clamp(preset, 0, 1);
    // Selecting a preset resets custom mappings; rows can be customized again afterward.
    g_state.controller_custom_mask = 0;
    g_controller_preset.store(g_state.controller_preset, std::memory_order_relaxed);
    notify(true);
}

void close_menu() {
    g_open = false;
    g_language_dropdown = false;
    g_game_language_dropdown = false;
    g_overlay_position_dropdown = false;
    g_capture_device = CaptureDevice::None;
    g_capture_action = -1;
    if (g_menu) ShowWindow(g_menu, SW_HIDE);
    if (g_owner) SetForegroundWindow(g_owner);
}

LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case kCaptureControllerMessage:
            if (g_capture_device == CaptureDevice::Controller && g_capture_action >= 0 &&
                w < kPcControlCount) {
                const size_t action = size_t(g_capture_action);
                g_state.controller_bindings[action] = int(w);
                g_state.controller_custom_mask |= 1u << action;
                g_capture_device = CaptureDevice::None;
                g_capture_action = -1;
                notify(true);
            }
            return 0;
        case WM_PAINT:
            paint(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
            if (g_capture_device == CaptureDevice::Keyboard && g_capture_action >= 0) {
                if (w == VK_ESCAPE) {
                    g_capture_device = CaptureDevice::None;
                    g_capture_action = -1;
                    InvalidateRect(h, nullptr, FALSE);
                } else if (!(l & (1ll << 30)) && w < 256) {
                    g_state.keyboard_bindings[size_t(g_capture_action)] = int(w);
                    g_capture_device = CaptureDevice::None;
                    g_capture_action = -1;
                    notify(true);
                }
                return 0;
            }
            if (w == VK_F1 || w == VK_ESCAPE) {
                close_menu();
                return 0;
            }
            break;
        case WM_SYSKEYDOWN:
            if (g_capture_device == CaptureDevice::Keyboard && g_capture_action >= 0 && w < 256) {
                g_state.keyboard_bindings[size_t(g_capture_action)] = int(w);
                g_capture_device = CaptureDevice::None;
                g_capture_action = -1;
                notify(true);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (g_overlay_position_dropdown && g_section == Section::Overlay && x >= 44 && x <= 516 && y >= 472 && y < 580) {
                g_state.overlay_position = (y - 472) / 27;
                g_overlay_position_dropdown = false;
                notify(true);
                return 0;
            }
            if (g_game_language_dropdown && g_section == Section::Audio && x >= 44 && x <= 516 && y >= 386 && y < 386 + int(std::size(kLanguageNames)) * kLanguageItemHeight) {
                g_state.game_language = (y - 386) / kLanguageItemHeight;
                g_game_language_dropdown = false;
                notify(true);
                return 0;
            }
            if (g_language_dropdown && x >= kLanguageLeft && x <= kLanguageRight &&
                y >= kLanguageBottom + 2 && y < kLanguageBottom + 2 + int(std::size(kLanguageNames)) * kLanguageItemHeight) {
                g_state.language = (y - kLanguageBottom - 2) / kLanguageItemHeight;
                g_language_dropdown = false;
                notify(true);
                return 0;
            }
            if (x >= kLanguageLeft && x <= kLanguageRight && y >= kLanguageTop && y <= kLanguageBottom) {
                g_language_dropdown = !g_language_dropdown;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (g_language_dropdown) {
                g_language_dropdown = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (g_game_language_dropdown && !(g_section == Section::Audio && y >= 340 && y <= 384)) {
                g_game_language_dropdown = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (g_overlay_position_dropdown && !(g_section == Section::Overlay && y >= 426 && y <= 470)) {
                g_overlay_position_dropdown = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (y >= kTabTop && y <= kTabBottom) {
                if (x >= 12 && x <= 116) g_section = Section::Video;
                else if (x >= 120 && x <= 224) g_section = Section::Audio;
                else if (x >= 228 && x <= 332) g_section = Section::Controls;
                else if (x >= 336 && x <= 440) g_section = Section::Overlay;
                else if (x >= 444 && x <= 548) g_section = Section::System;
                g_capture_device = CaptureDevice::None;
                g_capture_action = -1;
                g_game_language_dropdown = false;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (g_section == Section::System && y >= 183 && y <= 215 && x >= 416 && x <= 504) {
                g_state.memory_limit_gb = memory_limit_choice(g_state.memory_limit_gb, x < 460 ? -1 : 1);
                notify(true);
            } else if (g_section == Section::System && y >= 397 && y <= 443) {
                g_state.low_vram_profile = !g_state.low_vram_profile;
                notify(true);
            } else if (g_section == Section::Overlay && y >= 426 && y <= 470 && x >= 44 && x <= 516) {
                g_overlay_position_dropdown = !g_overlay_position_dropdown;
                InvalidateRect(h, nullptr, FALSE);
            } else if (g_section == Section::Overlay && y >= 144 && y <= 190) {
                g_state.metrics_enabled = !g_state.metrics_enabled;
                notify(true);
            } else if (g_section == Section::Overlay && y >= 199 && y <= 241) {
                g_state.metric_fps = !g_state.metric_fps;
                notify(true);
            } else if (g_section == Section::Overlay && y >= 247 && y <= 289) {
                g_state.metric_one_percent_low = !g_state.metric_one_percent_low;
                notify(true);
            } else if (g_section == Section::Overlay && y >= 295 && y <= 337) {
                g_state.metric_game_speed = !g_state.metric_game_speed;
                notify(true);
            } else if (g_section == Section::Overlay && y >= 343 && y <= 385) {
                g_state.metric_frametime_graph = !g_state.metric_frametime_graph;
                notify(true);
            } else if (g_section == Section::Video && y >= 183 && y <= 215 && x >= 416 && x <= 504) {
                int current = 0;
                for (int i = 1; i < int(std::size(kResolution)); ++i)
                    if (std::abs(g_state.width - kResolution[i][0]) + std::abs(g_state.height - kResolution[i][1]) <
                        std::abs(g_state.width - kResolution[current][0]) + std::abs(g_state.height - kResolution[current][1]))
                        current = i;
                current = (current + (x < 460 ? int(std::size(kResolution)) - 1 : 1)) % int(std::size(kResolution));
                g_state.width = kResolution[current][0];
                g_state.height = kResolution[current][1];
                notify(true);
            } else if (g_section == Section::Video && y >= 270 && y <= 302 && x >= 416 && x <= 504) {
                const int count = int(std::size(kInternalResolution[0]));
                g_state.internal_resolution = (g_state.internal_resolution + (x < 460 ? count - 1 : 1)) % count;
                notify(true);
            } else if (g_section == Section::Video && y >= 332 && y <= 364 && x >= 416 && x <= 504) {
                int index = 1;
                for (int i = 0; i < int(std::size(kFrameCaps)); ++i)
                    if (kFrameCaps[i] == g_state.frame_cap) index = i;
                index = (index + (x < 460 ? int(std::size(kFrameCaps)) - 1 : 1)) % int(std::size(kFrameCaps));
                g_state.frame_cap = kFrameCaps[index];
                notify(true);
            } else if (g_section == Section::Video && y >= 375 && y <= 419) {
                g_state.fullscreen = !g_state.fullscreen;
                notify(true);
            } else if (g_section == Section::Video && y >= 430 && y <= 474) {
                g_state.always_on_top = !g_state.always_on_top;
                notify(true);
            } else if (g_section == Section::Audio && y >= kSliderY - 12 && y <= kSliderY + 12 &&
                       x >= kSliderLeft - 8 && x <= kSliderRight + 8) {
                SetCapture(h);
                g_dragging_volume = true;
                set_volume_from_x(x);
            } else if (g_section == Section::Audio && y >= 272 && y <= 320) {
                g_state.muted = !g_state.muted;
                notify(true);
            } else if (g_section == Section::Audio && y >= 340 && y <= 384) {
                g_game_language_dropdown = !g_game_language_dropdown;
                InvalidateRect(h, nullptr, FALSE);
            } else if (g_section == Section::Controls && y >= 143 && y <= 173 && x >= 44 && x <= 272) {
                set_controller_preset(0);
            } else if (g_section == Section::Controls && y >= 143 && y <= 173 && x >= 288 && x <= 516) {
                set_controller_preset(1);
            } else if (g_section == Section::Controls && y >= 200 && y < 552) {
                const int action = (y - 200) / 22;
                if (action >= 0 && action < int(kPcControlCount) && x >= 155 && x <= 327) {
                    g_capture_action = action;
                    g_capture_device = CaptureDevice::Keyboard;
                    InvalidateRect(h, nullptr, FALSE);
                } else if (action >= 0 && action < int(kPcControlCount) && x >= 331 && x <= 515) {
                    g_capture_action = action;
                    g_capture_device = CaptureDevice::Controller;
                    InvalidateRect(h, nullptr, FALSE);
                }
            } else if (y >= 586) {
                close_menu();
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_dragging_volume) set_volume_from_x(GET_X_LPARAM(l));
            return 0;
        case WM_LBUTTONUP:
            if (g_dragging_volume) {
                g_dragging_volume = false;
                ReleaseCapture();
                notify(true);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (g_dragging_volume) {
                g_dragging_volume = false;
                notify(true);
            }
            return 0;
        case WM_CLOSE:
            close_menu();
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

void pc_menu_init(HWND owner, const PcMenuState& state, PcMenuApply apply) {
    g_owner = owner;
    g_state = state;
    g_state.language = std::clamp(g_state.language, 0, 3);
    g_state.controller_preset = std::clamp(g_state.controller_preset, 0, 1);
    g_state.overlay_position = std::clamp(g_state.overlay_position, 0, 3);
    g_controller_preset.store(g_state.controller_preset, std::memory_order_relaxed);
    g_apply = apply;
    WNDCLASSW wc{};
    wc.lpfnWndProc = menu_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));  // IDC_HAND
    wc.lpszClassName = L"WonderRecompQuickSettings";
    RegisterClassW(&wc);
    g_menu = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Quick Settings", WS_POPUP,
                             0, 0, kWidth, kHeight, owner, nullptr, wc.hInstance, nullptr);
    if (g_menu) SetLayeredWindowAttributes(g_menu, 0, 242, LWA_ALPHA);
}

void pc_menu_toggle() {
    if (!g_menu || !g_owner) return;
    if (g_open) {
        close_menu();
        return;
    }
    RECT owner{};
    GetWindowRect(g_owner, &owner);
    const int x = owner.left + ((owner.right - owner.left) - kWidth) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - kHeight) / 2;
    SetWindowPos(g_menu, HWND_TOP, x, y, kWidth, kHeight, SWP_SHOWWINDOW);
    g_open = true;
    SetForegroundWindow(g_menu);
    InvalidateRect(g_menu, nullptr, TRUE);
}

bool pc_menu_open() { return g_open.load(); }

void pc_menu_sync(const PcMenuState& state) {
    g_state = state;
    g_controller_preset.store(std::clamp(state.controller_preset, 0, 1), std::memory_order_relaxed);
    if (g_menu) InvalidateRect(g_menu, nullptr, FALSE);
}

void pc_menu_capture_controller_button(int physical_button) {
    HWND menu = g_menu;
    if (menu && physical_button >= 0 && physical_button < int(kPcControlCount))
        PostMessageW(menu, kCaptureControllerMessage, WPARAM(physical_button), 0);
}

}  // namespace nx::plat
