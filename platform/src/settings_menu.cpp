// The settings menu (#89, ADR 0076); see settings_menu.h.
#include "settings_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "canvas.h"
#include "game.h"
#include "present.h"

namespace maplethief::frontend {

namespace {

constexpr Rgba kBackdrop = {0x00, 0x00, 0x00, kOverlayBackdropAlpha};
constexpr Rgba kPanel = {0x14, 0x1A, 0x24, 240};
constexpr Rgba kBorder = {0x5A, 0x6B, 0x85, 255};
constexpr Rgba kSelection = {0x2E, 0x4A, 0x7A, 255};
constexpr Rgba kTabOff = {0x22, 0x2A, 0x38, 255};
constexpr Rgba kHeader = {0xF2, 0xB8, 0x4B, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};
constexpr Rgba kWarning = {0xFF, 0xD1, 0x66, 255};
constexpr Rgba kAssist = {0xFF, 0x8A, 0x5C, 255};
constexpr Rgba kGood = {0x9B, 0xE2, 0x9B, 255};

constexpr int kPanelX = 8, kPanelY = 6, kPanelW = 464, kPanelH = 308;
constexpr int kTextX = 20;
constexpr int kNoteY = 78;                   // the tab's note (the Assists warning)
constexpr int kHelpY = 256;                  // two lines of help
constexpr int kStatusY = 277;                // the status line
constexpr int kHintsY = 292;                 // the hints with glyphs
constexpr int kArrowW = 14;                  // a value's "<" and ">" boxes
constexpr int kMaxLine = (kPanelW - 24) / 6; // characters on a line

constexpr MenuActionInfo kActions[] = {
    {.key = "bindings",
     .group = SettingGroup::kControls,
     .label = "Bindings",
     .help = "Rebind the keys and gamepad buttons (the remap screen); Nintendo face buttons"},
    // The controls card (#193, controls_card.h).
    {.key = "controls_card",
     .group = SettingGroup::kControls,
     .label = "Controls card",
     .help = "One page of the controls for the device in use (keyboard, gamepad or touch)",
     .button = "Show..."},
    {.key = "game_options",
     .group = SettingGroup::kGameplay,
     .label = "Game options",
     .help = "The game's own options (sound, speed, vibration): its popup menu, in the field"},
    // The saves (#148, ADR 0078; #192: the dialogs, the list, the folder; save_menu.h).
    {.key = "export_saves",
     .group = SettingGroup::kSaves,
     .label = "Export saves...",
     .help = "Writes a zip of the save files where you choose (without a file dialog: the "
             "exports folder in the data folder)"},
    {.key = "import_saves",
     .group = SettingGroup::kSaves,
     .label = "Import saves...",
     .help = "From the title: a zip of saves you choose (without a file dialog: the newest in "
             "the import folder); the old files are backed up"},
    {.key = "backups",
     .group = SettingGroup::kSaves,
     .label = "Backups",
     .help = "The earlier saves kept per character, newest first: choose one to restore it "
             "(from the title)",
     .button = "List..."},
#if !defined(SDL_PLATFORM_IOS)
    {.key = "open_data_folder",
     .group = SettingGroup::kSaves,
     .label = "Open data folder",
     .help = "Shows the folder with the saves, the backups, the exports and settings.ini"},
#endif
    // The local achievements (#154, ADR 0084; local_game_center.h's achievement_rows).
    {.key = "achievements",
     .group = SettingGroup::kGameplay,
     .label = "Achievements",
     .help = "How many are unlocked; choose again to go through them one by one"},
    // The touch key pad's editor (#159, ADR 0086).
    {.key = "keypad_layout_edit",
     .group = SettingGroup::kControls,
     .label = "Edit the touch key pad",
     .help = "Move, resize and fade the touch key pad's groups (once the game shows its pad)"},
#if !defined(SDL_PLATFORM_IOS)
    // Quit (#179): at the bottom of every tab; game.cpp's handler sends SDL_EVENT_QUIT.
    {.key = "quit",
     .group = SettingGroup::kOther,
     .label = "Quit",
     .help = "Quits MapleThief, as Cmd-Q or closing the window does (autosave = on_quit saves "
             "first, where the game allows it)",
     .every_tab = true,
     .confirm = "Quit MapleThief?",
     .button = "Quit..."},
#endif
    // A new action row: one line here plus g.menu.set_action("key", fn) in game.cpp's
    // start_menu.
};

const Preset kPresetCycle[] = {Preset::kOriginal, Preset::kOriginalController, Preset::kEnhanced,
                               Preset::kCustom};

// `text` in at most `lines` lines of at most `max` characters, broken at blanks.
std::vector<std::string> wrap(const std::string &text, std::size_t max, std::size_t lines) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size() && out.size() < lines) {
        std::size_t end = std::min(text.size(), at + max);
        if (end < text.size()) {
            const std::size_t blank = text.rfind(' ', end);
            if (blank != std::string::npos && blank > at) {
                end = blank;
            }
        }
        std::string line = text.substr(at, end - at);
        at = end;
        while (at < text.size() && text[at] == ' ') {
            at++;
        }
        if (out.size() + 1 == lines && at < text.size()) {
            line = line.substr(0, std::min(line.size(), max - 3)) + "...";
        }
        out.push_back(line);
    }
    return out;
}

std::string fit(const std::string &s, std::size_t max) {
    return s.size() <= max ? s : s.substr(0, max - 3) + "...";
}

const char *tab_note(SettingGroup g) {
    switch (g) {
    case SettingGroup::kAssists:
        return "ASSISTS CHANGE HOW THE GAME PLAYS. No preset ever turns one on.";
    case SettingGroup::kSaves:
        return "The game's save files (Documents): copies in and out.";
    default:
        return nullptr;
    }
}

