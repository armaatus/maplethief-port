// Touch input (#65, ADR 0047); see input.h.
#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "maplethief/uikit.h"
#include "present.h"

namespace maplethief::frontend {

namespace {

int uikit_sink(void *, std::uint32_t finger, std::uint32_t phase, float x, float y) {
    return mt_uikit_touch(finger, phase, x, y);
}

const char *phase_name(std::uint32_t phase) {
    switch (phase) {
    case MT_UIKIT_TOUCH_BEGAN:
        return "began";
    case MT_UIKIT_TOUCH_MOVED:
        return "moved";
    case MT_UIKIT_TOUCH_ENDED:
        return "ended";
    default:
        return "cancelled";
    }
}

// The screen's last point inside it on each axis (just below 480 and 320).
const float kMaxX = std::nextafter(static_cast<float>(kScreenWidth), 0.0f);
const float kMaxY = std::nextafter(static_cast<float>(kScreenHeight), 0.0f);

bool on_screen(float x, float y) {
    return x >= 0 && y >= 0 && x < static_cast<float>(kScreenWidth) &&
           y < static_cast<float>(kScreenHeight);
}

} // namespace

const char *input_source_name(InputSource source) {
    switch (source) {
    case InputSource::kMouse:
        return "mouse";
    case InputSource::kFinger:
        return "finger";
    case InputSource::kScripted:
        return "scripted";
    case InputSource::kGamepad:
        return "gamepad";
    case InputSource::kKeyboard:
        return "keyboard";
    }
    return "?";
}

bool is_touch_source(InputSource source) {
    return source == InputSource::kMouse || source == InputSource::kFinger ||
           source == InputSource::kScripted;
}

Input::Input() : sink_(uikit_sink) {}

void Input::set_sink(TouchSink sink, void *user) {
    sink_ = sink != nullptr ? sink : uikit_sink;
    sink_user_ = sink != nullptr ? user : nullptr;
}

void Input::set_activity_listener(ActivityListener listener, void *user) {
    listener_ = listener;
    listener_user_ = user;
}

void Input::set_window(SDL_Window *window, const Placement *placement) {
    window_ = window;
    placement_ = placement;
}

bool Input::is_down(std::uint32_t finger) const {
    return std::any_of(live_.begin(), live_.end(),
                       [&](const Live &l) { return l.finger == finger; });
}

Input::Live *Input::find(std::uint32_t finger) {
    for (Live &l : live_) {
        if (l.finger == finger) {
            return &l;
        }
    }
    return nullptr;
}

ScreenPoint Input::map(float wx, float wy) const {
    if (window_ == nullptr || placement_ == nullptr || placement_->rect.w <= 0 ||
        placement_->rect.h <= 0) {
        return ScreenPoint{}; // no image yet: everything is "the bars"
    }
    return window_point_to_screen(window_, *placement_, wx, wy);
}

bool Input::emit(InputSource source, std::uint32_t finger, std::uint32_t phase, ScreenPoint p) {
    Live *l = find(finger);
    if (phase == MT_UIKIT_TOUCH_BEGAN) {
        if (!p.inside) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                         "input: a %s touch at (%g, %g) is outside the image: dropped",
                         input_source_name(source), static_cast<double>(p.x),
                         static_cast<double>(p.y));
            return false;
        }
        if (l != nullptr) {
            // Down again without an up in between (a lost event): the old touch lifts.
            sink_(sink_user_, finger, MT_UIKIT_TOUCH_ENDED, l->x, l->y);
            live_.erase(live_.begin() + (l - live_.data()));
        }
        live_.push_back(Live{finger, source, p.x, p.y});
        sink_(sink_user_, finger, phase, p.x, p.y);
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "input: finger 0x%X (%s) %s at (%g, %g)",
                     static_cast<unsigned>(finger), input_source_name(source), phase_name(phase),
                     static_cast<double>(p.x), static_cast<double>(p.y));
        if (listener_ != nullptr) {
            listener_(listener_user_, source);
        }
        return true;
    }
    if (l == nullptr) {
        return false; // not down: a press that began over the bars, or a stray event
    }
    // Clamped to the image: a drag that leaves it stays on its edge.
    const float x = std::clamp(p.x, 0.0f, kMaxX);
    const float y = std::clamp(p.y, 0.0f, kMaxY);
    if (phase == MT_UIKIT_TOUCH_MOVED && x == l->x && y == l->y) {
        return false; // no movement on the screen (along a clamped edge, or sub-point jitter)
    }
    l->x = x;
    l->y = y;
    sink_(sink_user_, finger, phase, x, y);
    if (phase != MT_UIKIT_TOUCH_MOVED) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "input: finger 0x%X (%s) %s at (%g, %g)",
                     static_cast<unsigned>(finger), input_source_name(source), phase_name(phase),
                     static_cast<double>(x), static_cast<double>(y));
        live_.erase(live_.begin() + (l - live_.data()));
    }
    return true;
}

