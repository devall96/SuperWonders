// The settings menu's tabs (Video, Audio, Controls, Overlay, System), at parity with the Win32 menu
// (pc_menu_win.cpp), whose four languages and strings it carries over. Each option has a description, shown at the
// bottom while the option is hovered or has the keyboard/controller focus (English for now).
#include <algorithm>
#include <cstdio>

#include "imgui.h"

#include "overlay/overlay_internal.h"

namespace nx::overlay {

namespace {

struct Labels {
    const char *title, *close, *video, *audio, *controls, *overlay, *window_size, *internal_resolution, *fullscreen,
        *topmost, *volume, *mute, *game_language, *game_language_restart, *control_preset, *standard_desc,
        *nintendo_desc, *restart_note, *system, *memory_limit, *low_vram, *restart_low_vram;
};

constexpr Labels kLabels[] = {
    {"Quick Settings", "F1, Esc or Minus + Plus to close", "Video", "Audio", "Controls", "Overlay",
     "Window / output size", "Internal resolution", "Fullscreen", "Always on top", "Game volume", "Mute audio",
     "Game language", "Restart the game to apply the language.", "Button layout",
     "A: bottom (A / Cross)     B: right (B / Circle)", "A: right (B / Circle)     B: bottom (A / Cross)",
     "Internal resolution applies after restarting the game.", "System", "Process memory limit", "Low VRAM profile",
     "Restart the game to apply the texture profile."},
    {"Impostazioni", "F1, Esc o Meno + Più per chiudere", "Video", "Audio", "Controlli", "Overlay",
     "Dimensione finestra / output", "Risoluzione interna", "Schermo intero", "Sempre in primo piano",
     "Volume gioco", "Disattiva audio", "Lingua del gioco", "Riavvia il gioco per applicare la lingua.",
     "Disposizione tasti", "A: in basso (A / Croce)     B: a destra (B / Cerchio)",
     "A: a destra (B / Cerchio)     B: in basso (A / Croce)", "La risoluzione interna si applica al riavvio.",
     "Sistema", "Limite memoria processo", "Profilo VRAM ridotta",
     "Riavvia il gioco per applicare il profilo texture."},
    {"Einstellungen", "F1, Esc oder Minus + Plus zum Schließen", "Video", "Audio", "Steuerung", "Overlay",
     "Fenster- / Ausgabegröße", "Interne Auflösung", "Vollbild", "Immer im Vordergrund", "Spiel-Lautstärke",
     "Ton stummschalten", "Spielsprache", "Spiel zum Übernehmen neu starten.", "Tastenbelegung",
     "A: unten (A / Kreuz)     B: rechts (B / Kreis)", "A: rechts (B / Kreis)     B: unten (A / Kreuz)",
     "Interne Auflösung wird nach Neustart übernommen.", "System", "Prozessspeicherlimit",
     "Niedriger VRAM-Verbrauch", "Spiel neu starten, um das Texturprofil anzuwenden."},
    {"Paramètres", "F1, Échap ou Moins + Plus pour fermer", "Vidéo", "Audio", "Commandes", "Overlay",
     "Taille fenêtre / sortie", "Résolution interne", "Plein écran", "Toujours au premier plan", "Volume du jeu",
     "Couper le son", "Langue du jeu", "Redémarrez le jeu pour appliquer la langue.", "Disposition des touches",
     "A : bas (A / Croix)     B : droite (B / Cercle)", "A : droite (B / Cercle)     B : bas (A / Croix)",
     "La résolution interne s'applique après redémarrage.", "Système", "Limite mémoire du processus",
     "Profil VRAM réduit", "Redémarrez pour appliquer le profil de textures."},
};
constexpr const char* kLanguageNames[] = {"English", "Italiano", "Deutsch", "Français"};
constexpr const char* kInternal[4][5] = {
    {"Original / Dynamic", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"},
    {"Originale / dinamica", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"},
    {"Original / Dynamisch", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"},
    {"Originale / dynamique", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"},
};
constexpr int kSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
constexpr const char* kOverlayLabels[4][6] = {
    {"Metrics overlay", "FPS", "1% lows", "Game speed", "Frametime graph", "Overlay position"},
    {"Overlay metriche", "FPS", "Minimi 1%", "Velocità gioco", "Grafico frametime", "Posizione overlay"},
    {"Metrik-Overlay", "FPS", "1%-Tiefstwerte", "Spieltempo", "Frametime-Diagramm", "Overlay-Position"},
    {"Superposition métriques", "FPS", "1 % les plus bas", "Vitesse du jeu", "Graphique frametime",
     "Position de l’overlay"},
};
constexpr const char* kPositions[4][4] = {
    {"Top left", "Top right", "Bottom left", "Bottom right"},
    {"In alto a sinistra", "In alto a destra", "In basso a sinistra", "In basso a destra"},
    {"Oben links", "Oben rechts", "Unten links", "Unten rechts"},
    {"En haut à gauche", "En haut à droite", "En bas à gauche", "En bas à droite"},
};
constexpr const char* kHeaders[4][2] = {
    {"Keyboard", "Controller"}, {"Tastiera", "Controller"}, {"Tastatur", "Controller"}, {"Clavier", "Manette"}};
constexpr const char* kActions[kControlCount] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "L Stick", "R Stick",
    "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down",
};
constexpr const char* kPhysical[kControlCount] = {
    "A / Cross", "B / Circle", "X / Square", "Y / Triangle", "LB / L1", "RB / R1", "LT / L2", "RT / R2",
    "Start / +", "Back / -", "L3 / LS", "R3 / RS", "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down",
};
constexpr int kTabs = 5;

// What the focused or hovered option does (set after each widget by describe(), shown in the footer).
const char* g_description = nullptr;

void describe(const char* text) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) g_description = text;
}

namespace desc {
constexpr const char* language = "Language of this menu.";
constexpr const char* window_size = "Size of the game window in pixels. Not available in fullscreen.";
constexpr const char* internal = "The resolution the game renders at. Original lets the game choose (it lowers it under "
                                 "load); higher is sharper and costs more GPU time. Applies after restarting the game.";
constexpr const char* fullscreen = "Fill the whole screen. F11 also switches it.";
constexpr const char* topmost = "Keep the game window above other windows. Not available on Wayland.";
constexpr const char* volume = "The game's volume.";
constexpr const char* mute = "Silence the game without changing its volume.";
constexpr const char* game_language = "The language of the game's text. Applies after restarting the game.";
constexpr const char* preset = "Which controller button is A. Xbox / DualSense: A at the bottom. Nintendo: A on the "
                               "right, as on a Switch controller.";
constexpr const char* key = "Select, then press the key for this action. Esc cancels.";
constexpr const char* pad = "The controller button for this action. Choosing the layout's default clears the change.";
constexpr const char* metrics = "A performance panel in a corner of the screen.";
constexpr const char* fps = "Frames per second over the last 60 frames.";
constexpr const char* low = "The frame rate of the slowest 1% of the last 300 frames: it shows stutter.";
constexpr const char* speed = "How fast the game runs against its own target: 100% is full speed.";
constexpr const char* graph = "The time of each of the last 300 frames, 0 to 50 ms.";
constexpr const char* position = "The corner the panel sits in.";
constexpr const char* memory = "The game closes itself if its memory use passes this, so a leak can never freeze the "
                               "computer.";
constexpr const char* low_vram = "For graphics cards with little video memory: decoded textures are kept compressed "
                                 "(BC3), at a small cost in quality. Applies after restarting the game.";
}  // namespace desc

// A combo over `names`; true when the choice changed.
bool combo(const char* label, int& value, const char* const* names, int count) {
    bool changed = false;
    value = std::clamp(value, 0, count - 1);
    if (ImGui::BeginCombo(label, names[value])) {
        for (int i = 0; i < count; ++i) {
            bool selected = i == value;
            if (ImGui::Selectable(names[i], selected) && !selected) value = i, changed = true;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.67f, 0.75f, 0.85f, 1.0f});
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void video(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    int lang = std::clamp(s.language, 0, 3);
    int size = 0;
    for (int i = 0; i < 4; ++i)
        if (kSizes[i][0] == s.width && kSizes[i][1] == s.height) size = i;
    const char* sizes[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"};
    ImGui::BeginDisabled(!c.host.can_resize || s.fullscreen);
    if (combo(l.window_size, size, sizes, 4)) s.width = kSizes[size][0], s.height = kSizes[size][1], settings_changed(true);
    ImGui::EndDisabled();
    describe(desc::window_size);
    if (combo(l.internal_resolution, s.internal_resolution, kInternal[lang], 5)) settings_changed(true);
    describe(desc::internal);
    note(l.restart_note);
    if (ImGui::Checkbox(l.fullscreen, &s.fullscreen)) settings_changed(true);
    describe(desc::fullscreen);
    ImGui::BeginDisabled(!c.host.can_always_on_top);
    if (ImGui::Checkbox(l.topmost, &s.always_on_top)) settings_changed(true);
    ImGui::EndDisabled();
    describe(desc::topmost);
}

void audio(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    if (ImGui::SliderInt(l.volume, &s.volume, 0, 100, "%d%%")) settings_changed(false);
    if (ImGui::IsItemDeactivatedAfterEdit()) settings_changed(true);
    describe(desc::volume);
    if (ImGui::Checkbox(l.mute, &s.muted)) settings_changed(true);
    describe(desc::mute);
    if (combo(l.game_language, s.game_language, kLanguageNames, 4)) settings_changed(true);
    describe(desc::game_language);
    note(l.game_language_restart);
}

void controls(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    int lang = std::clamp(s.language, 0, 3);
    const char* presets[] = {"Xbox / DualSense", "Nintendo"};
    if (combo(l.control_preset, s.controller_preset, presets, 2)) settings_changed(true);
    describe(desc::preset);
    note(s.controller_preset == 1 ? l.nintendo_desc : l.standard_desc);
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("bindings", 3, tf)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn(kHeaders[lang][0], ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(kHeaders[lang][1], ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableHeadersRow();
    for (size_t a = 0; a < kControlCount; ++a) {
        ImGui::PushID(int(a));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kActions[a]);
        ImGui::TableNextColumn();
        char key[64];
        if (c.capture_action == int(a))
            std::snprintf(key, sizeof key, "...##k");
        else
            std::snprintf(key, sizeof key, "%s##k", c.host.key_name ? c.host.key_name(s.keyboard_bindings[a]) : "?");
        if (ImGui::Button(key, {-1, 0})) c.capture_action = int(a);  // overlay.cpp takes the next key
        describe(desc::key);
        ImGui::TableNextColumn();
        int phys = controller_binding(s, a);
        ImGui::SetNextItemWidth(-1);
        if (combo("##c", phys, kPhysical, int(kControlCount))) {
            s.controller_bindings[a] = phys;
            s.controller_custom_mask |= 1u << a;
            if (phys == default_controller_binding(a, s.controller_preset)) s.controller_custom_mask &= ~(1u << a);
            settings_changed(true);
        }
        describe(desc::pad);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void overlay_tab(MenuContext& c) {
    Settings& s = c.settings;
    int lang = std::clamp(s.language, 0, 3);
    const auto& o = kOverlayLabels[lang];
    if (ImGui::Checkbox(o[0], &s.metrics_enabled)) settings_changed(true);
    describe(desc::metrics);
    ImGui::BeginDisabled(!s.metrics_enabled);
    ImGui::Indent();
    if (ImGui::Checkbox(o[1], &s.metric_fps)) settings_changed(true);
    describe(desc::fps);
    if (ImGui::Checkbox(o[2], &s.metric_one_percent_low)) settings_changed(true);
    describe(desc::low);
    if (ImGui::Checkbox(o[3], &s.metric_game_speed)) settings_changed(true);
    describe(desc::speed);
    if (ImGui::Checkbox(o[4], &s.metric_frametime_graph)) settings_changed(true);
    describe(desc::graph);
    ImGui::Unindent();
    if (combo(o[5], s.overlay_position, kPositions[lang], 4)) settings_changed(true);
    describe(desc::position);
    ImGui::EndDisabled();
}

void system_tab(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    if (ImGui::SliderInt(l.memory_limit, &s.memory_limit_gb, 2, 32, "%d GB")) settings_changed(false);
    if (ImGui::IsItemDeactivatedAfterEdit()) settings_changed(true);
    describe(desc::memory);
    if (ImGui::Checkbox(l.low_vram, &s.low_vram_profile)) settings_changed(true);
    describe(desc::low_vram);
    note(l.restart_low_vram);
}

}  // namespace

int menu_tab_count() { return kTabs; }
const char* const* menu_language_names() { return kLanguageNames; }

void draw_menu(MenuContext& c) {
    Settings& s = c.settings;
    const Labels& l = kLabels[std::clamp(s.language, 0, 3)];
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 size{std::min(640.0f * c.scale, display.x * 0.95f), std::min(620.0f * c.scale, display.y * 0.92f)};
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (c.appearing) ImGui::SetNextWindowFocus();  // keyboard and controller navigation start in the menu
    if (!ImGui::Begin("##nx_menu", nullptr, f)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(l.title);
    ImGui::SameLine();
    const float lang_w = 160 * c.scale;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - lang_w);
    ImGui::SetNextItemWidth(lang_w);
    g_description = nullptr;
    if (combo("##language", s.language, kLanguageNames, 4)) settings_changed(true);
    describe(desc::language);
    ImGui::TextDisabled("%s", l.close);
    ImGui::Separator();
    // The footer (description and controller hints) keeps the bottom lines of the window.
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 4.2f;

    const char* names[kTabs] = {l.video, l.audio, l.controls, l.overlay, l.system};
    int current = -1;
    // Tabs follow the keyboard / controller focus: moving onto a tab with the arrows or the D-pad shows it (next
    // frame), with no Enter or A needed. The mouse still selects by clicking.
    // Only a change of focus moves the tab: L / R select a tab without moving the focus, which must not pull it back.
    static int follow = -1, focused_before = -1;
    int follow_next = -1, focused = -1;
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_NoTooltip)) {
        for (int t = 0; t < kTabs; ++t) {
            ImGuiTabItemFlags tf = c.tab_request == t || follow == t ? ImGuiTabItemFlags_SetSelected : 0;
            const bool shown = ImGui::BeginTabItem(names[t], nullptr, tf);
            if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) focused = t;
            if (!shown && focused == t && focused_before != t) follow_next = t;
            if (shown && c.appearing) ImGui::SetItemDefaultFocus();  // navigation starts on the open tab
            if (shown) {
                current = t;
                ImGui::BeginChild("page", {0, -footer}, ImGuiChildFlags_NavFlattened);
                ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.45f);
                switch (t) {
                    case 0: video(c, l); break;
                    case 1: audio(c, l); break;
                    case 2: controls(c, l); break;
                    case 3: overlay_tab(c); break;
                    default: system_tab(c, l); break;
                }
                ImGui::PopItemWidth();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    follow = follow_next;
    focused_before = focused;
    c.tab_request = current;  // out: the tab shown, for the shoulder buttons
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.85f, 0.90f, 0.97f, 1.0f});
    ImGui::TextWrapped("%s", g_description ? g_description : "");
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight());
    ImGui::TextDisabled("A: select     B: back     L / R: tab     D-pad: move     Minus + Plus: close");
    ImGui::End();
}

}  // namespace nx::overlay
