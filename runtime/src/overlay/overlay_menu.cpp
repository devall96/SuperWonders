// The settings menu's tabs (Video, Audio, Controls, Overlay, System), at parity with the Win32 menu
// (pc_menu_win.cpp), whose four languages and strings it carries over.
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
    if (combo(l.internal_resolution, s.internal_resolution, kInternal[lang], 5)) settings_changed(true);
    note(l.restart_note);
    if (ImGui::Checkbox(l.fullscreen, &s.fullscreen)) settings_changed(true);
    ImGui::BeginDisabled(!c.host.can_always_on_top);
    if (ImGui::Checkbox(l.topmost, &s.always_on_top)) settings_changed(true);
    ImGui::EndDisabled();
}

void audio(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    if (ImGui::SliderInt(l.volume, &s.volume, 0, 100, "%d%%")) settings_changed(false);
    if (ImGui::IsItemDeactivatedAfterEdit()) settings_changed(true);
    if (ImGui::Checkbox(l.mute, &s.muted)) settings_changed(true);
    if (combo(l.game_language, s.game_language, kLanguageNames, 4)) settings_changed(true);
    note(l.game_language_restart);
}

void controls(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    int lang = std::clamp(s.language, 0, 3);
    const char* presets[] = {"Xbox / DualSense", "Nintendo"};
    if (combo(l.control_preset, s.controller_preset, presets, 2)) settings_changed(true);
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
        ImGui::TableNextColumn();
        int phys = controller_binding(s, a);
        ImGui::SetNextItemWidth(-1);
        if (combo("##c", phys, kPhysical, int(kControlCount))) {
            s.controller_bindings[a] = phys;
            s.controller_custom_mask |= 1u << a;
            if (phys == default_controller_binding(a, s.controller_preset)) s.controller_custom_mask &= ~(1u << a);
            settings_changed(true);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void overlay_tab(MenuContext& c) {
    Settings& s = c.settings;
    int lang = std::clamp(s.language, 0, 3);
    const auto& o = kOverlayLabels[lang];
    if (ImGui::Checkbox(o[0], &s.metrics_enabled)) settings_changed(true);
    ImGui::BeginDisabled(!s.metrics_enabled);
    ImGui::Indent();
    if (ImGui::Checkbox(o[1], &s.metric_fps)) settings_changed(true);
    if (ImGui::Checkbox(o[2], &s.metric_one_percent_low)) settings_changed(true);
    if (ImGui::Checkbox(o[3], &s.metric_game_speed)) settings_changed(true);
    if (ImGui::Checkbox(o[4], &s.metric_frametime_graph)) settings_changed(true);
    ImGui::Unindent();
    if (combo(o[5], s.overlay_position, kPositions[lang], 4)) settings_changed(true);
    ImGui::EndDisabled();
}

void system_tab(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    if (ImGui::SliderInt(l.memory_limit, &s.memory_limit_gb, 2, 32, "%d GB")) settings_changed(false);
    if (ImGui::IsItemDeactivatedAfterEdit()) settings_changed(true);
    if (ImGui::Checkbox(l.low_vram, &s.low_vram_profile)) settings_changed(true);
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
    if (combo("##language", s.language, kLanguageNames, 4)) settings_changed(true);
    ImGui::TextDisabled("%s   (L / R: tabs)", l.close);
    ImGui::Separator();

    const char* names[kTabs] = {l.video, l.audio, l.controls, l.overlay, l.system};
    int current = -1;
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_NoTooltip)) {
        for (int t = 0; t < kTabs; ++t) {
            ImGuiTabItemFlags tf = c.tab_request == t ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(names[t], nullptr, tf)) {
                current = t;
                ImGui::BeginChild("page", {0, 0}, ImGuiChildFlags_NavFlattened);
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
    c.tab_request = current;  // out: the tab shown, for the shoulder buttons
    ImGui::End();
}

}  // namespace nx::overlay
