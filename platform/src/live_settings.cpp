// Settings applied while the game runs (#89, ADR 0076); see live_settings.h.
#include "live_settings.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <utility>

#include "audio_out.h"
#include "desktop.h"
#include "focus.h"
#include "gfx_backend.h"
#include "input.h"
#include "keypad_hide.h"
#include "lifecycle.h"
#include "maplethief/audio.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/uikit.h"
#include "perf_overlay.h"
#include "present.h"
#include "rumble.h"
#include "save_tools.h"

namespace maplethief::frontend {

namespace {

bool is(const SettingInfo &row, const char *key) { return std::strcmp(row.key, key) == 0; }

namespace fs = std::filesystem;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The popup menu's option tab (CDataMgr +0xB0: 0 status ... 5 option).
constexpr std::int32_t kOptionTab = 5;
constexpr std::int32_t kFieldPanel = 0;
constexpr std::int32_t kPopupMenuPanel = 1;

} // namespace

bool apply_live_setting(const LiveTargets &t, const SettingInfo &row, const Settings &s) {
    if (is(row, "scale_mode") || is(row, "border_color")) {
        if (t.presenter == nullptr) {
            return false;
        }
        t.presenter->set_scaling(s.scale_mode, s.border_color);
        return true;
    }
    if (is(row, "filter")) {
        // A pass needs SDL's GPU renderer, which the window has only if a filter or the GPU
        // backend was asked for at start-up.
        if (t.presenter == nullptr || t.filter_pass == nullptr || t.renderer == nullptr ||
            SDL_GetGPURendererDevice(t.renderer) == nullptr) {
            return false;
        }
        t.filter_pass->close();
        t.presenter->set_stage(PresentStage{});
        if (s.filter == ShaderFilter::kNone) {
            SDL_Log("present: filter none (the scale mode's)");
            return true;
        }
        if (!t.filter_pass->open_filter(t.renderer, s.filter)) {
            return false;
        }
        t.presenter->set_stage(t.filter_pass->stage());
        SDL_Log("present: filter %s (%s)", shader_filter_name(s.filter),
                shader_filter_shader(s.filter));
        return true;
    }
    if (is(row, "master_volume")) {
        mt_audio_set_gain(MT_AUDIO_BUS_MASTER, volume_gain(s.master_volume));
        return true;
    }
    if (is(row, "music_volume")) {
        mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, volume_gain(s.music_volume));
        return true;
    }
    if (is(row, "effects_volume")) {
        mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, volume_gain(s.effects_volume));
        return true;
    }
    if (is(row, "mute_unfocused")) {
        if (t.focus_mute == nullptr) {
            return false;
        }
        t.focus_mute->set_enabled(s.mute_unfocused && t.focus_mute_allowed);
        return true;
    }
    if (is(row, "right_touch")) {
        if (t.input == nullptr) {
            return false;
        }
        t.input->set_right_touch(s.right_touch);
        return true;
    }
    if (is(row, "hide_keypad")) {
        if (t.keypad == nullptr) {
            return false;
        }
        t.keypad->set_mode(s.hide_keypad, t.now);
        return true;
    }
    if (is(row, "rumble") || is(row, "rumble_strength")) {
        if (t.rumble == nullptr) {
            return false;
        }
        t.rumble->configure(s.rumble, s.rumble_strength);
        return true;
    }
    if (is(row, "perf_overlay")) {
        if (t.perf == nullptr) {
            return false;
        }
        t.perf->set_on(s.perf_overlay);
        return true;
    }
    if (is(row, "fullscreen") || is(row, "vsync")) {
        if (t.desktop == nullptr) {
            return false;
        }
        t.desktop->apply(s);
        if (t.vsync != nullptr && t.stats != nullptr) {
            follow_vsync(*t.desktop, t.vsync, t.stats);
        }
        return true;
    }
    if (is(row, "pause_unfocused")) {
        if (t.lifecycle == nullptr) {
            return false;
        }
        t.lifecycle->set_pause_unfocused(s.pause_unfocused);
        return true;
    }
    if (is(row, "render_scale")) {
        return t.render_scale && t.render_scale(s.render_scale);
    }
    if (is(row, "border") || is(row, "border_image")) {
        if (t.presenter == nullptr) {
            return false;
        }
        t.presenter->set_border(s.border, s.border_image);
        SDL_Log("present: border %s%s%s", border_mode_name(s.border),
                s.border == BorderMode::kImage ? ": " : "",
                s.border == BorderMode::kImage ? s.border_image.c_str() : "");
        return true;
    }
    if (is(row, "colorblind_filter")) {
        if (t.presenter == nullptr) {
            return false;
        }
        t.presenter->set_colorblind(s.colorblind_filter);
        SDL_Log("present: colour-blind filter %s", colorblind_filter_name(s.colorblind_filter));
        return true;
    }
    if (is(row, "screen_shake")) {
        mt_hooks_install_screen_shake(s.screen_shake);
        return true;
    }
    if (is(row, "keypad_layout")) {
        if (!t.keypad_layout) {
            return false;
        }
        t.keypad_layout(s.keypad_layout);
        return true;
    }
    for (const char *key :
         {"menu_repeat", "confirm_guard", "focus_highlight", "button_prompts", "game_speed",
          "fast_forward", "autosave", "frame_rate", "widescreen_hud", "ui_animations",
          "turbo_attack", "auto_potion", "skip_intros"}) {
        if (is(row, key)) {
            return t.turn_reads_settings;
        }
    }
    return false;
}

