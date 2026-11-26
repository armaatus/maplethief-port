// #198's start-up question in the window (#416); see start_question.h.
#include "start_question.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "canvas.h"
#include "data_notice.h"
#include "gamepad.h"
#include "port_text.h"
#include "present.h"

namespace maplethief::frontend {

namespace {

// The preset picker's colours (preset_picker.cpp).
constexpr Rgba kBackground = {0x0B, 0x0E, 0x14, 255};
constexpr Rgba kRowFrame = {0x2A, 0x33, 0x42, 255};
constexpr Rgba kBorder = {0x5A, 0x6B, 0x85, 255};
constexpr Rgba kSelection = {0x2E, 0x4A, 0x7A, 255};
constexpr Rgba kHeader = {0xF2, 0xB8, 0x4B, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};

// The screen: 480 x 320 points at 2 pixels each.
constexpr int kQuestionScale = 2;

bool is_trackpad(SDL_TouchID id) {
    const SDL_TouchDeviceType type = SDL_GetTouchDeviceType(id);
    return type == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE || type == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE;
}

// `text`'s lines at kColumns: "\n" ends a line, "\n\n" leaves a gap ("").
std::vector<std::string> question_lines(const std::string &text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        if (line.empty()) {
            out.emplace_back();
        } else {
            for (std::string &part : wrap_notice(line, StartQuestion::kColumns)) {
                out.push_back(std::move(part));
            }
        }
        at = end + 1;
    }
    return out;
}

} // namespace

StartQuestion::StartQuestion(const std::string &text, std::vector<std::string> buttons)
    : buttons_(std::move(buttons)), lines_(question_lines(text)) {}

void StartQuestion::move(int delta) {
    const long n = static_cast<long>(buttons_.size());
    selected_ = static_cast<std::size_t>(((static_cast<long>(selected_) + delta) % n + n) % n);
    dirty_ = true;
}

void StartQuestion::choose(std::size_t button) {
    if (button >= buttons_.size() || done()) {
        return;
    }
    chosen_ = button;
    selected_ = button;
    pressed_ = -1;
    dirty_ = true;
}

int StartQuestion::button_at(float x, float y) const {
    const int top = list_y();
    if (x < kListX || x >= kListX + kListW || y < static_cast<float>(top)) {
        return -1;
    }
    const int i = static_cast<int>((y - static_cast<float>(top)) / kRowH);
    // The gap below each button's box is no button.
    if (i >= static_cast<int>(buttons_.size()) ||
        y - static_cast<float>(top + i * kRowH) >= static_cast<float>(kRowH - 4)) {
        return -1;
    }
    return i;
}

void StartQuestion::pointer(const ScreenPoint &p, bool down, bool up) {
    const int button = p.inside ? button_at(p.x, p.y) : -1;
    if (down) {
        pressed_ = button;
    }
    if (button >= 0 && static_cast<std::size_t>(button) != selected_) {
        selected_ = static_cast<std::size_t>(button);
        dirty_ = true;
    }
    if (up) {
        if (button >= 0 && button == pressed_) {
            choose(static_cast<std::size_t>(button));
        }
        pressed_ = -1;
    }
}

void StartQuestion::track(const SDL_Event &e) {
    const unsigned before = glyphs_.changes();
    glyphs_.handle(e, 0);
    dirty_ = dirty_ || glyphs_.changes() != before;
}

bool StartQuestion::handle(const SDL_Event &e) {
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
        } else if (k == SDLK_ESCAPE) {
            choose(buttons_.size() - 1);
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
        // A and B on the Switch (by label, #239); the bottom and right buttons elsewhere.
        const int face =
            screen_button_control(e.gbutton.which, static_cast<SDL_GamepadButton>(b)).code;
        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) {
            move(-1);
        } else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
            move(+1);
        } else if (face == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_START) {
            choose(selected_);
        } else if (face == SDL_GAMEPAD_BUTTON_EAST) {
            choose(buttons_.size() - 1);
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

void StartQuestion::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    const SDL_GamepadType type = glyphs_.type();
    const FaceButtons face = kFaceButtonsDefault;
    c.fill(0, 0, c.width(), c.height(), kBackground);
    // The text: its first line (the box's first sentence) as the title. Lines that would reach
    // the buttons are left out.
    const int top = list_y();
    int y = kTextY;
    for (std::size_t i = 0; i < lines_.size(); i++) {
        if (lines_[i].empty()) {
            y += kParagraphGap;
            continue;
        }
        if (y + kLineH > top - 4) {
            break;
        }
        c.text(kTextX, y, lines_[i], i == 0 ? kHeader : kText);
        y += kLineH;
    }
    for (std::size_t i = 0; i < buttons_.size(); i++) {
        const int by = top + static_cast<int>(i) * kRowH;
        const bool on = i == selected_;
        if (on) {
            c.fill(kListX, by, kListW, kRowH - 4, kSelection);
            c.frame(kListX, by, kListW, kRowH - 4, 1, kBorder);
        } else {
            c.frame(kListX, by, kListW, kRowH - 4, 1, kRowFrame);
        }
        c.text(kListX + 8, by + 6, std::to_string(i + 1), on ? kHeader : kDim, 2);
        c.text(kListX + 30, by + 6, buttons_[i], kText, 2);
    }
    // The hints, with the pad's own glyphs.
    int x = kListX;
    const auto key = [&](SDL_Keycode k) {
        if (!shows_key_hints()) {
            return; // no keyboard of its own: the buttons alone (#282)
        }
        x += draw_glyph(c, x, kHintY, Control{ControlKind::kKey, static_cast<int>(k)}, type, face) +
             2;
    };
    const auto button = [&](int b) {
        x += draw_glyph(c, x, kHintY, Control{ControlKind::kGamepadButton, b}, type, face) + 2;
    };
    const auto words = [&](std::string_view s) { x += c.text(x, kHintY + 3, s, kDim) + 8; };
    key(SDLK_UP);
    key(SDLK_DOWN);
    x +=
        draw_glyph(c, x, kHintY, Control{ControlKind::kDirection, static_cast<int>(Direction::kUp)},
                   type, face) +
        2;
    words(tr(Txt::hint_move));
    key(SDLK_RETURN);
    button(SDL_GAMEPAD_BUTTON_SOUTH);
    words(tr(Txt::picker_choose));
    key(SDLK_ESCAPE);
    button(SDL_GAMEPAD_BUTTON_EAST);
    words(buttons_.back());
}