bool in_box(float x, float y, int bx, int by, int bw, int bh) {
    return x >= static_cast<float>(bx) && x < static_cast<float>(bx + bw) &&
           y >= static_cast<float>(by) && y < static_cast<float>(by + bh);
}

} // namespace

std::span<const MenuActionInfo> menu_actions() { return kActions; }

const MenuActionInfo *find_menu_action(const char *key) {
    for (const MenuActionInfo &a : kActions) {
        if (std::strcmp(a.key, key) == 0) {
            return &a;
        }
    }
    return nullptr;
}

std::string frame_cost_text(const RecentStats &r) {
    if (r.frames == 0) {
        return "no frames yet";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f ms, %.0f fps", r.frontend_ms, r.fps);
    return buf;
}

GearPlace gear_place(const Placement &p, float density) {
    const float d = density > 0 ? density : 1;
    const float g = std::round(kGearPoints * d);
    const float m = std::round(4 * d);
    const Rect &img = p.rect;
    const Rect &safe = p.safe;
    GearPlace out;
    const float right_bar = safe.x + safe.w - (img.x + img.w);
    const float top_bar = img.y - safe.y;
    if (right_bar >= g + 2 * m) {
        out.rect = Rect{img.x + img.w + std::floor((right_bar - g) / 2), safe.y + m, g, g};
    } else if (top_bar >= g + 2 * m) {
        out.rect = Rect{safe.x + safe.w - g - m, safe.y + std::floor((top_bar - g) / 2), g, g};
    } else {
        out.rect = Rect{img.x + img.w - g - m, img.y + m, g, g};
        out.over_image = true;
    }
    return out;
}

GearPlace gear_place(const Placement &image, const Placement &wide, float density) {
    const GearPlace beside_wide = gear_place(wide, density);
    return beside_wide.over_image ? gear_place(image, density) : beside_wide;
}

SettingsMenu::SettingsMenu(std::span<const SettingInfo> rows, Actions *actions, Gamepads *gamepads,
                           Input *input)
    : rows_(rows), actions_(actions), gamepads_(gamepads), input_(input) {
    for (const SettingGroup g :
         {SettingGroup::kVideo, SettingGroup::kAudio, SettingGroup::kControls,
          SettingGroup::kGameplay, SettingGroup::kAccessibility, SettingGroup::kAssists,
          SettingGroup::kSaves, SettingGroup::kOther}) {
        const bool has =
            std::any_of(rows_.begin(), rows_.end(),
                        [g](const SettingInfo &r) { return r.group == g; }) ||
            std::any_of(std::begin(kActions), std::end(kActions),
                        [g](const MenuActionInfo &a) { return a.group == g && !a.every_tab; });
        if (has) {
            tabs_.push_back(g);
        }
    }
    build_items();
}

void SettingsMenu::set_action(const std::string &key, ActionFn fn) {
    const MenuActionInfo *info = find_menu_action(key.c_str());
    if (info == nullptr) {
        SDL_Log("settings: menu: no action row '%s'", key.c_str());
        return;
    }
    for (auto &h : handlers_) {
        if (h.first == info) {
            h.second = std::move(fn);
            return;
        }
    }
    handlers_.emplace_back(info, std::move(fn));
}

void SettingsMenu::set_frame_cost(const RecentStats &recent) {
    frame_cost_ = recent;
    dirty_ = true;
}

void SettingsMenu::build_items() {
    items_.clear();
    items_.push_back(Item{ItemKind::kPreset});
    if (list_open_) {
        for (std::size_t i = 0; i < list_.rows.size(); i++) {
            items_.push_back(Item{ItemKind::kListRow, nullptr, nullptr, i});
        }
        selected_ = std::clamp<std::size_t>(selected_, 1, items_.size() - 1);
        scroll_to_selection();
        return;
    }
    if (tabs_.empty()) {
        return;
    }
    const SettingGroup g = tabs_[tab_];
    for (const SettingInfo &r : rows_) {
        if (r.group == g) {
            items_.push_back(Item{ItemKind::kSetting, &r});
        }
    }
    for (const MenuActionInfo &a : kActions) {
        if (a.group == g && !a.every_tab) {
            items_.push_back(Item{ItemKind::kAction, nullptr, &a});
        }
    }
    for (const MenuActionInfo &a : kActions) {
        if (a.every_tab) {
            items_.push_back(Item{ItemKind::kAction, nullptr, &a});
        }
    }
    selected_ = std::min(selected_, items_.size() - 1);
    scroll_to_selection();
}

void SettingsMenu::set_tab(std::size_t tab) {
    if (tabs_.empty()) {
        return;
    }
    tab_ = tab % tabs_.size();
    confirming_ = false;
    list_open_ = false;
    selected_ = selected_ == 0 ? 0 : 1;
    top_ = 0;
    build_items();
    say("");
    dirty_ = true;
}

void SettingsMenu::open() {
    if (live_ == nullptr || open_) {
        return;
    }
    if (actions_ != nullptr) {
        actions_->release_source(InputSource::kKeyboard);
    }
    if (gamepads_ != nullptr) {
        gamepads_->release_all();
        if (actions_ != nullptr) {
            actions_->release_source(InputSource::kGamepad);
        }
    }
    if (input_ != nullptr) {
        input_->release_all();
    }
    open_ = true;
    opens_++;
    confirming_ = false;
    stick_x_ = stick_y_ = 0;
    left_trigger_ = false;
    pressed_what_ = 0;
    pressed_index_ = -1;
    build_items();
    say("");
    dirty_ = true;
    SDL_Log("settings: the settings menu opens (the game is paused)");
}

void SettingsMenu::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    confirming_ = false;
    if (list_open_) {
        list_open_ = false;
        selected_ = list_from_;
        top_ = 0;
        build_items();
    }
    dirty_ = true;
    SDL_Log("settings: the settings menu closes%s", changes_.empty() ? "" : " (changed)");
}

