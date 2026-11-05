// The touch key pad's haptics (#194, keypad_haptics.h) with synthetic data only, on every OS: the
// decision with a recording sink (one tap per new key under a finger, none for holds, off-mask
// reads or a touch's end, none while the pad is hidden, in a popup or the setting is off), the
// sink's prepare and release, the mask colour as a key, the wrapper on -[TouchKeyPadView
// colorAtPixel:onImage:] on synthetic calls as the recompiler writes them (alone and around
// keypad_layout's handler), and the keypad_haptics row (iOS only in the menu, outside the
// presets). The runtime's two readers are tested in uikit_test.cpp (mt_uikit_touch_delivering)
// and cg_test.cpp (mt_cg_color_rgba). The iOS generator itself needs the device (#83).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "keypad_haptics.h"
#include "keypad_layout.h"
#include "maplethief/hooks.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/uikit.h"
#include "settings.h"
#include "settings_menu.h"

namespace {

namespace fe = maplethief::frontend;
using fe::KeypadHaptics;
using fe::KeypadHapticsTracker;
using u32 = std::uint32_t;

constexpr u32 kBegan = MT_UIKIT_TOUCH_BEGAN, kMoved = MT_UIKIT_TOUCH_MOVED,
              kEnded = MT_UIKIT_TOUCH_ENDED, kCancelled = MT_UIKIT_TOUCH_CANCELLED;
// Keys as keypad_key_of_rgba makes them from the mask's colours (docs/controls/keypad.md).
constexpr u32 kLeft = 0x01FF0000u, kDown = 0x0100FF00u, kRight = 0x010000FFu, kJump = 0x017F0000u,
              kAttack = 0x01007F00u;
constexpr u32 kFinger1 = 0x100, kFinger2 = 0x101;

// The fake sink: what it was asked, in order.
struct RecordingSink final : fe::HapticsSink {
    std::vector<std::string> calls;
    static const char *name(KeypadHaptics l) {
        return l == KeypadHaptics::kLight ? "light" : l == KeypadHaptics::kMedium ? "medium" : "?";
    }
    void prepare(KeypadHaptics level) override {
        calls.push_back(std::string("prepare ") + name(level));
    }
    void impact(KeypadHaptics level) override {
        calls.push_back(std::string("impact ") + name(level));
    }
    void release() override { calls.emplace_back("release"); }
    std::size_t impacts() const {
        std::size_t n = 0;
        for (const std::string &c : calls) {
            n += c.rfind("impact", 0) == 0 ? 1 : 0;
        }
        return n;
    }
};

// A tracker at `level`, shown, on `sink`, its prepare forgotten.
void ready(KeypadHapticsTracker &t, RecordingSink &sink,
           KeypadHaptics level = KeypadHaptics::kLight) {
    t.set_sink(&sink);
    t.set_level(level);
    t.set_active(true);
    sink.calls.clear();
}

} // namespace

TEST_CASE("keypad haptics: one impact per new key under a finger") {
    RecordingSink sink;
    KeypadHapticsTracker t;
    ready(t, sink);
    t.key_read(kFinger1, kBegan, kLeft); // lands on the left arm
    CHECK(sink.calls == std::vector<std::string>{"impact light"});
    CHECK(t.key_of(kFinger1) == kLeft);
    // Held: the game reads it again at each move within the key; no repeat.
    for (int i = 0; i < 5; i++) {
        t.key_read(kFinger1, kMoved, kLeft);
    }
    CHECK(sink.impacts() == 1);
    // A second finger on attack: its own tap, the first finger's key unchanged.
    t.key_read(kFinger2, kBegan, kAttack);
    CHECK(sink.impacts() == 2);
    CHECK(t.key_of(kFinger1) == kLeft);
    t.key_read(kFinger2, kMoved, kAttack);
    CHECK(sink.impacts() == 2);
    // The ends (the game reads the end point too): nothing, and the fingers are gone.
    t.key_read(kFinger2, kEnded, kAttack);
    t.key_read(kFinger1, kEnded, kLeft);
    CHECK(sink.impacts() == 2);
    CHECK(t.key_of(kFinger1) == 0);
    CHECK(t.key_of(kFinger2) == 0);
    // The same key pressed again by a new touch is a new press.
    t.key_read(kFinger1, kBegan, kLeft);
    CHECK(sink.impacts() == 3);
    t.key_read(kFinger1, kCancelled, kLeft);
    CHECK(t.key_of(kFinger1) == 0);
    CHECK(t.stats().impacts == 3);
    CHECK(t.stats().reads == 12);
}

