// The remap overlay (#70, ADR 0054); see remap.h.
#include "remap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "canvas.h"
#include "maplethief/input.h"
#include "port_text.h"
#include "switch_pads.h"

namespace maplethief::frontend {

namespace {

constexpr Rgba kBackdrop = {0x00, 0x00, 0x00, kOverlayBackdropAlpha};
constexpr Rgba kPanel = {0x14, 0x1A, 0x24, 235};
constexpr Rgba kBorder = {0x5A, 0x6B, 0x85, 255};
constexpr Rgba kSelection = {0x2E, 0x4A, 0x7A, 255};
constexpr Rgba kHeader = {0xF2, 0xB8, 0x4B, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};
constexpr Rgba kWarning = {0xFF, 0xD1, 0x66, 255};
constexpr Rgba kGood = {0x9B, 0xE2, 0x9B, 255};

// The layout, in screen points (480 x 320).
constexpr int kPanelX = 8, kPanelY = 6, kPanelW = 464, kPanelH = 308;
constexpr int kListY = 46, kRowH = 14;
constexpr int kTitleX = 20, kKeysX = 128, kKeysEnd = 300, kPadX = 306, kPadEnd = 464;
constexpr int kStatusY = kListY + static_cast<int>(Remap::kVisibleRows) * kRowH + 4;

const char *context_title(Context c) {
    return tr(c == Context::kField ? Txt::card_in_field : Txt::card_in_menus);
}
const char *in_context(Context c) {
    return tr(c == Context::kField ? Txt::remap_in_field : Txt::remap_in_menus);
}

bool is_mouse_or_touch(Uint32 type) {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP ||
           type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_FINGER_DOWN || type == SDL_EVENT_FINGER_UP ||
           type == SDL_EVENT_FINGER_MOTION || type == SDL_EVENT_FINGER_CANCELED;
}

SDL_GamepadType type_of(SDL_JoystickID id) { return pad_type(id); }

} // namespace

const char *action_title(std::uint32_t action) {
    switch (action) {
    case MT_ACTION_UP:
        return tr(Txt::action_up);
    case MT_ACTION_DOWN:
        return tr(Txt::action_down);
    case MT_ACTION_LEFT:
        return tr(Txt::action_left);
    case MT_ACTION_RIGHT:
        return tr(Txt::action_right);
    case MT_ACTION_JUMP:
        return tr(Txt::action_jump);
    case MT_ACTION_ATTACK:
        return tr(Txt::action_attack);
    case MT_ACTION_SLOT1:
        return tr(Txt::action_slot1);
    case MT_ACTION_SLOT2:
        return tr(Txt::action_slot2);
    case MT_ACTION_SLOT3:
        return tr(Txt::action_slot3);
    case MT_ACTION_SLOT4:
        return tr(Txt::action_slot4);
    case MT_ACTION_OK:
        return tr(Txt::action_ok);
    case MT_ACTION_BACK:
        return tr(Txt::action_back);
    case MT_ACTION_MENU:
        return tr(Txt::action_menu);
    case MT_ACTION_MAP:
        return tr(Txt::action_map);
    case MT_ACTION_CASH_SHOP:
        return tr(Txt::action_cash_shop);
    case MT_ACTION_MINUS:
        return tr(Txt::action_minus);
    case MT_ACTION_PLUS:
        return tr(Txt::action_plus);
    case MT_ACTION_REVIVE:
        return tr(Txt::action_revive);
    case MT_ACTION_GRADE_REVIVE:
        return tr(Txt::action_grade_revive);
    case MT_ACTION_FAST_FORWARD:
        return tr(Txt::action_fast_forward);
    case MT_ACTION_AUTO_RUN:
        return tr(Txt::action_auto_run);
    case MT_ACTION_PAGE:
        return tr(Txt::action_page);
    case MT_ACTION_POINTER_TAP:
        return tr(Txt::action_pointer_tap);
    case MT_ACTION_QUICK_SAVE:
        return tr(Txt::action_quick_save);
    case MT_ACTION_QUICK_LOAD:
        return tr(Txt::action_quick_load);
    default: {
        // An action this table does not know yet: the runtime table's title.
        const mt_input_action_info *info = mt_input_action_get_info(action);
        return info != nullptr ? info->title : "?";
    }
    }
}

Remap::Remap(Actions *actions, Gamepads *gamepads) : actions_(actions), gamepads_(gamepads) {
    build_rows();
}

void Remap::build_rows() {
    rows_.clear();
    bool any = false;
    Context current = Context::kField;
    for (const ActionSlot &s : action_slots(edit_.table)) {
        if (!any || s.context != current) {
            rows_.push_back(Row{RowKind::kHeader, ActionSlot{s.context, MT_ACTION_NONE}});
            current = s.context;
            any = true;
        }
        rows_.push_back(Row{RowKind::kAction, s});
    }
    for (const RowKind k : {RowKind::kFace, RowKind::kReset, RowKind::kSave, RowKind::kCancel}) {
        rows_.push_back(Row{k, ActionSlot{Context::kMenu, MT_ACTION_NONE}});
    }
    if (selected_ >= rows_.size() || rows_[selected_].kind == RowKind::kHeader) {
        selected_ = 0;
        move(+1);
    }
    scroll_to_selection();
}

void Remap::open(double now) {
    (void)now;
    if (actions_ != nullptr) {
        edit_.table = actions_->bindings();
        actions_->release_source(InputSource::kKeyboard);
    }
    if (gamepads_ != nullptr) {
        edit_.face = gamepads_->face_buttons();
        gamepads_->release_all();
        if (actions_ != nullptr) {
            actions_->release_source(InputSource::kGamepad);
        }
    }
    if (window_keys_ != nullptr) {
        edit_.window = *window_keys_; // not shown, but Save writes them (#196)
    }
    start_ = edit_;
    open_ = true;
    capturing_ = false;
    stick_ = 0;
    trigger_[0] = trigger_[1] = false;
    selected_ = 0;
    build_rows();
    say("", false);
    dirty_ = true;
    SDL_Log("input: the remap overlay opens (the game is paused)");
}

bool Remap::changed() const {
    return !same_bindings(edit_.table, start_.table) || edit_.face != start_.face ||
           edit_.window != start_.window;
}

void Remap::back() {
    if (!open_) {
        return;
    }
    if (changed()) {
        save(); // applied and written; a failed write keeps it open with the error
        return;
    }
    cancel();
}

void Remap::cancel() {
    if (!open_) {
        return;
    }
    open_ = false;
    capturing_ = false;
    dirty_ = true;
    SDL_Log("input: the remap overlay closes");
}

void Remap::say(std::string text, bool warning) {
    status_ = std::move(text);
    status_warning_ = warning;
    if (!status_.empty()) {
        SDL_Log("input: remap: %s", status_.c_str());
    }
    dirty_ = true;
}

void Remap::scroll_to_selection() {
    if (selected_ < top_) {
        top_ = selected_;
        // Keep a context's header in view above its first action.
        if (top_ > 0 && rows_[top_ - 1].kind == RowKind::kHeader) {
            top_--;
        }
    } else if (selected_ >= top_ + kVisibleRows) {
        top_ = selected_ + 1 - kVisibleRows;
    }
    const std::size_t max_top = rows_.size() > kVisibleRows ? rows_.size() - kVisibleRows : 0;
    top_ = std::min(top_, max_top);
}

void Remap::move(int delta) {
    if (rows_.empty()) {
        return;
    }
    const long n = static_cast<long>(rows_.size());
    long i = static_cast<long>(selected_);
    for (long step = 0; step < n; step++) {
        i = (i + delta + n) % n;
        if (rows_[static_cast<std::size_t>(i)].kind != RowKind::kHeader) {
            break;
        }
    }
    selected_ = static_cast<std::size_t>(i);
    scroll_to_selection();
    dirty_ = true;
}

void Remap::choose(double now) {
    const Row &row = rows_[selected_];
    switch (row.kind) {
    case RowKind::kAction:
        capturing_ = true;
        capture_until_ = now + kCaptureSeconds;
        say(tr(Txt::remap_press_for, {action_title(row.slot.action), in_context(row.slot.context)}),
            false);
        break;
    case RowKind::kFace:
        edit_.face =
            edit_.face == FaceButtons::kPosition ? FaceButtons::kLabel : FaceButtons::kPosition;
        say(tr(Txt::remap_face_by, {face_buttons_name(edit_.face)}), false);
        break;
    case RowKind::kReset:
        edit_ = BindingSet{};
        build_rows();
        say(tr(Txt::remap_defaults_back), false);
        break;
    case RowKind::kSave:
        save();
        break;
    case RowKind::kCancel:
        cancel();
        break;
    case RowKind::kHeader:
        break;
    }
}

void Remap::capture(const Control &c) {
    capturing_ = false;
    const Row &row = rows_[selected_];
    if (row.kind != RowKind::kAction) {
        return;
    }
    const std::vector<std::string> before = unbound_warnings(edit_.table);
    const std::uint32_t taken = bind_control(&edit_.table, row.slot.context, row.slot.action, c);
    std::string text = std::string(action_title(row.slot.action)) + " " +
                       in_context(row.slot.context) + ": " + control_text(c);
    if (taken != MT_ACTION_NONE) {
        text += tr(Txt::remap_taken_from, {action_title(taken)});
    }
    bool warning = false;
    for (const std::string &w : unbound_warnings(edit_.table)) {
        if (std::find(before.begin(), before.end(), w) == before.end()) {
            text += tr(Txt::remap_warning, {w});
            warning = true;
        }
    }
    build_rows();
    say(text, warning);
}

void Remap::save() {
    if (actions_ != nullptr) {
        actions_->bindings() = edit_.table;
    }
    if (gamepads_ != nullptr) {
        gamepads_->set_face_buttons(edit_.face);
    }
    if (window_keys_ != nullptr) {
        *window_keys_ = edit_.window;
    }
    for (const std::string &w : unbound_warnings(edit_.table)) {
        SDL_Log("input: remap: warning: %s", w.c_str());
    }
    start_ = edit_; // in effect now, whether or not the file is written
    if (!save_bindings(path_, edit_)) {
        say(tr(Txt::remap_not_written,
               {path_.empty() ? std::string(tr(Txt::remap_bindings_file)) : path_}),
            true);
        return;
    }
    saves_++;
    say(tr(Txt::remap_saved_to, {path_}), false);
    open_ = false;
    dirty_ = true;
    SDL_Log("input: the remap overlay closes (saved)");
}

void Remap::tick(double now) {
    if (open_ && capturing_ &&
        static_cast<int>(std::ceil(capture_until_ - now)) !=
            static_cast<int>(std::ceil(capture_until_ - now_))) {
        dirty_ = true; // the countdown
    }
    now_ = now;
    if (open_ && capturing_ && now >= capture_until_) {
        capturing_ = false;
        say(tr(Txt::remap_no_key), false);
    }
}

bool Remap::handle_closed(const SDL_Event &e, double now) {
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F1) {
        open(now);
        return true;
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const int b = e.gbutton.button;
        bool chord = false;
        if (b == SDL_GAMEPAD_BUTTON_LEFT_STICK || b == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(e.gbutton.which);
            const SDL_GamepadButton other = b == SDL_GAMEPAD_BUTTON_LEFT_STICK
                                                ? SDL_GAMEPAD_BUTTON_RIGHT_STICK
                                                : SDL_GAMEPAD_BUTTON_LEFT_STICK;
            chord = pad != nullptr && SDL_GetGamepadButton(pad, other);
        }
        if (settings_button(b) || chord) {
            open(now);
            return true;
        }
    }
    return false;
}

