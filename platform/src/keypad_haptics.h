// The touch key pad's haptics (#194, ADR 0055's addendum): on iOS, one haptic tap at each press
// of a touch key pad key, so the flat glass feels more like buttons. Separate from the game's
// own vibration option and from the gamepad's rumble (ADR 0053).
//
//   keypad_haptics = off | light | medium   (settings.h; iOS only, default light there)
//
// What a press is: a new key under a finger. The game hit-tests each finger against its colour
// mask KeyPadArea.png with -[TouchKeyPadView colorAtPixel:onImage:] 0xB4DB5 when the finger
// lands and each time it moves (docs/controls/keypad.md), so a wrapper on that function
// (mt_hook_wrap: keypad_layout's handler stays, ADR 0086) sees, for the touch being delivered
// (mt_uikit_touch_delivering, ADR 0047's path), which colour the game read:
//   - a finger that lands on a key, or slides onto another key (another arm of the d-pad): one
//     tap
//   - the same key read again (a held key, a move within it): nothing
//   - no key (a transparent pixel, or nil off the image) and the read at a touch's end:
//     nothing; the finger is forgotten when it ends
//   - while the game pad is not the panel in use (a popup, a menu panel), the pad is hidden
//     (hide_keypad), the key pad editor is open, or the setting is off: nothing, though the
//     fingers' keys are still followed (a key held through a popup does not tap when it closes)
// The haptics follow the game's own hit test, so a custom layout, widescreen's offset and the
// game's own rules come with it.
//
// The sink: the platform's haptics. On iOS a UIImpactFeedbackGenerator (light or medium),
// made and prepared when the pad shows (so the first tap has no latency), prepared again after
// each tap, and let go when it hides (platform/ios/keypad_haptics_ios.mm). Elsewhere there is
// none: make_platform_haptics_sink() returns nullptr and game.cpp installs nothing, so the game
// runs byte for byte as before. Tests use a recording sink.
//
// Single thread: the guest's (the frontend's main thread, where UIKit's generator must be used).
#ifndef MAPLETHIEF_PLATFORM_KEYPAD_HAPTICS_H
#define MAPLETHIEF_PLATFORM_KEYPAD_HAPTICS_H

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "maplethief/recomp/recomp.h"
#include "settings.h"

namespace maplethief::frontend {

// Where the taps go. `level` is never kOff.
class HapticsSink {
  public:
    virtual ~HapticsSink() = default;
    // The pad shows (or the level changed while it shows): get ready to tap at `level`.
    virtual void prepare(KeypadHaptics level) = 0;
    // One tap at `level` (after a prepare at the same level).
    virtual void impact(KeypadHaptics level) = 0;
    // The pad hides, or the setting went off: the generator may go.
    virtual void release() = 0;
};

// The platform's sink: iOS's impact generator; nullptr elsewhere.
std::unique_ptr<HapticsSink> make_platform_haptics_sink();

// The key a mask colour read stands for, by its colour: 0 for no key (alpha 0, or no colour),
// else 0x01RRGGBB (each component * 255, rounded).
std::uint32_t keypad_key_of_rgba(const float rgba[4]);

// Whether presses may tap now: the game pad (panel 0) is the key pad's panel in use and shown
// (no popup or menu panel over it), not hidden by hide_keypad, and no overlay of the port's
// (the key pad editor) takes the touches.
bool keypad_haptics_active(std::int32_t panel, bool pad_view, bool pad_hidden, bool overlay_open);

struct KeypadHapticsStats {
    std::uint64_t reads = 0;    // mask reads seen for a delivered touch
    std::uint64_t impacts = 0;  // taps sent to the sink
    std::uint64_t muted = 0;    // presses while inactive or off (no tap)
    std::uint64_t prepares = 0; // sink prepares
    std::uint64_t releases = 0; // sink releases
};

// The decision (pure: no SDL, no runtime). game.cpp feeds it the level and the pad's state each
// turn; the wrapper feeds it each mask read.
class KeypadHapticsTracker {
  public:
    // `sink` may be nullptr (nothing is sent); not owned.
    void set_sink(HapticsSink *sink);
    void set_level(KeypadHaptics level);
    void set_active(bool active);
    // The game read the mask for `finger`'s touch in `phase` (MT_UIKIT_TOUCH_*) and found `key`
    // (keypad_key_of_rgba). Taps if that is a new key under the finger.
    void key_read(std::uint32_t finger, std::uint32_t phase, std::uint32_t key);

    KeypadHaptics level() const { return level_; }
    bool active() const { return active_; }
    bool prepared() const { return prepared_; }
    // The key under `finger` as last read (0: none, or the finger is not down).
    std::uint32_t key_of(std::uint32_t finger) const;
    const KeypadHapticsStats &stats() const { return stats_; }

  private:
    void sync();

    HapticsSink *sink_ = nullptr;
    KeypadHaptics level_ = KeypadHaptics::kOff;
    bool active_ = false;
    bool prepared_ = false;
    KeypadHaptics prepared_level_ = KeypadHaptics::kOff;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> keys_; // finger -> key
    KeypadHapticsStats stats_;
};

// --- the host side (the wrapper on -[TouchKeyPadView colorAtPixel:onImage:]) -----------------

// What the wrapper reads: the touch being delivered and a UIColor's components. The defaults
// are the runtime's (mt_uikit_touch_delivering, mt_cg_color_rgba); tests give their own.
struct KeypadHapticsReaders {
    int (*delivering)(std::uint32_t *finger, std::uint32_t *phase) = nullptr;
    int (*color)(mt_cpu *cpu, std::uint32_t color, float rgba[4]) = nullptr;
};
KeypadHapticsReaders keypad_haptics_runtime_readers();

// Wraps 0xB4DB5 for `tracker` (not owned), off until keypad_haptics_turn says otherwise.
void install_keypad_haptics(KeypadHapticsTracker *tracker,
                            KeypadHapticsReaders readers = keypad_haptics_runtime_readers());
// Each turn: the level and whether presses may tap; the wrapper is on while the level is not
// off. Nothing before install_keypad_haptics.
void keypad_haptics_turn(KeypadHaptics level, bool active);
void uninstall_keypad_haptics();

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_KEYPAD_HAPTICS_H
