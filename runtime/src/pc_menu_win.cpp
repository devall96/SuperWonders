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
constexpr int kHeight = 520;
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
constexpr const wchar_t* kInternalResolution[][5] = {
    {L"Original / Dynamic", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440"},
    {L"Originale / dinamica", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440"},
    {L"Original / Dynamisch", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440"},
    {L"Originale / dynamique", L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440"},
};
constexpr const wchar_t* kLanguageNames[] = {L"English", L"Italiano", L"Deutsch", L"Français"};

struct Labels {
    const wchar_t* title;
    const wchar_t* close;
    const wchar_t* video;
    const wchar_t* audio;
    const wchar_t* controls;
    const wchar_t* window_size;
    const wchar_t* internal_resolution;
    const wchar_t* fullscreen_on;
    const wchar_t* fullscreen_off;
    const wchar_t* topmost_on;
    const wchar_t* topmost_off;
    const wchar_t* volume;
    const wchar_t* mute;
    const wchar_t* unmute;
    const wchar_t* control_preset;
    const wchar_t* standard_desc;
    const wchar_t* nintendo_desc;
    const wchar_t* restart_note;
};

constexpr Labels kLabels[] = {
    {L"Quick Settings", L"F1 or Esc to close", L"Video", L"Audio", L"Controls", L"Window / output size",
     L"Internal resolution", L"Enable fullscreen", L"Exit fullscreen", L"Always on top: On", L"Always on top: Off",
     L"Game volume", L"Mute audio", L"Unmute audio", L"Button layout",
     L"A: bottom (A / Cross)     B: right (B / Circle)", L"A: right (B / Circle)     B: bottom (A / Cross)",
     L"Internal resolution applies after restarting the game."},
    {L"Impostazioni", L"F1 o Esc per chiudere", L"Video", L"Audio", L"Controlli", L"Dimensione finestra / output",
     L"Risoluzione interna", L"Attiva schermo intero", L"Esci da schermo intero", L"Sempre in primo piano: sì",
     L"Sempre in primo piano: no", L"Volume gioco", L"Disattiva audio", L"Riattiva audio", L"Disposizione tasti",
     L"A: in basso (A / Croce)     B: a destra (B / Cerchio)", L"A: a destra (B / Cerchio)     B: in basso (A / Croce)",
     L"La risoluzione interna si applica al riavvio."},
    {L"Einstellungen", L"F1 oder Esc zum Schließen", L"Video", L"Audio", L"Steuerung", L"Fenster- / Ausgabegröße",
     L"Interne Auflösung", L"Vollbild aktivieren", L"Vollbild beenden", L"Immer im Vordergrund: Ein",
     L"Immer im Vordergrund: Aus", L"Spiel-Lautstärke", L"Ton stummschalten", L"Ton einschalten", L"Tastenbelegung",
     L"A: unten (A / Kreuz)     B: rechts (B / Kreis)", L"A: rechts (B / Kreis)     B: unten (A / Kreuz)",
     L"Interne Auflösung wird nach Neustart übernommen."},
    {L"Paramètres", L"F1 ou Échap pour fermer", L"Vidéo", L"Audio", L"Commandes", L"Taille fenêtre / sortie",
     L"Résolution interne", L"Activer le plein écran", L"Quitter le plein écran", L"Toujours au premier plan : oui",
     L"Toujours au premier plan : non", L"Volume du jeu", L"Couper le son", L"Rétablir le son", L"Disposition des touches",
     L"A : bas (A / Croix)     B : droite (B / Cercle)", L"A : droite (B / Cercle)     B : bas (A / Croix)",
     L"La résolution interne s'applique après redémarrage."},
};

enum class Section { Video, Audio, Controls };

HWND g_owner = nullptr;
HWND g_menu = nullptr;
PcMenuState g_state;
PcMenuApply g_apply = nullptr;
std::atomic<bool> g_open{false};
std::atomic<int> g_controller_preset{0};
Section g_section = Section::Video;
bool g_dragging_volume = false;
bool g_language_dropdown = false;

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
                                           [std::clamp(g_state.internal_resolution, 0, 4)]);

    rounded(dc, RECT{44, 326, 516, 370}, RGB(35, 43, 61), RGB(68, 81, 105));
    paint_text(dc, g_state.fullscreen ? text.fullscreen_off : text.fullscreen_on, RECT{58, 326, 502, 370},
               RGB(230, 235, 245), 15, DT_CENTER);
    rounded(dc, RECT{44, 384, 516, 428}, RGB(35, 43, 61), RGB(68, 81, 105));
    paint_text(dc, g_state.always_on_top ? text.topmost_on : text.topmost_off, RECT{58, 384, 502, 428},
               RGB(230, 235, 245), 15, DT_CENTER);
    paint_text(dc, text.restart_note, RECT{35, 449, 525, 480}, RGB(155, 169, 192), 12, DT_CENTER);
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
}

void paint_controls(HDC dc) {
    const Labels& text = labels();
    paint_text(dc, text.control_preset, RECT{44, 145, 516, 177}, RGB(214, 222, 238), 17);
    const bool standard = std::clamp(g_state.controller_preset, 0, 1) == 0;
    rounded(dc, RECT{44, 192, 516, 274}, standard ? RGB(43, 70, 105) : RGB(35, 43, 61),
            standard ? RGB(102, 174, 255) : RGB(68, 81, 105), 12);
    paint_text(dc, L"Xbox / DualSense", RECT{62, 201, 498, 232}, RGB(245, 247, 255), 18);
    paint_text(dc, text.standard_desc, RECT{62, 235, 498, 263}, RGB(190, 203, 222), 13);

    rounded(dc, RECT{44, 292, 516, 374}, !standard ? RGB(43, 70, 105) : RGB(35, 43, 61),
            !standard ? RGB(102, 174, 255) : RGB(68, 81, 105), 12);
    paint_text(dc, L"Nintendo", RECT{62, 301, 498, 332}, RGB(245, 247, 255), 18);
    paint_text(dc, text.nintendo_desc, RECT{62, 335, 498, 363}, RGB(190, 203, 222), 13);
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

    paint_tab(dc, RECT{28, kTabTop, 190, kTabBottom}, text.video, g_section == Section::Video);
    paint_tab(dc, RECT{199, kTabTop, 361, kTabBottom}, text.audio, g_section == Section::Audio);
    paint_tab(dc, RECT{370, kTabTop, 532, kTabBottom}, text.controls, g_section == Section::Controls);

    if (g_section == Section::Video) paint_video(dc);
    else if (g_section == Section::Audio) paint_audio(dc);
    else paint_controls(dc);

    paint_text(dc, text.close, RECT{35, 478, 525, 501}, RGB(140, 154, 177), 12, DT_CENTER);

    if (g_language_dropdown) {
        for (int i = 0; i < int(std::size(kLanguageNames)); ++i) {
            const int top = kLanguageBottom + 2 + i * kLanguageItemHeight;
            rounded(dc, RECT{kLanguageLeft, top, kLanguageRight, top + kLanguageItemHeight},
                    i == g_state.language ? RGB(52, 84, 126) : RGB(35, 43, 61), RGB(92, 108, 136), 6);
            paint_text(dc, kLanguageNames[i], RECT{kLanguageLeft + 8, top, kLanguageRight - 8, top + kLanguageItemHeight},
                       RGB(238, 243, 252), 14, DT_CENTER);
        }
    }
    EndPaint(h, &ps);
}

void set_controller_preset(int preset) {
    g_state.controller_preset = std::clamp(preset, 0, 1);
    g_controller_preset.store(g_state.controller_preset, std::memory_order_relaxed);
    notify(true);
}

void close_menu() {
    g_open = false;
    g_language_dropdown = false;
    if (g_menu) ShowWindow(g_menu, SW_HIDE);
    if (g_owner) SetForegroundWindow(g_owner);
}

LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT:
            paint(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
            if (w == VK_F1 || w == VK_ESCAPE) {
                close_menu();
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
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
            if (y >= kTabTop && y <= kTabBottom) {
                if (x >= 28 && x <= 190) g_section = Section::Video;
                else if (x >= 199 && x <= 361) g_section = Section::Audio;
                else if (x >= 370 && x <= 532) g_section = Section::Controls;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (g_section == Section::Video && y >= 183 && y <= 215 && x >= 416 && x <= 504) {
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
            } else if (g_section == Section::Video && y >= 326 && y <= 370) {
                g_state.fullscreen = !g_state.fullscreen;
                notify(true);
            } else if (g_section == Section::Video && y >= 384 && y <= 428) {
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
            } else if (g_section == Section::Controls && y >= 192 && y <= 274) {
                set_controller_preset(0);
            } else if (g_section == Section::Controls && y >= 292 && y <= 374) {
                set_controller_preset(1);
            } else if (y >= 478) {
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

}  // namespace nx::plat