TEST_CASE("keypad haptics: a slide onto another key taps once; a slide off and back taps again") {
    RecordingSink sink;
    KeypadHapticsTracker t;
    ready(t, sink, KeypadHaptics::kMedium);
    t.key_read(kFinger1, kBegan, kLeft);
    t.key_read(kFinger1, kMoved, kDown); // the d-pad's next arm
    t.key_read(kFinger1, kMoved, kDown);
    t.key_read(kFinger1, kMoved, kRight);
    CHECK(sink.calls ==
          std::vector<std::string>{"impact medium", "impact medium", "impact medium"});
    // Off every key (the d-pad's middle, a transparent pixel), then back onto the same arm.
    t.key_read(kFinger1, kMoved, 0);
    CHECK(sink.impacts() == 3);
    CHECK(t.key_of(kFinger1) == 0);
    t.key_read(kFinger1, kMoved, kRight);
    CHECK(sink.impacts() == 4);
}

TEST_CASE("keypad haptics: off-mask touches give nothing") {
    RecordingSink sink;
    KeypadHapticsTracker t;
    ready(t, sink);
    // A transparent pixel (the field, the NPC check) and nil (off the image): no key.
    t.key_read(kFinger1, kBegan, 0);
    t.key_read(kFinger1, kMoved, 0);
    t.key_read(kFinger2, kBegan, fe::keypad_key_of_rgba(nullptr));
    const float clear[4] = {0, 0, 0, 0};
    t.key_read(kFinger2, kMoved, fe::keypad_key_of_rgba(clear));
    CHECK(sink.calls.empty());
    // Then onto a key from there: that is a press.
    t.key_read(kFinger1, kMoved, kJump);
    CHECK(sink.impacts() == 1);
}

TEST_CASE("keypad haptics: none while the pad is hidden, in a popup or off; held keys don't tap "
          "when it comes back") {
    RecordingSink sink;
    KeypadHapticsTracker t;
    t.set_sink(&sink);
    t.set_level(KeypadHaptics::kLight);
    // Before the pad shows: no prepare, no tap.
    CHECK(sink.calls.empty());
    t.key_read(kFinger1, kBegan, kLeft);
    CHECK(sink.calls.empty());
    CHECK(t.stats().muted == 1);
    // The pad shows: prepared (the generator is ready before the first press).
    t.set_active(true);
    CHECK(sink.calls == std::vector<std::string>{"prepare light"});
    CHECK(t.prepared());
    // The held key goes on being held: no tap now that the pad shows.
    t.key_read(kFinger1, kMoved, kLeft);
    CHECK(sink.impacts() == 0);
    // A popup (or the pad hidden): released, and presses are muted but followed.
    t.set_active(false);
    CHECK(sink.calls.back() == "release");
    t.key_read(kFinger2, kBegan, kAttack);
    CHECK(sink.impacts() == 0);
    CHECK(t.key_of(kFinger2) == kAttack);
    t.set_active(true);
    t.key_read(kFinger2, kMoved, kAttack); // still held after the popup: no tap
    CHECK(sink.impacts() == 0);
    t.key_read(kFinger2, kMoved, kJump); // a new key: a tap
    CHECK(sink.impacts() == 1);
    // Off: released; presses give nothing.
    t.set_level(KeypadHaptics::kOff);
    CHECK(sink.calls.back() == "release");
    CHECK_FALSE(t.prepared());
    t.key_read(kFinger1, kMoved, kDown);
    CHECK(sink.impacts() == 1);
    // A level change while shown makes the generator again at the new style.
    t.set_level(KeypadHaptics::kLight);
    t.set_level(KeypadHaptics::kMedium);
    CHECK(sink.calls == std::vector<std::string>{"prepare light", "release", "prepare light",
                                                 "impact light", "release", "prepare light",
                                                 "release", "prepare medium"});
    t.key_read(kFinger1, kMoved, kRight);
    CHECK(sink.calls.back() == "impact medium");
    // No sink: nothing to call, nothing breaks.
    t.set_sink(nullptr);
    CHECK(sink.calls.back() == "release");
    t.key_read(kFinger1, kMoved, kLeft);
    CHECK(sink.impacts() == 2);
    CHECK_FALSE(t.prepared());
}