void SettingsMenu::say(std::string text) {
    status_ = std::move(text);
    dirty_ = true;
}

void SettingsMenu::cancel_confirm() {
    confirming_ = false;
    say("");
}

void SettingsMenu::open_list(MenuList list) {
    if (list.rows.empty()) {
        return;
    }
    if (!list_open_) {
        list_from_ = selected_;
    }
    list_ = std::move(list);
    list_open_ = true;
    confirming_ = false;
    selected_ = 1;
    top_ = 0;
    build_items();
    dirty_ = true;
    SDL_Log("settings: menu: the list '%s' (%zu rows)", list_.title.c_str(), list_.rows.size());
}

void SettingsMenu::close_list() {
    if (!list_open_) {
        return;
    }
    list_open_ = false;
    confirming_ = false;
    selected_ = list_from_;
    top_ = 0;
    build_items();
    dirty_ = true;
}

void SettingsMenu::page(int dir) {
    if (!list_open_) {
        move(dir);
        return;
    }
    const long last = static_cast<long>(items_.size()) - 1;
    const long to =
        std::clamp(static_cast<long>(selected_) + dir * static_cast<long>(kVisibleRows), 1L, last);
    if (static_cast<std::size_t>(to) == selected_) {
        return;
    }
    selected_ = static_cast<std::size_t>(to);
    scroll_to_selection();
    confirming_ = false;
    say("");
    dirty_ = true;
}

void SettingsMenu::scroll_to_selection() {
    if (selected_ == 0) {
        return; // the preset line is above the list
    }
    const std::size_t row = selected_ - 1;
    if (row < top_) {
        top_ = row;
    } else if (row >= top_ + kVisibleRows) {
        top_ = row + 1 - kVisibleRows;
    }
}

void SettingsMenu::scroll(int delta) {
    const std::size_t rows = items_.size() - 1;
    const std::size_t max_top = rows > kVisibleRows ? rows - kVisibleRows : 0;
    const long t = std::clamp(static_cast<long>(top_) + delta, 0L, static_cast<long>(max_top));
    if (static_cast<std::size_t>(t) != top_) {
        top_ = static_cast<std::size_t>(t);
        dirty_ = true;
    }
}

void SettingsMenu::move(int delta) {
    if (list_open_) {
        // The list's rows only (1...n-1), wrapping.
        const long n = static_cast<long>(items_.size()) - 1;
        selected_ =
            static_cast<std::size_t>(((static_cast<long>(selected_) - 1 + delta) % n + n) % n + 1);
    } else {
        const long n = static_cast<long>(items_.size());
        selected_ = static_cast<std::size_t>(((static_cast<long>(selected_) + delta) % n + n) % n);
    }
    scroll_to_selection();
    confirming_ = false;
    say("");
    dirty_ = true;
}

bool SettingsMenu::waits(const SettingInfo &row) const {
    if (live_ == nullptr || setting_text(*live_, row) == setting_text(started_, row)) {
        return false;
    }
    return row.applies == Applies::kNextStart ||
           std::find(failed_.begin(), failed_.end(), &row) != failed_.end();
}

void SettingsMenu::after_change(const Settings &before, const std::string &key,
                                const std::string &value) {
    changes_.emplace_back(key, value);
    for (const SettingInfo &r : rows_) {
        if (setting_text(before, r) == setting_text(*live_, r)) {
            continue;
        }
        bool now = false;
        if (r.applies == Applies::kAtOnce && apply_) {
            now = apply_(r, *live_);
        }
        const auto f = std::find(failed_.begin(), failed_.end(), &r);
        if (r.applies == Applies::kAtOnce && !now && f == failed_.end()) {
            failed_.push_back(&r);
        } else if (now && f != failed_.end()) {
            failed_.erase(f);
        }
        SDL_Log("settings: menu: %s = %s (%s)", r.key, setting_text(*live_, r).c_str(),
                now ? "applied" : "at the next start");
    }
    if (before.preset != live_->preset) {
        SDL_Log("settings: menu: preset %s", live_->preset ? preset_name(*live_->preset) : "none");
    }
    dirty_ = true;
}

void SettingsMenu::step(int delta) {
    if (live_ == nullptr) {
        return;
    }
    const Item &it = items_[selected_];
    if (confirming_) {
        confirm_yes_ = delta > 0; // No on the left, Yes on the right
        dirty_ = true;
        return;
    }
    if (it.kind == ItemKind::kPreset) {
        long i = -1;
        for (long k = 0; k < 4; k++) {
            if (live_->preset == kPresetCycle[k]) {
                i = k;
            }
        }
        i = i < 0 ? (delta >= 0 ? 0 : 3) : ((i + delta) % 4 + 4) % 4;
        const Preset p = kPresetCycle[i];
        const Settings before = *live_;
        apply_setting(live_, kPresetKey, preset_name(p), nullptr);
        after_change(before, kPresetKey, preset_name(p));
        std::size_t changed = 0;
        for (const SettingInfo &r : rows_) {
            changed += setting_text(before, r) != setting_text(*live_, r) ? 1 : 0;
        }
        say(std::string(preset_title(p)) + ": " + std::to_string(changed) +
            (changed == 1 ? " setting changed" : " settings changed"));
        return;
    }
    if (it.kind != ItemKind::kSetting) {
        return;
    }
    const SettingInfo &r = *it.setting;
    const Settings before = *live_;
    if (!step_setting(live_, r, delta)) {
        say(std::string("Set ") + r.key + " in settings.ini: " + setting_expects(r));
        return;
    }
    const std::string value = setting_text(*live_, r);
    if (value == setting_text(before, r)) {
        return; // a range at its end
    }
    after_change(before, r.key, value);
    say(std::string(r.label) + ": " + value + (waits(r) ? ", at the next start" : ""));
}

