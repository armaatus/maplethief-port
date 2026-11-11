// The first-launch preset picker (#144, ADR 0070); see preset_picker.h.
#include "preset_picker.h"

#include <cstring>
#include <string>

#include "canvas.h"
#include "desktop.h"
#include "gamepad.h"
#include "port_text.h"
#include "present.h"

namespace maplethief::frontend {

namespace {

constexpr Rgba kBackground = {0x0B, 0x0E, 0x14, 255};
constexpr Rgba kRowFrame = {0x2A, 0x33, 0x42, 255};
constexpr Rgba kBorder = {0x5A, 0x6B, 0x85, 255};
constexpr Rgba kSelection = {0x2E, 0x4A, 0x7A, 255};
constexpr Rgba kHeader = {0xF2, 0xB8, 0x4B, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};

// The picker's screen: 480 x 320 points at 2 pixels each.
constexpr int kPickerScale = 2;

const char *line_title(Preset p) {
    return p == Preset::kCustom ? tr(Txt::picker_keep) : preset_title(p);
}

bool is_trackpad(SDL_TouchID id) {
    const SDL_TouchDeviceType type = SDL_GetTouchDeviceType(id);
    return type == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE || type == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE;
}

} // namespace

bool wants_preset_picker(bool game, const Settings &settings, const GameOptions &options,
                         const char *video_driver) {
    // The interactive-run rule is desktop.h's (window.ini follows it too, #145).
    return game && !settings.preset && is_interactive_run(options, video_driver);
}

PresetPicker::PresetPicker(bool keep, bool offer, bool ask_updates)
    : offer_(offer), ask_updates_(ask_updates) {
    if (keep) {
        lines_.push_back(Preset::kCustom);
    }
    for (const Preset p : {Preset::kOriginal, Preset::kOriginalController, Preset::kEnhanced}) {
        lines_.push_back(p);
    }
}

PresetPicker::Step PresetPicker::step() const {
    if (!chosen_) {
        return Step::kPreset;
    }
    if (ask_updates_ && *chosen_ != Preset::kOriginal && !update_check_) {
        return Step::kUpdates;
    }
    return offer_ && !show_controls_ ? Step::kControls : Step::kDone;
}

void PresetPicker::move(int delta) {
    const long n = static_cast<long>(count());
    selected_ = static_cast<std::size_t>(((static_cast<long>(selected_) + delta) % n + n) % n);
    dirty_ = true;
}

void PresetPicker::choose(std::size_t line) {
    if (line >= count() || done()) {
        return;
    }
    if (step() == Step::kPreset) {
        chosen_ = lines_[line];
    } else if (step() == Step::kUpdates) { // #199: line 0 "No", line 1 "Yes"
        update_check_ = line == 1;
    } else { // the controls step (#193): line 0 "Show controls", line 1 "Start playing"
        show_controls_ = line == 0;
    }
    selected_ = done() ? line : 0; // a new step starts on its first line
    pressed_ = -1;
    dirty_ = true;
}

int PresetPicker::line_at(float x, float y) const {
    if (x < kListX || x >= kListX + kListW || y < kListY) {
        return -1;
    }
    const int i = static_cast<int>((y - kListY) / kRowH);
    // The gap below each line's box is no line.
    if (i >= static_cast<int>(count()) ||
        y - kListY - static_cast<float>(i * kRowH) >= static_cast<float>(kRowH - 6)) {
        return -1;
    }
    return i;
}

void PresetPicker::pointer(const ScreenPoint &p, bool down, bool up) {
    const int line = p.inside ? line_at(p.x, p.y) : -1;
    if (down) {
        pressed_ = line;
    }
    if (line >= 0 && static_cast<std::size_t>(line) != selected_) {
        selected_ = static_cast<std::size_t>(line);
        dirty_ = true;
    }
    if (up) {
        if (line >= 0 && line == pressed_) {
            choose(static_cast<std::size_t>(line));
        }
        pressed_ = -1;
    }
}

void PresetPicker::track(const SDL_Event &e) {
    const unsigned before = glyphs_.changes();
    glyphs_.handle(e, 0);
    devices_.handle(e);
    dirty_ = dirty_ || glyphs_.changes() != before;
}

bool PresetPicker::handle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_UP || k == SDLK_W) {
            move(-1);
        } else if (k == SDLK_DOWN || k == SDLK_S) {
            move(+1);
        } else if (e.key.repeat) {
            // Only the cursor repeats.
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
            choose(selected_);
        } else if (k >= SDLK_1 && k <= SDLK_9) {
            choose(static_cast<std::size_t>(k - SDLK_1));
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const int b = e.gbutton.button;
        // A on the Switch (by label, #239); the bottom button elsewhere.
        const int face =
            screen_button_control(e.gbutton.which, static_cast<SDL_GamepadButton>(b)).code;
        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
            move(-1);
        } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
            move(+1);
        } else if (face == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_START) {
            choose(selected_);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            const int v = e.gaxis.value;
            const int dir = v < -16384 ? -1 : v > 16384 ? 1 : 0;
            if (dir != 0 && stick_ == 0) {
                move(dir);
                stick_ = dir;
            } else if (v > -8192 && v < 8192) {
                stick_ = 0;
            }
        }
        return true;
    default:
        return false;
    }
}