TEST_CASE("keypad haptics: when presses may tap, and the mask colour as a key") {
    // The game pad (panel 0) shown and in use, not hidden, no editor.
    CHECK(fe::keypad_haptics_active(0, true, false, false));
    CHECK_FALSE(fe::keypad_haptics_active(7, true, false, false));  // a popup's panel
    CHECK_FALSE(fe::keypad_haptics_active(1, true, false, false));  // the popup menu
    CHECK_FALSE(fe::keypad_haptics_active(-1, true, false, false)); // no key pad panel
    CHECK_FALSE(fe::keypad_haptics_active(0, false, false, false)); // no pad view yet
    CHECK_FALSE(fe::keypad_haptics_active(0, true, true, false));   // hide_keypad
    CHECK_FALSE(fe::keypad_haptics_active(0, true, false, true));   // the key pad editor

    const float red[4] = {1, 0, 0, 1}, half[4] = {0, 127.0f / 255, 127.0f / 255, 1},
                faint[4] = {0, 0, 1, 0.01f}, none[4] = {1, 1, 1, 0};
    CHECK(fe::keypad_key_of_rgba(red) == kLeft);
    CHECK(fe::keypad_key_of_rgba(half) == 0x01007F7Fu); // short 1
    CHECK(fe::keypad_key_of_rgba(faint) == kRight);
    CHECK(fe::keypad_key_of_rgba(none) == 0);
    CHECK(fe::keypad_key_of_rgba(nullptr) == 0);
    // Black is a colour, not "no key": only alpha decides.
    const float black[4] = {0, 0, 0, 1};
    CHECK(fe::keypad_key_of_rgba(black) == 0x01000000u);
}

namespace {

// -[TouchKeyPadView colorAtPixel:onImage:] as the recompiler writes it, with a stand-in body
// that answers a colour object for the point (x < 240: the left arm's red, else nil).
int g_body_calls = 0;
void color_at_pixel_body(mt_cpu *MT_RESTRICT cpu) {
    g_body_calls++;
    float x = 0;
    std::memcpy(&x, &cpu->r[2], 4);
    cpu->r[0] = x < 240 ? 0xC0102u : 0u; // a "UIColor"
}
void color_at_pixel(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL, color_at_pixel_body);
}

// The readers: the touch "being delivered" and the colour object's components.
bool g_delivering = false;
u32 g_finger = 0, g_phase = 0;
int fake_delivering(u32 *finger, u32 *phase) {
    if (!g_delivering) {
        return 0;
    }
    *finger = g_finger;
    *phase = g_phase;
    return 1;
}
int fake_color(mt_cpu *, u32 color, float rgba[4]) {
    if (color != 0xC0102u) {
        return 0;
    }
    rgba[0] = 1;
    rgba[1] = rgba[2] = 0;
    rgba[3] = 1;
    return 1;
}

void read_at(mt_cpu &cpu, float x, float y) {
    std::memcpy(&cpu.r[2], &x, 4);
    std::memcpy(&cpu.r[3], &y, 4);
    color_at_pixel(&cpu);
}

void deliver(mt_cpu &cpu, u32 finger, u32 phase, float x, float y) {
    g_delivering = true;
    g_finger = finger;
    g_phase = phase;
    read_at(cpu, x, y);
    g_delivering = false;
}

} // namespace