void Remap::track(const SDL_Event &e) {
    const unsigned before = glyphs_.changes();
    glyphs_.handle(e, gamepads_ != nullptr ? gamepads_->active_id() : 0);
    if (open_ && glyphs_.changes() != before) {
        dirty_ = true;
    }
}

bool Remap::handle(const SDL_Event &e, double now) {
    now_ = now;
    if (!open_) {
        return handle_closed(e, now);
    }
    if (is_mouse_or_touch(e.type)) {
        return true; // the game gets no touch while the overlay is up
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = e.key.key;
        if (capturing_) {
            if (e.key.repeat || (e.key.mod & SDL_KMOD_GUI) != 0) {
                return true;
            }
            if (k == SDLK_F1) {
                capturing_ = false;
                say("Nothing changed", false);
            } else {
                capture(Control{ControlKind::kKey, static_cast<int>(k)});
            }
            return true;
        }
        if (k == SDLK_UP) {
            move(-1);
        } else if (k == SDLK_DOWN) {
            move(+1);
        } else if (k == SDLK_PAGEUP) {
            move(-static_cast<int>(kVisibleRows));
        } else if (k == SDLK_PAGEDOWN) {
            move(static_cast<int>(kVisibleRows));
        } else if (e.key.repeat) {
            // Only the cursor repeats.
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
            choose(now);
        } else if ((k == SDLK_LEFT || k == SDLK_RIGHT) && rows_[selected_].kind == RowKind::kFace) {
            choose(now);
        } else if (k == SDLK_ESCAPE || k == SDLK_F1) {
            back();
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const auto b = static_cast<SDL_GamepadButton>(e.gbutton.button);
        if (capturing_) {
            if (settings_button(b)) {
                capturing_ = false;
                say("Nothing changed", false);
            } else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
                       b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                const int d = b - SDL_GAMEPAD_BUTTON_DPAD_UP; // up, down, left, right
                capture(Control{ControlKind::kDirection, d});
            } else {
                capture(gamepad_button_control(type_of(e.gbutton.which), b, edit_.face));
            }
            return true;
        }
        const Control c = gamepad_button_control(type_of(e.gbutton.which), b, edit_.face);
        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
            move(-1);
        } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
            move(+1);
        } else if ((b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) &&
                   rows_[selected_].kind == RowKind::kFace) {
            choose(now);
        } else if (c.code == SDL_GAMEPAD_BUTTON_SOUTH) {
            choose(now);
        } else if (c.code == SDL_GAMEPAD_BUTTON_EAST || settings_button(b)) {
            back();
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const int v = e.gaxis.value;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY && !capturing_) {
            const int dir = v < -16384 ? -1 : v > 16384 ? 1 : 0;
            if (dir != 0 && stick_ == 0) {
                move(dir);
                stick_ = dir;
            } else if (v > -8192 && v < 8192) {
                stick_ = 0;
            }
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                   e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            bool &on = trigger_[e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 1];
            if (!on && v > 16384) {
                on = true;
                if (capturing_) {
                    capture(Control{ControlKind::kGamepadTrigger, e.gaxis.axis});
                }
            } else if (on && v < 13107) {
                on = false;
            }
        }
        return true;
    }
    default:
        return false; // window events, quit, gamepads added or removed
    }
}

