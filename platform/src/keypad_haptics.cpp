// The touch key pad's haptics (#194); see keypad_haptics.h.
#include "keypad_haptics.h"

#include <SDL3/SDL_platform_defines.h>

#include <algorithm>
#include <cmath>

#include "maplethief/cg.h"
#include "maplethief/hooks.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

#if !defined(SDL_PLATFORM_IOS)
// iOS's is in platform/ios/keypad_haptics_ios.mm.
std::unique_ptr<HapticsSink> make_platform_haptics_sink() { return nullptr; }
#endif

std::uint32_t keypad_key_of_rgba(const float rgba[4]) {
    if (rgba == nullptr || !(rgba[3] > 0)) {
        return 0;
    }
    std::uint32_t key = 0x01000000u;
    for (int i = 0; i < 3; i++) {
        const float c = std::isfinite(rgba[i]) ? std::clamp(rgba[i], 0.0f, 1.0f) : 0.0f;
        key |= static_cast<std::uint32_t>(std::lround(c * 255.0f)) << (16 - 8 * i);
    }
    return key;
}

bool keypad_haptics_active(std::int32_t panel, bool pad_view, bool pad_hidden, bool overlay_open) {
    return panel == 0 && pad_view && !pad_hidden && !overlay_open;
}

void KeypadHapticsTracker::set_sink(HapticsSink *sink) {
    if (sink_ != nullptr && prepared_) {
        sink_->release();
        stats_.releases++;
    }
    prepared_ = false;
    sink_ = sink;
    sync();
}

void KeypadHapticsTracker::set_level(KeypadHaptics level) {
    level_ = level;
    sync();
}

void KeypadHapticsTracker::set_active(bool active) {
    active_ = active;
    sync();
}

// The sink is prepared exactly while presses may tap, at the level in effect.
void KeypadHapticsTracker::sync() {
    const bool want = sink_ != nullptr && active_ && level_ != KeypadHaptics::kOff;
    if (prepared_ && sink_ != nullptr && (!want || prepared_level_ != level_)) {
        sink_->release();
        stats_.releases++;
        prepared_ = false;
    }
    if (want && !prepared_) {
        sink_->prepare(level_);
        stats_.prepares++;
        prepared_ = true;
        prepared_level_ = level_;
    }
}

std::uint32_t KeypadHapticsTracker::key_of(std::uint32_t finger) const {
    for (const auto &[f, key] : keys_) {
        if (f == finger) {
            return key;
        }
    }
    return 0;
}

void KeypadHapticsTracker::key_read(std::uint32_t finger, std::uint32_t phase, std::uint32_t key) {
    stats_.reads++;
    auto it = std::find_if(keys_.begin(), keys_.end(),
                           [finger](const auto &e) { return e.first == finger; });
    // A touch's end (the game reads the end point to clear its key): the finger is gone.
    if (phase == MT_UIKIT_TOUCH_ENDED || phase == MT_UIKIT_TOUCH_CANCELLED) {
        if (it != keys_.end()) {
            keys_.erase(it);
        }
        return;
    }
    const std::uint32_t before = it != keys_.end() ? it->second : 0;
    if (it != keys_.end()) {
        it->second = key;
    } else {
        // A finger whose end was never read (its touch ended on another view): a bound, not a
        // leak. Ten fingers is what a phone reports; the oldest goes first.
        constexpr std::size_t kMaxFingers = 16;
        if (keys_.size() >= kMaxFingers) {
            keys_.erase(keys_.begin());
        }
        keys_.emplace_back(finger, key);
    }
    if (key == 0 || key == before) {
        return; // no key, or the key held
    }
    if (!active_ || level_ == KeypadHaptics::kOff || sink_ == nullptr) {
        stats_.muted++;
        return;
    }
    sink_->impact(level_);
    stats_.impacts++;
}

// --- the host side ------------------------------------------------------------------------------

namespace {

int runtime_delivering(std::uint32_t *finger, std::uint32_t *phase) {
    return mt_uikit_touch_delivering(finger, phase);
}
int runtime_color(mt_cpu *cpu, std::uint32_t color, float rgba[4]) {
    return mt_cg_color_rgba(cpu, color, rgba);
}

KeypadHapticsTracker *g_tracker = nullptr;
KeypadHapticsReaders g_readers;

// -[TouchKeyPadView colorAtPixel:onImage:] 0xB4DB5 (keypad_layout.cpp has the arguments): the
// read runs as it would (keypad_layout's handler, then the body), then its UIColor (r0, nil off
// the image) is the key under the touch being delivered.
int on_color_at_pixel(mt_cpu *cpu, const mt_hook_call *call) {
    call->original(cpu);
    std::uint32_t finger = 0, phase = 0;
    if (g_tracker == nullptr || g_readers.delivering == nullptr ||
        g_readers.delivering(&finger, &phase) == 0) {
        return MT_HOOK_HANDLED; // not a touch's hit test
    }
    float rgba[4] = {0, 0, 0, 0};
    std::uint32_t key = 0;
    if (cpu->r[0] != 0 && g_readers.color != nullptr && g_readers.color(cpu, cpu->r[0], rgba)) {
        key = keypad_key_of_rgba(rgba);
    }
    g_tracker->key_read(finger, phase, key);
    return MT_HOOK_HANDLED;
}

} // namespace

KeypadHapticsReaders keypad_haptics_runtime_readers() {
    return KeypadHapticsReaders{runtime_delivering, runtime_color};
}

void install_keypad_haptics(KeypadHapticsTracker *tracker, KeypadHapticsReaders readers) {
    g_tracker = tracker;
    g_readers = readers;
    mt_hook_wrap(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL, on_color_at_pixel, nullptr, 0);
}

void keypad_haptics_turn(KeypadHaptics level, bool active) {
    if (g_tracker == nullptr) {
        return;
    }
    g_tracker->set_level(level);
    g_tracker->set_active(active);
    const bool on = level != KeypadHaptics::kOff;
    if ((mt_hook_wrapped(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL, on_color_at_pixel) != 0) != on) {
        mt_hook_wrap(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL, on_color_at_pixel, nullptr, on ? 1 : 0);
    }
}

void uninstall_keypad_haptics() {
    mt_hook_unwrap(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL, on_color_at_pixel);
    if (g_tracker != nullptr) {
        g_tracker->set_active(false); // the sink lets its generator go
    }
    g_tracker = nullptr;
    g_readers = KeypadHapticsReaders{};
}

} // namespace maplethief::frontend