TEST_CASE("keypad haptics: the wrapper on colorAtPixel:onImage: taps for the delivered touch") {
    mt_cpu cpu{};
    cpu.r[MT_LR] = 0x2345u;
    g_body_calls = 0;
    RecordingSink sink;
    KeypadHapticsTracker t;
    t.set_sink(&sink);
    fe::install_keypad_haptics(&t, fe::KeypadHapticsReaders{fake_delivering, fake_color});
    // Installed but off until a turn says the level.
    deliver(cpu, kFinger1, kBegan, 30, 250);
    CHECK(g_body_calls == 1);
    CHECK(t.stats().reads == 0);

    fe::keypad_haptics_turn(KeypadHaptics::kLight, true);
    CHECK(sink.calls == std::vector<std::string>{"prepare light"});
    deliver(cpu, kFinger1, kBegan, 30, 250); // the left arm
    CHECK(g_body_calls == 2);
    CHECK(cpu.r[0] == 0xC0102u); // the game gets its colour unchanged
    CHECK(sink.impacts() == 1);
    deliver(cpu, kFinger1, kMoved, 32, 252); // held
    CHECK(sink.impacts() == 1);
    deliver(cpu, kFinger2, kBegan, 300, 100); // nil: off the image
    CHECK(cpu.r[0] == 0u);
    CHECK(sink.impacts() == 1);
    // A read outside a touch's delivery (none in the game, but): not a press.
    read_at(cpu, 10, 250);
    CHECK(sink.impacts() == 1);
    CHECK(t.stats().reads == 3);
    deliver(cpu, kFinger1, kEnded, 32, 252);
    CHECK(t.key_of(kFinger1) == 0);

    // In a popup: muted; off: the wrapper is off and the game's read is untouched.
    fe::keypad_haptics_turn(KeypadHaptics::kLight, false);
    deliver(cpu, kFinger1, kBegan, 30, 250);
    CHECK(sink.impacts() == 1);
    deliver(cpu, kFinger1, kEnded, 30, 250);
    fe::keypad_haptics_turn(KeypadHaptics::kOff, true);
    const std::uint64_t reads = t.stats().reads;
    deliver(cpu, kFinger1, kBegan, 30, 250);
    CHECK(t.stats().reads == reads);
    CHECK(g_body_calls == 9);

    // Around keypad_layout's handler (ADR 0086): the game reads the moved mask point, the
    // wrapper sees that read's colour.
    fe::keypad_haptics_turn(KeypadHaptics::kMedium, true);
    fe::KeypadLayout moved;
    REQUIRE(fe::parse_keypad_layout("dpad=100,300,0,100", &moved));
    fe::install_keypad_layout(moved);
    sink.calls.clear();
    deliver(cpu, kFinger2, kBegan, 30, 250); // the d-pad's old place: no key there now
    CHECK(sink.impacts() == 0);
    CHECK(fe::keypad_layout_stats().remapped == 1);
    fe::uninstall_keypad_layout();

    fe::uninstall_keypad_haptics();
    CHECK(sink.calls.back() == "release");
    deliver(cpu, kFinger1, kMoved, 40, 250);
    CHECK(t.stats().reads == reads + 1);
    mt_hook_reset(); // the hook's counts, which keypad_layout_test reads
}

TEST_CASE("keypad haptics: the setting: off | light | medium, iOS only, outside the presets") {
    const fe::SettingInfo *row = fe::find_setting("keypad_haptics");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kControls);
    CHECK_FALSE(row->presets.in_presets());
    CHECK(row->ios_only);
    CHECK(row->applies == fe::Applies::kAtOnce);
    CHECK(fe::find_setting_flag("--keypad-haptics") == row);
    CHECK(fe::setting_choices(*row) == std::vector<std::string>{"off", "light", "medium"});
    // Light on iOS, off elsewhere (where it does nothing).
    fe::Settings s;
    CHECK(s.keypad_haptics == (fe::kSettingsForIos ? KeypadHaptics::kLight : KeypadHaptics::kOff));
    CHECK(fe::setting_text(s, *row) == (fe::kSettingsForIos ? "light" : "off"));
    // The file takes it on every platform.
    CHECK(fe::parse_settings("keypad_haptics = MEDIUM\n", &s).empty());
    CHECK(s.keypad_haptics == KeypadHaptics::kMedium);
    CHECK_FALSE(fe::parse_settings("keypad_haptics = strong\n", &s).empty());
    CHECK(s.keypad_haptics == KeypadHaptics::kMedium);
    // A preset never touches it.
    for (const fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        fe::Settings q = s;
        REQUIRE(fe::apply_setting(&q, fe::kPresetKey, fe::preset_name(p), nullptr));
        CHECK(q.keypad_haptics == KeypadHaptics::kMedium);
    }
    // Shown in the menu only in a build for iOS.
    CHECK(fe::setting_shown(*row, true));
    CHECK_FALSE(fe::setting_shown(*row, false));
    CHECK(fe::setting_shown(*fe::find_setting("rumble"), false));
    // A tab with only such a row is no tab elsewhere (settings_menu_test's reachability test
    // checks the registry's rows, the ios_only ones only on iOS).
    static constexpr fe::SettingInfo kRows[] = {
        {.key = "scale_mode",
         .group = fe::SettingGroup::kVideo,
         .comment = "# scale",
         .value = fe::choice<&fe::Settings::scale_mode>(fe::kScaleModeNames)},
        {.key = "keypad_haptics",
         .group = fe::SettingGroup::kAudio,
         .comment = "# haptics",
         .value = fe::choice<&fe::Settings::keypad_haptics>(fe::kKeypadHapticsNames),
         .ios_only = true},
    };
    fe::SettingsMenu menu(kRows, nullptr, nullptr, nullptr);
    const bool audio_tab = std::find(menu.tabs().begin(), menu.tabs().end(),
                                     fe::SettingGroup::kAudio) != menu.tabs().end();
    CHECK(audio_tab == fe::kSettingsForIos);
}
