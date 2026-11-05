// The key pad editor (#159, ADR 0086); see keypad_editor.h.
#include "keypad_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "canvas.h"
#include "port_text.h"

namespace maplethief::frontend {

namespace {

constexpr Rgba kBarBack{16, 20, 32, 210};
constexpr Rgba kButtonBack{48, 56, 80, 255};
constexpr Rgba kSaveBack{40, 110, 60, 255};
constexpr Rgba kText{240, 240, 240, 255};
constexpr Rgba kDim{190, 196, 210, 255};
constexpr Rgba kOutline{255, 255, 255, 170};
constexpr Rgba kSelected{255, 214, 64, 255};
constexpr Rgba kLabelBack{0, 0, 0, 170};

constexpr int kButtonGap = 4;
constexpr int kButtonPad = 4; // points each side of a label

constexpr Txt kButtonLabels[KeypadEditor::kButtonCount] = {
    Txt::keypad_size_down, Txt::keypad_size_up,   Txt::keypad_opacity_down, Txt::keypad_opacity_up,
    Txt::keypad_reset,     Txt::keypad_reset_all, Txt::keypad_save,         Txt::keypad_cancel};

int button_width(int i) { return Canvas::text_width(tr(kButtonLabels[i])) + 2 * kButtonPad; }

int bar_width() {
    int w = 0;
    for (int i = 0; i < KeypadEditor::kButtonCount; i++) {
        w += button_width(i) + (i > 0 ? kButtonGap : 0);
    }
    return w;
}

float distance(KeypadPoint a, KeypadPoint b) { return std::hypot(a.x - b.x, a.y - b.y); }

} // namespace

KeypadRect KeypadEditor::button_rect(Button b) {
    int x = (480 - bar_width()) / 2;
    for (int i = 0; i < static_cast<int>(b); i++) {
        x += button_width(i) + kButtonGap;
    }
    return KeypadRect{static_cast<float>(x), static_cast<float>(kBarY),
                      static_cast<float>(button_width(static_cast<int>(b))),
                      static_cast<float>(kBarH)};
}

const char *KeypadEditor::button_label(Button b) { return tr(kButtonLabels[static_cast<int>(b)]); }

void KeypadEditor::open(const KeypadLayout &current) {
    open_ = true;
    saved_ = false;
    layout_ = opened_ = current;
    selected_ = KeypadGroup::kDpad;
    pointers_.clear();
    gesture_ = false;
    dirty_ = true;
    version_++;
}

void KeypadEditor::cancel() { close(false); }

void KeypadEditor::close(bool save) {
    if (!open_) {
        return;
    }
    open_ = false;
    saved_ = save;
    if (!save && !(layout_ == opened_)) {
        layout_ = opened_;
        version_++;
    }
    pointers_.clear();
    gesture_ = false;
    dirty_ = true;
}

void KeypadEditor::select(KeypadGroup g) {
    if (g != selected_) {
        selected_ = g;
        dirty_ = true;
    }
}

void KeypadEditor::change(KeypadGroup g, const GroupLayout &value) {
    GroupLayout v = value;
    v.scale = std::clamp(v.scale, kKeypadMinScale, kKeypadMaxScale);
    v.opacity = std::min<std::uint32_t>(v.opacity, 100);
    v = keypad_clamped(g, v);
    if (!(v == layout_[g])) {
        layout_[g] = v;
        version_++;
        dirty_ = true;
    }
}

void KeypadEditor::resize(KeypadGroup g, int delta) {
    GroupLayout v = layout_[g];
    const int s = static_cast<int>(v.scale) + delta;
    v.scale = static_cast<std::uint32_t>(
        std::clamp(s, static_cast<int>(kKeypadMinScale), static_cast<int>(kKeypadMaxScale)));
    change(g, v);
}

void KeypadEditor::fade(KeypadGroup g, int delta) {
    GroupLayout v = layout_[g];
    v.opacity = static_cast<std::uint32_t>(std::clamp(static_cast<int>(v.opacity) + delta, 0, 100));
    change(g, v);
}

void KeypadEditor::move(KeypadGroup g, int dx, int dy) {
    GroupLayout v = layout_[g];
    v.dx += dx;
    v.dy += dy;
    change(g, v);
}

void KeypadEditor::press(Button b) {
    switch (b) {
    case Button::kSizeDown:
        resize(selected_, -static_cast<int>(kScaleStep));
        break;
    case Button::kSizeUp:
        resize(selected_, static_cast<int>(kScaleStep));
        break;
    case Button::kFadeDown:
        fade(selected_, -static_cast<int>(kOpacityStep));
        break;
    case Button::kFadeUp:
        fade(selected_, static_cast<int>(kOpacityStep));
        break;
    case Button::kReset:
        change(selected_, GroupLayout{});
        break;
    case Button::kResetAll:
        for (const KeypadGroup g : kKeypadGroups) {
            change(g, GroupLayout{});
        }
        break;
    case Button::kSave:
        close(true);
        break;
    case Button::kCancel:
        close(false);
        break;
    }
}

bool KeypadEditor::handle(const SDL_Event &e) {
    if (!open_) {
        return false;
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        const int step = shift ? kMoveStepFast : kMoveStep;
        switch (e.key.key) {
        case SDLK_TAB: {
            const int n = static_cast<int>(kKeypadGroupCount);
            const int i = (static_cast<int>(selected_) + (shift ? n - 1 : 1)) % n;
            select(kKeypadGroups[i]);
            break;
        }
        case SDLK_LEFT:
            move(selected_, -step, 0);
            break;
        case SDLK_RIGHT:
            move(selected_, step, 0);
            break;
        case SDLK_UP:
            move(selected_, 0, -step);
            break;
        case SDLK_DOWN:
            move(selected_, 0, step);
            break;
        case SDLK_EQUALS:
        case SDLK_PLUS:
        case SDLK_KP_PLUS:
            press(Button::kSizeUp);
            break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
            press(Button::kSizeDown);
            break;
        case SDLK_LEFTBRACKET:
            press(Button::kFadeDown);
            break;
        case SDLK_RIGHTBRACKET:
            press(Button::kFadeUp);
            break;
        case SDLK_R:
            press(shift ? Button::kResetAll : Button::kReset);
            break;
        case SDLK_BACKSPACE:
        case SDLK_DELETE:
            press(Button::kResetAll);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            press(Button::kSave);
            break;
        case SDLK_ESCAPE:
            press(Button::kCancel);
            break;
        default:
            break;
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            move(selected_, -kPadStep, 0);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            move(selected_, kPadStep, 0);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            move(selected_, 0, -kPadStep);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            move(selected_, 0, kPadStep);
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: {
            const int n = static_cast<int>(kKeypadGroupCount);
            const bool back = e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
            select(kKeypadGroups[(static_cast<int>(selected_) + (back ? n - 1 : 1)) % n]);
            break;
        }
        case SDL_GAMEPAD_BUTTON_WEST:
            press(Button::kSizeDown);
            break;
        case SDL_GAMEPAD_BUTTON_NORTH:
            press(Button::kSizeUp);
            break;
        case SDL_GAMEPAD_BUTTON_BACK:
            press(Button::kReset);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_START:
            press(Button::kSave);
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            press(Button::kCancel);
            break;
        default:
            break;
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        wheel(e.wheel.y);
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        return false;
    }
}

void KeypadEditor::start_gesture() {
    gesture_ = !pointers_.empty();
    gesture_from_ = layout_[gesture_group_];
    for (Pointer &p : pointers_) {
        p.start = p.now;
    }
}

void KeypadEditor::pointer(std::uint64_t id, Phase phase, KeypadPoint p) {
    if (!open_) {
        return;
    }
    last_pointer_ = p;
    auto it = std::find_if(pointers_.begin(), pointers_.end(),
                           [id](const Pointer &q) { return q.id == id; });
    switch (phase) {
    case Phase::kDown: {
        if (it != pointers_.end()) {
            return; // a second down without an up
        }
        for (int i = 0; i < kButtonCount; i++) {
            if (pointers_.empty() && button_rect(static_cast<Button>(i)).contains(p)) {
                press(static_cast<Button>(i));
                return;
            }
        }
        KeypadGroup g{};
        if (pointers_.empty()) {
            if (!keypad_group_at(layout_, p, &g)) {
                return;
            }
            select(g);
            gesture_group_ = g;
        } else if (pointers_.size() >= 2) {
            return;
        }
        pointers_.push_back(Pointer{id, p, p});
        start_gesture();
        return;
    }
    case Phase::kMove: {
        if (it == pointers_.end() || !gesture_) {
            return;
        }
        it->now = p;
        GroupLayout v = gesture_from_;
        const Pointer &a = pointers_[0];
        if (pointers_.size() == 1) {
            v.dx += static_cast<std::int32_t>(std::lround(a.now.x - a.start.x));
            v.dy += static_cast<std::int32_t>(std::lround(a.now.y - a.start.y));
        } else {
            const Pointer &b = pointers_[1];
            const float d0 = distance(a.start, b.start), d1 = distance(a.now, b.now);
            if (d0 >= 4) {
                const float s = static_cast<float>(gesture_from_.scale) * d1 / d0;
                v.scale = static_cast<std::uint32_t>(std::lround(std::clamp(
                    s, static_cast<float>(kKeypadMinScale), static_cast<float>(kKeypadMaxScale))));
            }
            v.dx += static_cast<std::int32_t>(
                std::lround((a.now.x + b.now.x - a.start.x - b.start.x) / 2));
            v.dy += static_cast<std::int32_t>(
                std::lround((a.now.y + b.now.y - a.start.y - b.start.y) / 2));
        }
        change(gesture_group_, v);
        return;
    }
    case Phase::kUp:
        if (it == pointers_.end()) {
            return;
        }
        pointers_.erase(it);
        start_gesture(); // the finger left goes on dragging from here
        return;
    }
}

void KeypadEditor::wheel(float steps) {
    if (!open_ || steps == 0) {
        return;
    }
    KeypadGroup g = selected_;
    if (keypad_group_at(layout_, last_pointer_, &g)) {
        select(g);
    }
    resize(g, steps > 0 ? static_cast<int>(kScaleStep) : -static_cast<int>(kScaleStep));
}

std::string KeypadEditor::status() const {
    const GroupLayout &g = layout_[selected_];
    return tr(Txt::keypad_status,
              {keypad_group_title(selected_), std::to_string(static_cast<unsigned>(g.scale)),
               std::to_string(static_cast<unsigned>(g.opacity)),
               std::to_string(static_cast<int>(g.dx)), std::to_string(static_cast<int>(g.dy))});
}

void KeypadEditor::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    // The groups: an outline where each is drawn, the selected one thicker, with its name.
    for (const KeypadGroup g : kKeypadGroups) {
        const KeypadRect r = keypad_drawn_rect(g, layout_[g]);
        const int x = static_cast<int>(std::floor(r.x)), y = static_cast<int>(std::floor(r.y));
        const int w = static_cast<int>(std::ceil(r.x + r.w)) - x;
        const int h = static_cast<int>(std::ceil(r.y + r.h)) - y;
        const bool sel = g == selected_;
        c.frame(x, y, w, h, sel ? 2 : 1, sel ? kSelected : kOutline);
        const char *name = keypad_group_title(g);
        const int tw = Canvas::text_width(name);
        const int ty = y >= 12 ? y - 11 : y + h + 2;
        const int tx = std::clamp(x, 0, 480 - tw - 4);
        c.fill(tx, ty, tw + 4, 10, kLabelBack);
        c.text(tx + 2, ty + 1, name, sel ? kSelected : kText);
    }
    // The bar, the status and the hint.
    const KeypadRect first = button_rect(Button::kSizeDown);
    const int bx = static_cast<int>(first.x) - 4;
    c.fill(bx, 0, bar_width() + 8, kBarY + kBarH + 26, kBarBack);
    for (int i = 0; i < kButtonCount; i++) {
        const Button b = static_cast<Button>(i);
        const KeypadRect r = button_rect(b);
        c.fill(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.w),
               static_cast<int>(r.h), b == Button::kSave ? kSaveBack : kButtonBack);
        c.text(static_cast<int>(r.x) + kButtonPad, static_cast<int>(r.y) + 6, tr(kButtonLabels[i]),
               kText);
    }
    c.text(bx + 4, kBarY + kBarH + 3, status(), kSelected);
    c.text(bx + 4, kBarY + kBarH + 14, tr(Txt::keypad_hint), kDim);
}

} // namespace maplethief::frontend
