// Hiding the on-screen key pad (#71, ADR 0052); see keypad_hide.h.
#include "keypad_hide.h"

#include <algorithm>
#include <cmath>

#include "maplethief/input.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

KeypadHider::KeypadHider(KeypadMode mode, bool auto_hidden)
    : mode_(mode),
      want_hidden_(mode == KeypadMode::kAlways || (mode == KeypadMode::kAuto && auto_hidden)),
      from_(want_hidden_ ? 0.0f : 1.0f), target_(from_) {}

void KeypadHider::set_mode(KeypadMode mode, double now) {
    mode_ = mode;
    want_hidden_ = mode == KeypadMode::kAlways;
    if (mode != KeypadMode::kAuto) {
        fade_to(want_hidden_ ? 0.0f : 1.0f, now);
    } else if (target_ == 0) {
        fade_to(1.0f, now);
    }
}

void KeypadHider::listener(void *user, InputSource source) {
    static_cast<KeypadHider *>(user)->on_activity(source);
}

void KeypadHider::on_activity(InputSource source) {
    if (mode_ == KeypadMode::kAuto) {
        want_hidden_ = !is_touch_source(source);
    }
}

void KeypadHider::turn(double now, std::size_t touches_down) {
    const float want = want_hidden_ ? 0.0f : 1.0f;
    if (want == target_) {
        return;
    }
    if (want == 0 && touches_down != 0) {
        return; // a finger's key is held: hide once it lets go
    }
    fade_to(want, now);
}

void KeypadHider::fade_to(float target, double now) {
    if (target == target_) {
        return;
    }
    from_ = opacity(now);
    target_ = target;
    start_ = now;
    duration_ = kKeypadFadeSeconds * std::fabs(target_ - from_);
    if (target == 0) {
        hides_++;
    } else {
        shows_++;
    }
}

float KeypadHider::opacity(double now) const {
    if (!(duration_ > 0) || now >= start_ + duration_) {
        return target_;
    }
    const double f = std::max(0.0, (now - start_) / duration_);
    return static_cast<float>(from_ + (target_ - from_) * f);
}

bool KeypadHider::fading(double now) const { return duration_ > 0 && now < start_ + duration_; }

bool apply_keypad_opacity(float opacity) {
    const std::uint32_t view = mt_input_pad_panel_view(0);
    return view != 0 && mt_uikit_set_host_opacity(view, opacity) != 0;
}

} // namespace maplethief::frontend