void PresetPicker::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    const SDL_GamepadType type = glyphs_.type();
    const FaceButtons face = kFaceButtonsDefault;
    c.fill(0, 0, c.width(), c.height(), kBackground);
    const Step now = step();
    const bool step2 = now == Step::kControls || (now == Step::kDone && offer_);
    const bool updates = now == Step::kUpdates;
    if (step2) {
        c.text(kListX, 16, tr(Txt::picker_ready), kText, 2);
        c.text(kListX, 40, tr(Txt::picker_is_set, {line_title(*chosen_)}), kDim);
    } else if (updates) {
        c.text(kListX, 16, tr(Txt::picker_updates_title), kText, 2);
        c.text(kListX, 40, tr(Txt::picker_updates_help), kDim);
    } else {
        c.text(kListX, 16, tr(Txt::picker_title), kText, 2);
        c.text(kListX, 40, tr(Txt::picker_help), kDim);
    }
    for (std::size_t i = 0; i < count(); i++) {
        const int y = kListY + static_cast<int>(i) * kRowH;
        const bool on = i == selected_;
        if (on) {
            c.fill(kListX, y, kListW, kRowH - 6, kSelection);
            c.frame(kListX, y, kListW, kRowH - 6, 1, kBorder);
        } else {
            c.frame(kListX, y, kListW, kRowH - 6, 1, kRowFrame);
        }
        const std::string number = std::to_string(i + 1);
        c.text(kListX + 8, y + 7, number, on ? kHeader : kDim, 2);
        const char *const title =
            updates ? tr(i == 0 ? Txt::picker_updates_no : Txt::picker_updates_yes)
            : step2 ? tr(i == 0 ? Txt::picker_show_controls : Txt::picker_start)
                    : line_title(lines_[i]);
        const char *const help =
            updates ? tr(i == 0 ? Txt::picker_updates_no_help : Txt::picker_updates_yes_help)
            : step2 ? tr(i == 0 ? Txt::picker_show_controls_help : Txt::picker_start_help)
                    : preset_summary(lines_[i]);
        c.text(kListX + 30, y + 7, title, kText, 2);
        c.text(kListX + 30, y + 26, help, on ? kText : kDim);
    }
    // The hints, with the pad's own glyphs.
    int x = kListX;
    const int hy = 290;
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
    words(tr(Txt::picker_choose));
    words(tr(Txt::picker_click_too));
}