void SettingsMenu::choose() {
    const Item &it = items_[selected_];
    if (it.kind == ItemKind::kListRow) {
        const std::size_t row = it.row;
        const std::string question = list_.question ? list_.question(row) : std::string();
        if (!question.empty() && !confirming_) {
            confirming_ = true;
            confirm_yes_ = false;
            say("Right picks Yes, choose again to confirm");
            return;
        }
        const bool yes = question.empty() || confirm_yes_;
        cancel_confirm();
        if (!yes) {
            return;
        }
        const std::function<std::string(std::size_t)> fn = list_.choose;
        close_list();
        say(fn ? fn(row) : std::string());
        return;
    }
    if (it.kind != ItemKind::kAction) {
        step(+1);
        return;
    }
    const bool has = std::any_of(handlers_.begin(), handlers_.end(),
                                 [&](const auto &h) { return h.first == it.action && h.second; });
    if (it.action->confirm != nullptr && has) {
        if (!confirming_) {
            confirming_ = true;
            confirm_yes_ = false;
            say(std::string(it.action->confirm) + " Right picks Yes, choose again to confirm");
            return;
        }
        const bool yes = confirm_yes_;
        cancel_confirm();
        if (!yes) {
            return;
        }
    }
    for (auto &h : handlers_) {
        if (h.first == it.action && h.second) {
            say(h.second());
            return;
        }
    }
    say(std::string(it.action->label) + " is not available in this build");
}

bool SettingsMenu::hotkey(const SDL_Event &e) const {
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F1) {
        return true;
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const int b = e.gbutton.button;
        if (b == SDL_GAMEPAD_BUTTON_GUIDE) {
            return true;
        }
        if (b == SDL_GAMEPAD_BUTTON_LEFT_STICK || b == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(e.gbutton.which);
            const SDL_GamepadButton other = b == SDL_GAMEPAD_BUTTON_LEFT_STICK
                                                ? SDL_GAMEPAD_BUTTON_RIGHT_STICK
                                                : SDL_GAMEPAD_BUTTON_LEFT_STICK;
            return pad != nullptr && SDL_GetGamepadButton(pad, other);
        }
    }
    return false;
}

void SettingsMenu::track(const SDL_Event &e) {
    const unsigned before = glyphs_.changes();
    glyphs_.handle(e, gamepads_ != nullptr ? gamepads_->active_id() : 0);
    if (open_ && glyphs_.changes() != before) {
        dirty_ = true;
    }
}

bool SettingsMenu::handle(const SDL_Event &e) {
    if (!open_) {
        if (allowed_ && live_ != nullptr && hotkey(e)) {
            open();
            return true;
        }
        return false;
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = e.key.key;
        const bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        if (list_open_ && shift && (k == SDLK_UP || k == SDLK_DOWN)) {
            page(k == SDLK_UP ? -1 : 1); // Shift with the arrows pages (ADR 0114)
        } else if (list_open_ && (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN)) {
            page(k == SDLK_PAGEUP ? -1 : 1);
        } else if (k == SDLK_UP || k == SDLK_W) {
            move(-1);
        } else if (k == SDLK_DOWN || k == SDLK_S) {
            move(+1);
        } else if (k == SDLK_LEFT || k == SDLK_A) {
            step(-1);
        } else if (k == SDLK_RIGHT || k == SDLK_D) {
            step(+1);
        } else if (e.key.repeat) {
            // Only the cursor and the values repeat.
        } else if (k == SDLK_TAB) {
            set_tab(tab_ + (shift ? tabs_.size() - 1 : 1));
        } else if (k == SDLK_PAGEDOWN || k == SDLK_E) {
            set_tab(tab_ + 1);
        } else if (k == SDLK_PAGEUP || k == SDLK_Q) {
            set_tab(tab_ + tabs_.size() - 1);
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
            choose();
        } else if (confirming_ && (k == SDLK_ESCAPE || k == SDLK_BACKSPACE)) {
            cancel_confirm();
        } else if (list_open_ && (k == SDLK_ESCAPE || k == SDLK_BACKSPACE)) {
            close_list();
        } else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_F1) {
            close();
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const auto b = static_cast<SDL_GamepadButton>(e.gbutton.button);
        const FaceButtons face =
            gamepads_ != nullptr ? gamepads_->face_buttons() : FaceButtons::kPosition;
        const Control c = gamepad_button_control(SDL_GetGamepadTypeForID(e.gbutton.which), b, face);
        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
            const int dir = b == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : 1;
            if (left_trigger_ && list_open_) {
                page(dir); // LT with up / down pages (ADR 0114)
            } else {
                move(dir);
            }
        } else if (list_open_ && (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ||
                                  b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
            page(b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ? -1 : 1);
        } else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
            step(-1);
        } else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
            step(+1);
        } else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
            set_tab(tab_ + tabs_.size() - 1);
        } else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
            set_tab(tab_ + 1);
        } else if (confirming_ && c.code == SDL_GAMEPAD_BUTTON_EAST) {
            cancel_confirm();
        } else if (list_open_ && c.code == SDL_GAMEPAD_BUTTON_EAST) {
            close_list();
        } else if (b == SDL_GAMEPAD_BUTTON_GUIDE || b == SDL_GAMEPAD_BUTTON_START ||
                   c.code == SDL_GAMEPAD_BUTTON_EAST) {
            close();
        } else if (c.code == SDL_GAMEPAD_BUTTON_SOUTH) {
            choose();
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const int v = e.gaxis.value;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            left_trigger_ = left_trigger_ ? v > 8192 : v > 16384;
            return true;
        }
        const bool y = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
        if (y || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            int &held = y ? stick_y_ : stick_x_;
            const int dir = v < -16384 ? -1 : v > 16384 ? 1 : 0;
            if (dir != 0 && held == 0) {
                held = dir;
                if (y && left_trigger_ && list_open_) {
                    page(dir);
                } else if (y) {
                    move(dir);
                } else {
                    step(dir);
                }
            } else if (v > -8192 && v < 8192) {
                held = 0;
            }
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        const float w = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        if (w != 0) {
            scroll(w > 0 ? -1 : 1);
        }
        return true;
    }
    default:
        return false;
    }
}

