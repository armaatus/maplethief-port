// The gamepad pointer (#185, ADR 0107); see pad_pointer.h.
#include "pad_pointer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "maplethief/uikit.h"

namespace maplethief::frontend {

namespace {

constexpr float kHeight = static_cast<float>(kScreenHeight);

// SDL's axis value as -1..1.
float axis_value(Sint16 v) {
    return v < 0 ? static_cast<float>(v) / 32768.0f : static_cast<float>(v) / 32767.0f;
}

float below(float edge) { return std::nextafter(edge, -1e30f); }

} // namespace

PointerBounds pointer_bounds(std::uint32_t view_width) {
    PointerBounds b;
    if (view_width > kScreenWidth) {
        const float wing = static_cast<float>(view_width - kScreenWidth) / 2.0f;
        b.x0 = -wing;
        b.x1 = static_cast<float>(kScreenWidth) + wing;
    }
    return b;
}

PointerVelocity pointer_velocity(float x, float y, double pushed, const PointerConfig &config) {
    const float r = std::sqrt(x * x + y * y);
    if (!std::isfinite(r) || r <= config.deadzone || config.deadzone >= 1.0f) {
        return {};
    }
    const float past = (std::min(r, 1.0f) - config.deadzone) / (1.0f - config.deadzone);
    const float curve = std::pow(past, config.exponent);
    const float t = config.ramp_seconds > 0
                        ? static_cast<float>(std::clamp(pushed / config.ramp_seconds, 0.0, 1.0))
                        : 1.0f;
    const float speed = config.speed * curve * (1.0f + (config.boost - 1.0f) * t);
    return PointerVelocity{x / r * speed, y / r * speed};
}

void pointer_step(float *x, float *y, PointerVelocity v, double dt, const PointerBounds &b) {
    const auto d = static_cast<float>(std::max(dt, 0.0));
    *x = std::clamp(*x + v.x * d, b.x0, below(b.x1));
    *y = std::clamp(*y + v.y * d, 0.0f, below(kHeight));
}

void touchpad_point(float nx, float ny, const PointerBounds &b, float *x, float *y) {
    nx = std::isfinite(nx) ? std::clamp(nx, 0.0f, 1.0f) : 0.5f;
    ny = std::isfinite(ny) ? std::clamp(ny, 0.0f, 1.0f) : 0.5f;
    *x = std::min(b.x0 + nx * (b.x1 - b.x0), below(b.x1));
    *y = std::min(ny * kHeight, below(kHeight));
}

PadPointer::PadPointer(Input *input, Actions *actions) : input_(input), actions_(actions) {}

bool PadPointer::hold_gate(void *self) {
    const auto *p = static_cast<const PadPointer *>(self);
    return p->shown_ || p->pressing_;
}

void PadPointer::set_mode(PadPointerMode mode) {
    if (mode == mode_) {
        return;
    }
    release_all();
    mode_ = mode;
    SDL_Log("input: pad pointer %s", pad_pointer_name(mode));
}

void PadPointer::hide(const char *why) {
    if (shown_) {
        shown_ = false;
        stats_.hides++;
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "input: the pad pointer hides (%s)", why);
    }
}

void PadPointer::end_press() {
    if (pressing_) {
        pressing_ = false;
        if (input_ != nullptr) {
            input_->touch(InputSource::kPointer, kPointerFinger, MT_UIKIT_TOUCH_ENDED, x_, y_);
        }
    }
}

void PadPointer::release_all() {
    end_press();
    for (const PadFinger &f : fingers_) {
        if (input_ != nullptr) {
            input_->touch(InputSource::kPointer, f.id, MT_UIKIT_TOUCH_ENDED, x_, y_);
        }
    }
    fingers_.clear();
    sx_ = sy_ = 0;
    pushed_ = 0;
    hide("released");
}

bool PadPointer::touchpad(const SDL_Event &e) {
    const SDL_GamepadTouchpadEvent &t = e.gtouchpad;
    auto it = std::find_if(fingers_.begin(), fingers_.end(), [&](const PadFinger &f) {
        return f.pad == t.which && f.touchpad == t.touchpad && f.finger == t.finger;
    });
    float x = 0, y = 0;
    touchpad_point(t.x, t.y, bounds_, &x, &y);
    if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
        if (!touchpad_on()) {
            return false;
        }
        const std::uint32_t id =
            kTouchpadFinger + static_cast<std::uint32_t>(t.touchpad * 8 + t.finger) % 16u;
        if (it == fingers_.end()) {
            fingers_.push_back(PadFinger{t.which, t.touchpad, t.finger, id});
        }
        // The screen is the pad now: the stick's pointer goes (its tap with it).
        end_press();
        hide("a touchpad finger");
        if (input_ != nullptr &&
            input_->touch(InputSource::kPointer, id, MT_UIKIT_TOUCH_BEGAN, x, y)) {
            stats_.pad_touches++;
        }
        return true;
    }
    if (it == fingers_.end()) {
        return false; // a finger that went down before the mode allowed it, or another's
    }
    const std::uint32_t id = it->id;
    const bool up = e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP;
    if (up) {
        fingers_.erase(it);
    }
    if (input_ != nullptr) {
        input_->touch(InputSource::kPointer, id, up ? MT_UIKIT_TOUCH_ENDED : MT_UIKIT_TOUCH_MOVED,
                      x, y);
    }
    return true;
}