GameOptionsOpener::GameOptionsOpener(Press press, Panel panel, Touch touch)
    : press_(std::move(press)), panel_(std::move(panel)), touch_(std::move(touch)) {}

bool GameOptionsOpener::available() const {
    return state_ == State::kIdle && panel_ && panel_() == kFieldPanel;
}

bool GameOptionsOpener::start() {
    if (!available()) {
        outcome_ = Outcome::kNotInField;
        return false;
    }
    popup_menu_tab_point(kOptionTab, &x_, &y_);
    outcome_ = Outcome::kNone;
    state_ = State::kPress;
    return true;
}

void GameOptionsOpener::turn(double now) {
    switch (state_) {
    case State::kIdle:
        return;
    case State::kPress:
        press_(MT_ACTION_MENU, true);
        state_ = State::kRelease;
        return;
    case State::kRelease:
        press_(MT_ACTION_MENU, false);
        state_ = State::kWaitMenu;
        until_ = now + kWaitSeconds;
        return;
    case State::kWaitMenu:
        if (panel_() == kPopupMenuPanel) {
            state_ = State::kSettle;
            until_ = now + kSettleSeconds;
        } else if (now >= until_) {
            SDL_Log("settings: game options: the popup menu did not come up");
            state_ = State::kIdle;
            outcome_ = Outcome::kNoMenu;
        }
        return;
    case State::kSettle:
        if (panel_() != kPopupMenuPanel) {
            state_ = State::kIdle;
            outcome_ = Outcome::kNoMenu;
        } else if (now >= until_) {
            touch_(MT_UIKIT_TOUCH_BEGAN, x_, y_);
            state_ = State::kLift;
        }
        return;
    case State::kLift:
        touch_(MT_UIKIT_TOUCH_ENDED, x_, y_);
        state_ = State::kIdle;
        outcome_ = Outcome::kOpened;
        SDL_Log("settings: game options: the popup menu's option tab");
        return;
    }
}

std::string export_saves_action(const SaveLocation &loc, const std::string &stamp) {
    if (loc.data_dir.empty()) {
        return "No data folder: use --export-saves";
    }
    const fs::path dir = utf8_path(loc.data_dir) / "exports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string name = "MapleThief-saves-" + stamp + ".zip";
    std::vector<std::string> exported;
    std::string why;
    if (!export_saves(loc, path_utf8(dir / name), &exported, &why)) {
        SDL_Log("saves: export: %s", why.c_str());
        return "Not exported: " + why;
    }
    SDL_Log("saves: %zu files exported to %s", exported.size(), path_utf8(dir / name).c_str());
    return "Exported to exports/" + name;
}

std::string import_saves_action(const SaveLocation &loc, std::int32_t panel,
                                const std::string &stamp) {
    if (loc.data_dir.empty()) {
        return "No data folder: use --import-saves";
    }
    if (panel >= 0) {
        return "Import from the title or the main menu (a character is loaded)";
    }
    const fs::path dir = utf8_path(loc.data_dir) / "import";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path source = dir;
    std::string newest;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec)) {
        std::string ext = path_utf8(e.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const std::string name = path_utf8(e.path().filename());
        std::error_code fec;
        if (ext == ".zip" && e.is_regular_file(fec) && name > newest) {
            newest = name;
            source = e.path();
        }
    }
    ImportResult result;
    std::string why;
    if (!import_saves(loc, path_utf8(source), stamp, &result, &why)) {
        SDL_Log("saves: import from %s: %s", path_utf8(source).c_str(), why.c_str());
        for (const SaveProblem &p : result.problems) {
            SDL_Log("saves:   %s", p.line().c_str());
        }
        return "Not imported: " + (result.problems.empty() ? why : result.problems.front().line());
    }
    SDL_Log("saves: %zu files imported from %s", result.imported.size(), path_utf8(source).c_str());
    return "Imported " + std::to_string(result.imported.size()) + " files from import/" + newest;
}

std::string backups_action(const SaveLocation &loc) {
    const std::vector<BackupInfo> all = list_backups(loc);
    if (all.empty()) {
        return loc.keep == 0 ? "No backups (Backups per character is 0)"
                             : "No backups yet: one is kept after each save";
    }
    std::string line;
    for (const std::string kind : {"0", "1", "2", "import"}) {
        const auto n = std::count_if(all.begin(), all.end(),
                                     [&kind](const BackupInfo &b) { return b.kind == kind; });
        if (n > 0) {
            line += (line.empty() ? "" : ", ") +
                    (kind == "import" ? std::string("imports") : "slot " + kind) + ": " +
                    std::to_string(n);
        }
    }
    return "Backups: " + line + " (in backups/)";
}

} // namespace maplethief::frontend