std::pair<int, int> SettingsMenu::tab_box(std::size_t i) const {
    int x = kListX;
    for (std::size_t k = 0; k < tabs_.size(); k++) {
        const int w = Canvas::text_width(setting_group_name(tabs_[k])) + 10;
        if (tabs_[k] == SettingGroup::kAssists) {
            x += 6; // set apart from the enhancements
        }
        if (k == i) {
            return {x, w};
        }
        x += w + 2;
    }
    return {0, 0};
}

int SettingsMenu::tab_at(float x, float y) const {
    for (std::size_t i = 0; i < tabs_.size(); i++) {
        const auto [bx, bw] = tab_box(i);
        if (in_box(x, y, bx, kTabsY, bw, kTabH)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int SettingsMenu::item_at(float x, float y) const {
    if (x < kListX || x >= kListX + kListW) {
        return -1;
    }
    if (in_box(x, y, kListX, kPresetY, kListW, kPresetH)) {
        return list_open_ ? -1 : 0; // a list's preset line is not a row
    }
    if (y < kListY) {
        return -1;
    }
    const std::size_t row = static_cast<std::size_t>((y - kListY) / kRowH);
    if (row >= kVisibleRows || top_ + row + 1 >= items_.size()) {
        return -1;
    }
    return static_cast<int>(top_ + row + 1);
}

void SettingsMenu::pointer(const ScreenPoint &p, bool down, bool up) {
    if (!open_) {
        return;
    }
    // What is under the pointer: 1 the close box, 2 / 3 the list's ^ / v, 4 a tab, 5 an item.
    int what = 0;
    int index = -1;
    int zone = 0; // on an item's value: -1 "<", +1 ">", 2 the rest of the value
    if (p.inside) {
        if (in_box(p.x, p.y, kCloseX, kCloseY, kCloseW, 18)) {
            what = 1;
        } else if (in_box(p.x, p.y, kScrollX, kListY, kScrollW, 2 * kRowH)) {
            what = 2;
        } else if (in_box(p.x, p.y, kScrollX, kListY + static_cast<int>(kVisibleRows - 2) * kRowH,
                          kScrollW, 2 * kRowH)) {
            what = 3;
        } else if ((index = tab_at(p.x, p.y)) >= 0) {
            what = 4;
        } else if ((index = item_at(p.x, p.y)) >= 0) {
            what = 5;
            if (p.x >= kValueX && p.x < kValueX + kArrowW) {
                zone = -1;
            } else if (p.x >= kValueEnd - kArrowW && p.x < kValueEnd) {
                zone = 1;
            } else if (p.x >= kValueX && p.x < kValueEnd) {
                zone = 2;
            }
        }
    }
    if (what == 5 && static_cast<std::size_t>(index) != selected_) {
        selected_ = static_cast<std::size_t>(index);
        confirming_ = false;
        say("");
        dirty_ = true;
    }
    if (down) {
        pressed_what_ = what;
        pressed_index_ = index;
        pressed_zone_ = zone;
    }
    if (!up) {
        return;
    }
    const bool same =
        what != 0 && what == pressed_what_ && index == pressed_index_ && zone == pressed_zone_;
    pressed_what_ = 0;
    pressed_index_ = -1;
    if (!same) {
        return;
    }
    switch (what) {
    case 1:
        close();
        break;
    case 2:
        scroll(-1);
        break;
    case 3:
        scroll(+1);
        break;
    case 4:
        set_tab(static_cast<std::size_t>(index));
        break;
    case 5:
        if (items_[selected_].kind == ItemKind::kAction ||
            items_[selected_].kind == ItemKind::kListRow) {
            if (confirming_ && (zone == -1 || zone == 1)) {
                step(zone); // "<" No, ">" Yes
            } else {
                choose();
            }
        } else if (zone != 0) {
            step(zone == -1 ? -1 : +1);
        }
        break;
    default:
        break;
    }
}

std::vector<std::pair<std::string, std::string>> SettingsMenu::take_changes() {
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(changes_);
    return out;
}

void SettingsMenu::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    const SDL_GamepadType type = glyphs_.type();
    const FaceButtons face =
        gamepads_ != nullptr ? gamepads_->face_buttons() : FaceButtons::kPosition;
    // The test pattern (#187): with a row selected that asks for it (the picture adjustments), the
    // list's slots that show no such row are left as the game drew them (no backdrop, no panel),
    // and the first of them gets a grey ramp, so a change shows at once in the game's own picture
    // and on the ramp (the present stage's colour step grades the menu with the picture).
    const Item &selected_item = items_[selected_];
    const bool pattern =
        selected_item.kind == ItemKind::kSetting && selected_item.setting->test_pattern;
    const auto pattern_row = [&](std::size_t r) {
        const std::size_t i = top_ + r + 1;
        return i < items_.size() && items_[i].kind == ItemKind::kSetting &&
               items_[i].setting->test_pattern;
    };
    // The backdrop and the panel over (x, y, w, h).
    const auto shade = [&](int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) {
            return;
        }
        c.fill(x, y, w, h, kBackdrop);
        const int px = std::max(x, kPanelX);
        const int py = std::max(y, kPanelY);
        const int pw = std::min(x + w, kPanelX + kPanelW) - px;
        const int ph = std::min(y + h, kPanelY + kPanelH) - py;
        if (pw > 0 && ph > 0) {
            c.fill(px, py, pw, ph, kPanel);
        }
    };
    constexpr int kListH = static_cast<int>(kVisibleRows) * kRowH;
    if (!pattern) {
        shade(0, 0, c.width(), c.height());
    } else {
        shade(0, 0, c.width(), kListY);
        shade(0, kListY + kListH, c.width(), c.height() - kListY - kListH);
        shade(0, kListY, kListX, kListH);
        shade(kListX + kListW, kListY, c.width() - kListX - kListW, kListH);
        bool ramp = false;
        for (std::size_t r = 0; r < kVisibleRows; r++) {
            const int y = kListY + static_cast<int>(r) * kRowH;
            if (pattern_row(r)) {
                shade(kListX, y, kListW, kRowH);
            } else if (!ramp) {
                // 16 greys, black to white, on the panel.
                ramp = true;
                shade(kListX, y, kListW, kRowH);
                constexpr int kSteps = 16;
                constexpr int kStepW = kListW / kSteps;
                const int x0 = kListX + (kListW - kSteps * kStepW) / 2;
                for (int k = 0; k < kSteps; k++) {
                    const auto v = static_cast<std::uint8_t>(k * 255 / (kSteps - 1));
                    c.fill(x0 + k * kStepW, y + 2, kStepW, kRowH - 4, Rgba{v, v, v, 255});
                }
            }
        }
    }
    c.frame(kPanelX, kPanelY, kPanelW, kPanelH, 1, kBorder);
    c.text(kTextX, 12, "SETTINGS", kText, 2);
    // The close box.
    c.frame(kCloseX, kCloseY, kCloseW, 18, 1, kBorder);
    c.cross(kCloseX + 6, kCloseY + 4, 10, 1, kText);

    const Settings shown = live_ != nullptr ? *live_ : Settings{};
    const auto value_box = [&](int y, int h, const std::string &value, bool arrows, bool on) {
        const int ty = y + (h - Canvas::kGlyphHeight) / 2;
        if (arrows) {
            c.text(kValueX + 4, ty, "<", on ? kText : kDim);
            c.text(kValueEnd - kArrowW + 5, ty, ">", on ? kText : kDim);
        }
        const std::string v = fit(value, static_cast<std::size_t>((kValueEnd - kValueX) / 6 - 5));
        const int vx = kValueX + (kValueEnd - kValueX - Canvas::text_width(v)) / 2;
        c.text(vx, ty, v, kText);
    };

    // The preset line.
    {
        const bool on = selected_ == 0;
        if (on) {
            c.fill(kListX, kPresetY, kListW, kPresetH, kSelection);
        }
        c.text(kTextX, kPresetY + 3, "Preset", kHeader);
        std::string title = shown.preset ? preset_title(*shown.preset) : "None yet";
        std::string summary;
        const std::optional<Preset> same = matching_preset(shown);
        if (!shown.preset) {
            summary = "No preset chosen: the defaults";
            if (same) {
                summary += std::string(" (as ") + preset_title(*same) + ")";
            }
        } else if (*shown.preset == Preset::kCustom) {
            summary = same ? std::string("Your own values (as ") + preset_title(*same) + ")"
                           : std::string("Your own values");
        } else {
            summary = preset_summary(*shown.preset);
        }
        c.text(kTextX, kPresetY + 13, fit(summary, kMaxLine), on ? kText : kDim);
        value_box(kPresetY, 12, title, true, on);
    }

    // The tabs.
    for (std::size_t i = 0; i < tabs_.size(); i++) {
        const auto [bx, bw] = tab_box(i);
        const bool on = i == tab_;
        const bool assists = tabs_[i] == SettingGroup::kAssists;
        c.fill(bx, kTabsY, bw, kTabH, on ? kSelection : kTabOff);
        if (on) {
            c.frame(bx, kTabsY, bw, kTabH, 1, assists ? kAssist : kBorder);
        }
        c.text(bx + 5, kTabsY + 3, setting_group_name(tabs_[i]),
               assists ? kAssist : (on ? kText : kDim));
    }
    c.fill(kListX, kTabsY + kTabH, kListW, 1, kBorder);
    if (list_open_) {
        c.text(kTextX, kNoteY, fit(list_.title, kMaxLine), kHeader);
    } else if (!tabs_.empty()) {
        if (const char *note = tab_note(tabs_[tab_]); note != nullptr) {
            c.text(kTextX, kNoteY, fit(note, kMaxLine),
                   tabs_[tab_] == SettingGroup::kAssists ? kAssist : kDim);
        }
    }

    // The rows.
    for (std::size_t r = 0; r < kVisibleRows && top_ + r + 1 < items_.size(); r++) {
        const std::size_t i = top_ + r + 1;
        const Item &it = items_[i];
        const int y = kListY + static_cast<int>(r) * kRowH;
        const bool on = i == selected_;
        if (pattern && !pattern_row(r)) {
            continue; // the test pattern's slot
        }
        if (on) {
            c.fill(kListX, y, kListW, kRowH, kSelection);
        }
        const int ty = y + (kRowH - Canvas::kGlyphHeight) / 2;
        if (it.kind == ItemKind::kListRow) {
            const MenuListRow &lr = list_.rows[it.row];
            c.text(kTextX, ty, fit(lr.label, static_cast<std::size_t>((kValueX - kTextX) / 6 - 1)),
                   kText);
            value_box(y, kRowH, on && confirming_ ? (confirm_yes_ ? "Yes" : "No") : lr.value,
                      on && confirming_, on);
            continue;
        }
        if (it.kind == ItemKind::kAction) {
            c.text(kTextX, ty, it.action->label, kText);
            const bool has = std::any_of(handlers_.begin(), handlers_.end(), [&](const auto &h) {
                return h.first == it.action && h.second;
            });
            if (on && confirming_) {
                value_box(y, kRowH, confirm_yes_ ? "Yes" : "No", true, on);
            } else {
                value_box(y, kRowH, has ? it.action->button : "not available", false, on);
            }
            continue;
        }
        const SettingInfo &row = *it.setting;
        int lx = kTextX + c.text(kTextX, ty, fit(row.label, 34), kText);
        if (row.frame_cost) {
            const std::string cost = frame_cost_text(frame_cost_);
            const int room = (kValueX - 4 - lx - 6) / 6;
            if (room > 4) {
                lx += 6;
                c.text(lx, ty, fit(cost, static_cast<std::size_t>(room)), kDim);
            }
        }
        const std::string value = live_ != nullptr ? setting_text(*live_, row) : "";
        value_box(y, kRowH, value, !setting_choices(row).empty(), on);
        if (waits(row)) {
            c.text(kValueEnd + 3, ty, "*", kWarning);
        }
    }
    // Scroll marks, which a click or tap also uses.
    const std::size_t rows = items_.size() - 1;
    if (top_ > 0) {
        c.text(kScrollX + 8, kListY + 4, "^", kText);
    }
    if (top_ + kVisibleRows < rows) {
        c.text(kScrollX + 8, kListY + static_cast<int>(kVisibleRows - 1) * kRowH + 4, "v", kText);
    }

    // The help of the selected item, then the status.
    std::string help;
    const Item &sel = items_[selected_];
    if (sel.kind == ItemKind::kPreset) {
        help = "Left / Right picks a preset: it sets every setting it controls. Custom keeps "
               "your own values.";
    } else if (sel.kind == ItemKind::kAction) {
        help = sel.action->help;
    } else if (sel.kind == ItemKind::kListRow) {
        help = confirming_ && list_.question ? list_.question(sel.row) : list_.help;
    } else {
        help = sel.setting->help;
        if (sel.setting->frame_cost && frame_cost_.frames != 0) {
            char buf[128];
            std::snprintf(buf, sizeof buf,
                          ". Now %.1f ms a frame in the frontend (the costliest %.1f), %.0f "
                          "frames a second",
                          frame_cost_.frontend_ms, frame_cost_.max_frontend_ms, frame_cost_.fps);
            help += buf;
        }
    }
    const std::vector<std::string> lines = wrap(help, kMaxLine, 2);
    for (std::size_t i = 0; i < lines.size(); i++) {
        c.text(kTextX, kHelpY + static_cast<int>(i) * 10, lines[i], kDim);
    }
    std::string status = status_;
    Rgba status_colour = kGood;
    if (status.empty() && sel.kind == ItemKind::kSetting && waits(*sel.setting)) {
        status = "* applies at the next start";
        status_colour = kWarning;
    }
    if (!status.empty()) {
        c.text(kTextX, kStatusY, fit(status, kMaxLine), status_colour);
    }

    // The hints, with the pad's own glyphs.
    int x = kTextX - 6;
    const auto key = [&](SDL_Keycode k) {
        x +=
            draw_glyph(c, x, kHintsY, Control{ControlKind::kKey, static_cast<int>(k)}, type, face) +
            2;
    };
    const auto button = [&](int b) {
        x += draw_glyph(c, x, kHintsY, Control{ControlKind::kGamepadButton, b}, type, face) + 2;
    };
    const auto direction = [&](Direction d) {
        x += draw_glyph(c, x, kHintsY, Control{ControlKind::kDirection, static_cast<int>(d)}, type,
                        face) +
             2;
    };
    const auto words = [&](const char *s) { x += c.text(x, kHintsY + 3, s, kDim) + 7; };
    key(SDLK_UP);
    key(SDLK_DOWN);
    direction(Direction::kUp);
    words("move");
    if (list_open_) {
        // A list's own hints (#192): page (ADR 0114), choose, back.
        key(SDLK_LSHIFT);
        x += draw_glyph(c, x, kHintsY,
                        Control{ControlKind::kGamepadTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, type,
                        face) +
             2;
        words("page");
        key(SDLK_RETURN);
        button(SDL_GAMEPAD_BUTTON_SOUTH);
        words("choose");
        key(SDLK_ESCAPE);
        button(SDL_GAMEPAD_BUTTON_EAST);
        words("back");
        return;
    }
    key(SDLK_LEFT);
    key(SDLK_RIGHT);
    direction(Direction::kRight);
    words("change");
    key(SDLK_TAB);
    button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    words("tab");
    key(SDLK_ESCAPE);
    button(SDL_GAMEPAD_BUTTON_EAST);
    words("close");
}

namespace {

bool is_trackpad(SDL_TouchID id) {
    const SDL_TouchDeviceType type = SDL_GetTouchDeviceType(id);
    return type == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE || type == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE;
}

bool is_real_mouse(SDL_MouseID which) {
    return which != SDL_TOUCH_MOUSEID && which != SDL_PEN_MOUSEID;
}

bool is_screen_finger(const SDL_Event &e) {
    return e.tfinger.touchID != SDL_MOUSE_TOUCHID && !is_trackpad(e.tfinger.touchID);
}

// A finger's window point (SDL gives it normalised to the window).
void finger_point(const SDL_Event &e, SDL_Window *window, float *x, float *y) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    *x = e.tfinger.x * static_cast<float>(ww);
    *y = e.tfinger.y * static_cast<float>(wh);
}

} // namespace

bool is_screen_finger_down(const SDL_Event &e) {
    return e.type == SDL_EVENT_FINGER_DOWN && is_screen_finger(e);
}

bool gear_pressed(const SDL_Event &e, SDL_Window *window, const Rect &gear) {
    float wx = 0, wy = 0;
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT &&
        is_real_mouse(e.button.which)) {
        wx = e.button.x;
        wy = e.button.y;
    } else if (is_screen_finger_down(e)) {
        finger_point(e, window, &wx, &wy);
    } else {
        return false;
    }
    float dx = 1, dy = 1;
    window_density(window, &dx, &dy);
    const float px = wx * dx, py = wy * dy;
    return gear.w > 0 && px >= gear.x && px < gear.x + gear.w && py >= gear.y &&
           py < gear.y + gear.h;
}