bool PadPointer::handle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX) {
            sx_ = stick_on() ? axis_value(e.gaxis.value) : 0.0f;
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY) {
            sy_ = stick_on() ? axis_value(e.gaxis.value) : 0.0f;
        }
        return false; // Gamepads keeps the axes too (the triggers, the left stick)
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        return touchpad(e);
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const SDL_JoystickID gone = e.gdevice.which;
        for (const PadFinger &f : fingers_) {
            if (f.pad == gone && input_ != nullptr) {
                input_->touch(InputSource::kPointer, f.id, MT_UIKIT_TOUCH_ENDED, x_, y_);
            }
        }
        fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(),
                                      [gone](const PadFinger &f) { return f.pad == gone; }),
                       fingers_.end());
        sx_ = sy_ = 0;
        return false;
    }
    default:
        return false;
    }
}

void PadPointer::turn(double now, std::uint64_t focus_uses) {
    const double dt = last_ < 0 ? 0.0 : std::clamp(now - last_, 0.0, 0.1);
    last_ = now;
    // The focus used (a d-pad, OK or back on its screens): it takes over from the pointer.
    if (!uses_known_ || focus_uses != uses_) {
        if (uses_known_) {
            dropped_ = false;
            if (!pressing_) {
                hide("the focus moved");
            }
        }
        uses_ = focus_uses;
        uses_known_ = true;
    }
    if (!stick_on()) {
        end_press();
        hide("off");
        return;
    }
    const PointerVelocity v = pointer_velocity(sx_, sy_, pushed_, config_);
    const bool pushed = v.x != 0 || v.y != 0;
    pushed_ = pushed ? pushed_ + dt : 0.0;
    const float ox = x_, oy = y_;
    if (pushed) {
        if (!shown_) {
            shown_ = true;
            stats_.shows++;
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "input: the pad pointer shows at (%g, %g)",
                         static_cast<double>(x_), static_cast<double>(y_));
        }
        pointer_step(&x_, &y_, v, dt, bounds_);
        active_ = now;
    } else {
        // Kept inside a picture that narrowed (widescreen leaving the field).
        pointer_step(&x_, &y_, PointerVelocity{}, 0, bounds_);
    }
    // Its touch: while pointer_tap is held and the pointer is shown.
    const bool want = actions_ != nullptr && actions_->holding(MT_ACTION_POINTER_TAP);
    if (want && !pressing_ && shown_) {
        if (input_ != nullptr &&
            input_->touch(InputSource::kPointer, kPointerFinger, MT_UIKIT_TOUCH_BEGAN, x_, y_)) {
            pressing_ = true;
            dropped_ = true;
            stats_.taps++;
            SDL_Log("input: pad pointer tap at (%g, %g)", static_cast<double>(x_),
                    static_cast<double>(y_));
        }
    } else if (pressing_ && !want) {
        end_press();
    } else if (pressing_ && (x_ != ox || y_ != oy) && input_ != nullptr) {
        input_->touch(InputSource::kPointer, kPointerFinger, MT_UIKIT_TOUCH_MOVED, x_, y_);
    }
    if (pressing_) {
        active_ = now;
    }
    if (shown_ && now - active_ >= kIdleSeconds) {
        hide("idle");
    }
}

void draw_pad_pointer(SDL_Renderer *renderer, const Placement &placement, float x, float y,
                      bool pressed) {
    if (renderer == nullptr || placement.rect.w <= 0 || placement.rect.h <= 0) {
        return;
    }
    float px = 0, py = 0;
    screen_to_drawable(placement, x, y, &px, &py);
    // An arrow in units of 1/16 of its 12-point height: a head and a tail.
    const float u = 12.0f / 16.0f * static_cast<float>(placement.rect.h) / kHeight;
    static constexpr float kHead[] = {0, 0, 0, 15, 11, 11};
    static constexpr float kTail[] = {4, 11, 7, 10, 10, 16, 4, 11, 10, 16, 7.5f, 17.5f};
    const auto draw = [&](float dx, float dy, SDL_FColor color) {
        SDL_Vertex v[9];
        int n = 0;
        for (int i = 0; i < 6; i += 2) {
            v[n++] = SDL_Vertex{{px + dx + kHead[i] * u, py + dy + kHead[i + 1] * u}, color, {}};
        }
        for (int i = 0; i < 12; i += 2) {
            v[n++] = SDL_Vertex{{px + dx + kTail[i] * u, py + dy + kTail[i + 1] * u}, color, {}};
        }
        SDL_RenderGeometry(renderer, nullptr, v, n, nullptr, 0);
    };
    const float o = std::max(1.0f, u);
    const SDL_FColor ink{0.05f, 0.05f, 0.08f, 1.0f};
    for (const auto &d :
         {std::pair{-o, 0.0f}, std::pair{o, 0.0f}, std::pair{0.0f, -o}, std::pair{0.0f, o}}) {
        draw(d.first, d.second, ink);
    }
    draw(0, 0, pressed ? SDL_FColor{1.0f, 0.78f, 0.25f, 1.0f} : SDL_FColor{1, 1, 1, 1});
}

} // namespace maplethief::frontend