std::optional<std::size_t> run_start_question(SDL_Window *window, SDL_Renderer *renderer,
                                              const Settings &settings, StartQuestion *question,
                                              bool *quit) {
    *quit = false;
    constexpr std::uint32_t w = kScreenWidth * kQuestionScale, h = kScreenHeight * kQuestionScale;
    Presenter presenter;
    if (!presenter.start(renderer, w, h)) {
        SDL_Log("safe start: the question cannot be shown in the window");
        return std::nullopt;
    }
    presenter.set_scaling(settings.scale_mode, settings.border_color);
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
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    const auto to_screen = [&](float wx, float wy) {
        return window_point_to_screen(window, presenter.placement(), wx, wy);
    };
    while (!question->done() && !*quit) {
        if (question->dirty()) {
            question->draw(px.data(), w, h, kQuestionScale);
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
                    question->pointer(to_screen(e.motion.x, e.motion.y), false, false);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT && e.button.which != SDL_TOUCH_MOUSEID &&
                    e.button.which != SDL_PEN_MOUSEID) {
                    question->pointer(to_screen(e.button.x, e.button.y),
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
                    question->pointer(to_screen(e.tfinger.x * static_cast<float>(ww),
                                                e.tfinger.y * static_cast<float>(wh)),
                                      e.type == SDL_EVENT_FINGER_DOWN,
                                      e.type == SDL_EVENT_FINGER_UP);
                }
                break;
            default:
                question->handle(e);
                break;
            }
            question->track(e);
        } while (!question->done() && !*quit && SDL_PollEvent(&e));
    }
    for (SDL_Gamepad *pad : pads) {
        SDL_CloseGamepad(pad);
    }
    presenter.stop();
    if (*quit) {
        SDL_Log("safe start: the window closed in the question");
        return std::nullopt;
    }
    const std::optional<std::size_t> chosen = question->chosen();
    if (chosen) {
        SDL_Log("safe start: asked in the window: %s", question->buttons()[*chosen].c_str());
    }
    return chosen;
}

StartPrompts window_start_prompts(SDL_Window *window, SDL_Renderer *renderer,
                                  const Settings &settings, bool *quit) {
    *quit = false;
    StartPrompts p;
    p.safe_start = [=](const std::string &crash_file) {
        StartQuestion q(safe_start_question_text(crash_file),
                        {start_choice_label(SafeStartChoice::kDefaults),
                         start_choice_label(SafeStartChoice::kNormal)});
        const std::optional<std::size_t> i =
            run_start_question(window, renderer, settings, &q, quit);
        return i == std::size_t{0} ? SafeStartChoice::kDefaults : SafeStartChoice::kNormal;
    };
    p.recovery = [=](const std::string &crash_file, const RecoveryOffer &offer) {
        constexpr RecoveryChoice kOrder[] = {
            RecoveryChoice::kResume, RecoveryChoice::kResumeDefaults,
            RecoveryChoice::kStartDefaults, RecoveryChoice::kStartNormal};
        std::vector<std::string> buttons;
        for (const RecoveryChoice choice : kOrder) {
            buttons.emplace_back(start_choice_label(choice));
        }
        StartQuestion q(recovery_question_text(crash_file, offer), std::move(buttons));
        const std::optional<std::size_t> i =
            run_start_question(window, renderer, settings, &q, quit);
        return i ? kOrder[*i] : RecoveryChoice::kStartNormal;
    };
    return p;
}

} // namespace maplethief::frontend