bool menu_pointer_event(SettingsMenu &menu, const SDL_Event &e, SDL_Window *window,
                        const Placement &placement) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (is_real_mouse(e.motion.which)) {
            menu.pointer(window_point_to_screen(window, placement, e.motion.x, e.motion.y), false,
                         false);
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT && is_real_mouse(e.button.which)) {
            menu.pointer(window_point_to_screen(window, placement, e.button.x, e.button.y),
                         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                         e.type == SDL_EVENT_MOUSE_BUTTON_UP);
        }
        return true;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
        if (is_screen_finger(e)) {
            float wx = 0, wy = 0;
            finger_point(e, window, &wx, &wy);
            menu.pointer(window_point_to_screen(window, placement, wx, wy),
                         e.type == SDL_EVENT_FINGER_DOWN, e.type == SDL_EVENT_FINGER_UP);
        }
        return true;
    case SDL_EVENT_FINGER_CANCELED:
        return true;
    default:
        return false;
    }
}

bool menu_route_event(SettingsMenu &menu, const SDL_Event &e, SDL_Window *window,
                      const Placement &placement, const Rect *gear) {
    if (menu.is_open()) {
        return menu.handle(e) || menu_pointer_event(menu, e, window, placement);
    }
    if (menu.handle(e)) {
        return true; // a hotkey opened it
    }
    if (gear != nullptr && menu.allowed() && gear_pressed(e, window, *gear)) {
        menu.open();
        return menu.is_open();
    }
    return false;
}