void Remap::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    const SDL_GamepadType type = glyphs_.type();
    const FaceButtons face = edit_.face;
    c.fill(0, 0, c.width(), c.height(), kBackdrop);
    c.fill(kPanelX, kPanelY, kPanelW, kPanelH, kPanel);
    c.frame(kPanelX, kPanelY, kPanelW, kPanelH, 1, kBorder);
    c.text(kTitleX, 14, tr(Txt::card_title), kText, 2);
    const std::string pad = glyphs_.pad() != 0 ? tr(Txt::remap_pad, {glyph_set_name(glyphs_.set())})
                                               : std::string(tr(Txt::remap_no_pad));
    c.text(kPadEnd - Canvas::text_width(pad), 18, pad, kDim);
    c.text(kTitleX, 34, tr(Txt::remap_action), kDim);
    c.text(kKeysX, 34, tr(Txt::card_keyboard), kDim);
    c.text(kPadX, 34, tr(Txt::remap_gamepad), kDim);

    for (std::size_t i = top_; i < rows_.size() && i < top_ + kVisibleRows; i++) {
        const Row &row = rows_[i];
        const int y = kListY + static_cast<int>(i - top_) * kRowH;
        if (i == selected_) {
            c.fill(kPanelX + 4, y, kPanelW - 8, kRowH, kSelection);
        }
        const int ty = y + (kRowH - Canvas::kGlyphHeight) / 2;
        const int gy = y + (kRowH - kButtonHeight) / 2;
        switch (row.kind) {
        case RowKind::kHeader:
            c.text(kTitleX - 6, ty, context_title(row.slot.context), kHeader);
            break;
        case RowKind::kAction: {
            c.text(kTitleX, ty, action_title(row.slot.action), kText);
            if (capturing_ && i == selected_) {
                const int left = static_cast<int>(std::ceil(capture_until_ - now_));
                const int shown_left =
                    std::max(0, std::min(left, static_cast<int>(kCaptureSeconds)));
                c.text(kKeysX, ty, tr(Txt::remap_press_countdown, {std::to_string(shown_left)}),
                       kWarning);
                break;
            }
            const std::vector<Control> controls =
                controls_for(edit_.table, row.slot.context, row.slot.action);
            for (const Device d : {Device::kKeyboard, Device::kGamepad}) {
                int x = d == Device::kKeyboard ? kKeysX : kPadX;
                const int end = d == Device::kKeyboard ? kKeysEnd : kPadEnd;
                std::vector<Control> mine;
                for (const Control &ctl : controls) {
                    if (device_of(ctl) == d) {
                        mine.push_back(ctl);
                    }
                }
                std::size_t shown = 0;
                for (; shown < mine.size(); shown++) {
                    const int w = glyph_width(mine[shown], type, face);
                    // Room for "+n" unless it is the last one.
                    if (x + w > end - (shown + 1 < mine.size() ? 18 : 0)) {
                        break;
                    }
                    x += draw_glyph(c, x, gy, mine[shown], type, face) + 3;
                }
                if (mine.empty()) {
                    c.text(x, ty, "-", kDim);
                } else if (shown < mine.size()) {
                    char more[24]; // "+" and up to 20 digits: GCC's -Wformat-truncation at -O3
                    std::snprintf(more, sizeof more, "+%zu", mine.size() - shown);
                    c.text(x, ty, more, kDim);
                }
            }
            break;
        }
        case RowKind::kFace:
            c.text(kTitleX, ty, tr(Txt::remap_nintendo), kText);
            c.text(
                kKeysX, ty,
                tr(face == FaceButtons::kPosition ? Txt::remap_by_position : Txt::remap_by_label),
                kText);
            break;
        case RowKind::kReset:
            c.text(kTitleX, ty, tr(Txt::remap_reset), kText);
            break;
        case RowKind::kSave:
            c.text(kTitleX, ty, tr(Txt::keypad_save), kGood);
            break;
        case RowKind::kCancel:
            c.text(kTitleX, ty, tr(Txt::keypad_cancel), kText);
            break;
        }
    }
    // Scroll marks.
    if (top_ > 0) {
        c.text(kPanelX + kPanelW - 12, kListY, "^", kDim);
    }
    if (top_ + kVisibleRows < rows_.size()) {
        c.text(kPanelX + kPanelW - 12, kListY + (static_cast<int>(kVisibleRows) - 1) * kRowH + 3,
               "v", kDim);
    }
    if (!status_.empty()) {
        std::string s = status_;
        const std::size_t max = static_cast<std::size_t>((kPanelW - 24) / 6);
        if (s.size() > max) {
            s = s.substr(0, max - 3) + "...";
        }
        c.text(kTitleX - 6, kStatusY, s, status_warning_ ? kWarning : kGood);
    }
    // The hints, with the pad's own glyphs.
    int x = kTitleX - 6;
    const int hy = kStatusY + 14;
    const auto key = [&](SDL_Keycode k) {
        x += draw_glyph(c, x, hy, Control{ControlKind::kKey, static_cast<int>(k)}, type, face) + 2;
    };
    const auto button = [&](int b) {
        x += draw_glyph(c, x, hy, Control{ControlKind::kGamepadButton, b}, type, face) + 2;
    };
    const auto words = [&](const char *s) { x += c.text(x, hy + 3, s, kDim) + 8; };
    key(SDLK_UP);
    key(SDLK_DOWN);
    x += draw_glyph(c, x, hy, Control{ControlKind::kDirection, static_cast<int>(Direction::kUp)},
                    type, face) +
         2;
    words(tr(Txt::hint_move));
    key(SDLK_RETURN);
    button(SDL_GAMEPAD_BUTTON_SOUTH);
    words(tr(Txt::hint_change));
    key(SDLK_ESCAPE);
    button(SDL_GAMEPAD_BUTTON_EAST);
    words(tr(Txt::hint_done));
    const std::u8string file =
        std::filesystem::path(std::u8string(path_.begin(), path_.end())).filename().u8string();
    c.text(kTitleX - 6, hy + 16,
           tr(Txt::remap_footer, {file.empty() ? std::string(tr(Txt::remap_no_file))
                                               : std::string(file.begin(), file.end())}),
           kDim);
}

} // namespace maplethief::frontend