bool Input::touch(InputSource source, std::uint32_t id, std::uint32_t phase, float x, float y) {
    if (phase > MT_UIKIT_TOUCH_CANCELLED || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    const std::uint32_t finger =
        source == InputSource::kScripted ? kMouseFinger : kFirstVirtualFinger + id;
    return emit(source, finger, phase, ScreenPoint{x, y, on_screen(x, y)});
}

void Input::note(InputSource source) {
    if (listener_ != nullptr) {
        listener_(listener_user_, source);
    }
}

void Input::release_where(bool (*pred)(const Live &)) {
    const std::vector<Live> live = live_;
    for (const Live &l : live) {
        if (pred(l)) {
            emit(l.source, l.finger, MT_UIKIT_TOUCH_ENDED, ScreenPoint{l.x, l.y, true});
        }
    }
}

void Input::release_all() {
    release_where([](const Live &) { return true; });
}

bool Input::mouse_button(const SDL_Event &e) {
    const SDL_MouseButtonEvent &b = e.button;
    if (b.which == SDL_TOUCH_MOUSEID) {
        return false; // SDL's mouse from a touch: the finger events have it
    }
    const std::uint32_t phase = b.down ? MT_UIKIT_TOUCH_BEGAN : MT_UIKIT_TOUCH_ENDED;
    if (b.button == SDL_BUTTON_LEFT) {
        return emit(InputSource::kMouse, kMouseFinger, phase, map(b.x, b.y));
    }
    if (b.button == SDL_BUTTON_RIGHT && (right_.on || is_down(kRightMouseFinger))) {
        // At the fixed point, wherever the pointer is.
        const auto x = static_cast<float>(right_.x);
        const auto y = static_cast<float>(right_.y);
        return emit(InputSource::kMouse, kRightMouseFinger, phase,
                    ScreenPoint{x, y, on_screen(x, y)});
    }
    return false;
}

bool Input::mouse_motion(const SDL_Event &e) {
    const SDL_MouseMotionEvent &m = e.motion;
    if (m.which == SDL_TOUCH_MOUSEID || !is_down(kMouseFinger)) {
        return false;
    }
    // The button is no longer held: its up was lost (released outside a window that did not
    // capture the mouse). The touch ends where the pointer is.
    const std::uint32_t phase =
        (m.state & SDL_BUTTON_LMASK) != 0 ? MT_UIKIT_TOUCH_MOVED : MT_UIKIT_TOUCH_ENDED;
    return emit(InputSource::kMouse, kMouseFinger, phase, map(m.x, m.y));
}

bool Input::finger(const SDL_Event &e) {
    const SDL_TouchFingerEvent &f = e.tfinger;
    if (f.touchID == SDL_MOUSE_TOUCHID) {
        return false; // SDL's touch from the mouse: the mouse events have it
    }
    const SDL_TouchDeviceType type = SDL_GetTouchDeviceType(f.touchID);
    if (type == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE || type == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE) {
        return false; // a trackpad: its fingers move the pointer, they do not touch the screen
    }
    // The finger's slot: the one it has while down, or a free one for a new finger.
    std::uint32_t slot = kMaxScreenFingers;
    for (std::uint32_t i = 0; i < kMaxScreenFingers; i++) {
        if (slots_[i].used && slots_[i].touch_id == f.touchID &&
            slots_[i].finger_id == f.fingerID) {
            slot = i;
            break;
        }
    }
    const bool down = e.type == SDL_EVENT_FINGER_DOWN;
    if (slot == kMaxScreenFingers) {
        if (!down) {
            return false; // a finger we never saw go down (or one that began over the bars)
        }
        for (std::uint32_t i = 0; i < kMaxScreenFingers; i++) {
            if (!slots_[i].used) {
                slot = i;
                break;
            }
        }
        if (slot == kMaxScreenFingers) {
            return false; // too many fingers at once
        }
    }
    std::uint32_t phase = MT_UIKIT_TOUCH_MOVED;
    if (down) {
        phase = MT_UIKIT_TOUCH_BEGAN;
    } else if (e.type == SDL_EVENT_FINGER_UP) {
        phase = MT_UIKIT_TOUCH_ENDED;
    } else if (e.type == SDL_EVENT_FINGER_CANCELED) {
        phase = MT_UIKIT_TOUCH_ENDED; // an interruption: lifted, not cancelled (input.h)
    }
    // Normalised to the window (0..1) -> window points.
    int w = 0, h = 0;
    if (window_ != nullptr) {
        SDL_GetWindowSize(window_, &w, &h);
    }
    const ScreenPoint p = map(f.x * static_cast<float>(w), f.y * static_cast<float>(h));
    const std::uint32_t id = kFirstScreenFinger + slot;
    const bool sent = emit(InputSource::kFinger, id, phase, p);
    slots_[slot].used = is_down(id);
    slots_[slot].touch_id = f.touchID;
    slots_[slot].finger_id = f.fingerID;
    return sent;
}

bool Input::handle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return mouse_button(e);
    case SDL_EVENT_MOUSE_MOTION:
        return mouse_motion(e);
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        return finger(e);
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED: {
        const bool any = !live_.empty();
        release_all();
        for (Slot &s : slots_) {
            s.used = false;
        }
        return any;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
        const bool any = is_down(kMouseFinger) || is_down(kRightMouseFinger);
        release_where([](const Live &l) { return l.source == InputSource::kMouse; });
        return any;
    }
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat) {
            note(InputSource::kKeyboard);
        }
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        note(InputSource::kGamepad);
        return false;
    default:
        return false;
    }
}

} // namespace maplethief::frontend