std::vector<std::uint8_t> GearButton::pixels(int size) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(size) * size * 4, 0);
    Canvas c(px.data(), static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size), 1);
    constexpr Rgba kBack = {0x14, 0x1A, 0x24, 200};
    constexpr Rgba kTeeth = {0xD8, 0xDE, 0xE8, 255};
    const int s = size;
    c.disc(0, 0, s, kBack);
    const int t = std::max(2, s / 6); // a tooth's width
    const int body = s * 11 / 20;     // the wheel
    const int reach = s * 15 / 20;    // the teeth's tips
    const int r0 = (s - reach) / 2;
    c.fill(s / 2 - t / 2, r0, t, reach, kTeeth);
    c.fill(r0, s / 2 - t / 2, reach, t, kTeeth);
    c.cross(r0 + reach / 7, r0 + reach / 7, reach - 2 * (reach / 7), t * 3 / 4, kTeeth);
    c.disc((s - body) / 2, (s - body) / 2, body, kTeeth);
    const int hole = s / 4;
    c.disc((s - hole) / 2, (s - hole) / 2, hole, kBack);
    return px;
}

bool GearButton::draw(SDL_Renderer *renderer, const Rect &rect, bool faint) {
    const int size = static_cast<int>(rect.w);
    if (renderer == nullptr || size < 4) {
        return false;
    }
    if (texture_ == nullptr || size_ != size) {
        stop();
        const std::vector<std::uint8_t> px = pixels(size);
        texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                     size, size);
        if (texture_ == nullptr || !SDL_UpdateTexture(texture_, nullptr, px.data(), size * 4)) {
            stop();
            return false;
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        size_ = size;
    }
    SDL_SetTextureAlphaMod(texture_, faint ? 150 : 255);
    const SDL_FRect dst{rect.x, rect.y, rect.w, rect.h};
    return SDL_RenderTexture(renderer, texture_, nullptr, &dst);
}

void GearButton::stop() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    size_ = 0;
}

bool save_menu_changes(const std::string &dir,
                       const std::vector<std::pair<std::string, std::string>> &changes) {
    Settings file;
    load_settings(dir, &file);
    for (const auto &[key, value] : changes) {
        std::string why;
        if (!apply_setting(&file, key, value, &why)) {
            SDL_Log("settings: menu: %s", why.c_str());
        }
    }
    return save_settings(dir, file);
}

} // namespace maplethief::frontend