std::optional<Preset> run_preset_picker(SDL_Window *window, SDL_Renderer *renderer,
                                        const Settings &settings, bool keep, bool *quit,
                                        ControlsOffer *offer, std::optional<bool> *update_check) {
    *quit = false;
    if (offer != nullptr) {
        *offer = ControlsOffer{};
    }
    constexpr std::uint32_t w = kScreenWidth * kPickerScale, h = kScreenHeight * kPickerScale;
    Presenter presenter;
    if (!presenter.start(renderer, w, h)) {
        SDL_Log("presets: the picker cannot be shown: the game starts without a preset");
        return std::nullopt;
    }
    presenter.set_scaling(settings.scale_mode, settings.border_color);
    PresetPicker picker(keep, offer != nullptr, update_check != nullptr);
    std::vector<SDL_Gamepad *> pads;
    const auto open_pad = [&pads](SDL_JoystickID id) {
        if (SDL_Gamepad *pad = SDL_OpenGamepad(id); pad != nullptr) {
            pads.push_back(pad);
        }
    };
    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count); ids != nullptr) {
        for (int i = 0; i < count; i++) {
            open_pad(ids[i]);
        }
        SDL_free(ids);
    }
    SDL_Log("presets: no preset chosen yet: the first-launch picker (%s)",
            keep ? "an older settings.ini: Keep my settings first" : "no settings.ini");
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    const auto to_screen = [&](float wx, float wy) {
        return window_point_to_screen(window, presenter.placement(), wx, wy);
    };
    while (!picker.done() && !*quit) {
        if (picker.dirty()) {
            picker.draw(px.data(), w, h, kPickerScale);
            presenter.upload(px.data(), w * 4);
        }
        presenter.draw();
        SDL_RenderPresent(renderer);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) {
            continue;
        }
        do {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_TERMINATING:
                *quit = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                open_pad(e.gdevice.which);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (e.motion.which != SDL_TOUCH_MOUSEID && e.motion.which != SDL_PEN_MOUSEID) {
                    picker.pointer(to_screen(e.motion.x, e.motion.y), false, false);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT && e.button.which != SDL_TOUCH_MOUSEID &&
                    e.button.which != SDL_PEN_MOUSEID) {
                    picker.pointer(to_screen(e.button.x, e.button.y),
                                   e.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                                   e.type == SDL_EVENT_MOUSE_BUTTON_UP);
                }
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION:
                if (e.tfinger.touchID != SDL_MOUSE_TOUCHID && !is_trackpad(e.tfinger.touchID)) {
                    int ww = 0, wh = 0;
                    SDL_GetWindowSize(window, &ww, &wh);
                    picker.pointer(to_screen(e.tfinger.x * static_cast<float>(ww),
                                             e.tfinger.y * static_cast<float>(wh)),
                                   e.type == SDL_EVENT_FINGER_DOWN, e.type == SDL_EVENT_FINGER_UP);
                }
                break;
            default:
                picker.handle(e);
                break;
            }
            picker.track(e);
        } while (!picker.done() && !*quit && SDL_PollEvent(&e));
    }
    for (SDL_Gamepad *pad : pads) {
        SDL_CloseGamepad(pad);
    }
    presenter.stop();
    if (*quit) {
        SDL_Log("presets: the window closed in the picker: nothing written");
        return std::nullopt;
    }
    SDL_Log("presets: %s chosen", line_title(*picker.chosen()));
    if (update_check != nullptr && picker.update_check()) {
        *update_check = picker.update_check();
        SDL_Log("presets: update check %s", **update_check ? "on" : "off");
    }
    if (offer != nullptr) {
        *offer = ControlsOffer{picker.show_controls().value_or(false), picker.device()};
        SDL_Log("presets: %s (%s)",
                offer->show ? "the controls card opens once the game is up" : "no controls card",
                prompt_device_name(offer->device));
    }
    return picker.chosen();
}

} // namespace maplethief::frontend
