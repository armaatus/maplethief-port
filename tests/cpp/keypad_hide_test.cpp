// Hiding the on-screen key pad (#71, ADR 0052) with synthetic data only: KeypadHider's modes
// (auto, always, never), the activity of every input source (the mouse, a finger, a scripted
// tap, a gamepad, the keyboard) through Input's activity listener, a hide that waits while a
// touch is down, the fade's values, hide_keypad in settings.ini, and the opacity reaching the
// game pad (TouchKeyPadView's subview 0) of a synthetic guest without touching its alpha.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "input.h"
#include "keypad_hide.h"
#include "maplethief/cf.h"
#include "maplethief/hle/imports.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/uikit.h"
#include "settings.h"
#include "uikit/uikit_internal.h"

namespace {

namespace fe = maplethief::frontend;
using fe::InputSource;
using fe::KeypadHider;
using fe::KeypadMode;
using u32 = std::uint32_t;

int drop_touch(void *, u32, u32, float, float) { return 0; }

// An Input whose touches go nowhere, with the hider as its activity listener.
struct Rig {
    fe::Input input;
    KeypadHider hider;
    explicit Rig(KeypadMode mode) : hider(mode) {
        input.set_sink(drop_touch, nullptr);
        input.set_activity_listener(KeypadHider::listener, &hider);
    }
    void touch(InputSource s, u32 phase, u32 id = 0) { input.touch(s, id, phase, 240, 160); }
    void tap(InputSource s) {
        touch(s, MT_UIKIT_TOUCH_BEGAN);
        touch(s, MT_UIKIT_TOUCH_ENDED);
    }
    void turn(double now) { hider.turn(now, input.live()); }
};

} // namespace

TEST_CASE("keypad hide: the mode names, hide_keypad in settings.ini and on the command line") {
    KeypadMode m = KeypadMode::kAuto;
    CHECK(fe::parse_keypad_mode("always", &m));
    CHECK(m == KeypadMode::kAlways);
    CHECK(fe::parse_keypad_mode("NEVER", &m));
    CHECK(m == KeypadMode::kNever);
    CHECK(fe::parse_keypad_mode("Auto", &m));
    CHECK(m == KeypadMode::kAuto);
    for (const char *bad : {"", "on", "off", "auto ", "hidden", "alwayss"}) {
        CHECK_FALSE(fe::parse_keypad_mode(bad, &m));
    }
    CHECK(std::string(fe::keypad_mode_name(KeypadMode::kNever)) == "never");

    fe::Settings s;
    CHECK(s.hide_keypad == KeypadMode::kAuto);
    std::vector<std::string> problems =
        fe::parse_settings("hide_keypad = always\nHIDE_KEYPAD=never\n", &s);
    CHECK(s.hide_keypad == KeypadMode::kNever);
    REQUIRE(problems.size() == 1); // the repeated key
    problems = fe::parse_settings("hide_keypad = sometimes\n", &s);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0] == "line 1: hide_keypad: 'sometimes' is not auto, always or never, ignored");
    CHECK(s.hide_keypad == KeypadMode::kNever);
    // A new file lists it with its comment; an old one keeps its text and gets it appended.
    s.hide_keypad = KeypadMode::kAlways;
    const std::string fresh = fe::serialise_settings(s);
    CHECK(fresh.find("\nhide_keypad = always\n") != std::string::npos);
    fe::Settings back;
    CHECK(fe::parse_settings(fresh, &back).empty());
    CHECK(back == s);
    const std::string kept = fe::serialise_settings(s, "# mine\nscale_mode = fit\n");
    CHECK(kept.rfind("# mine\nscale_mode = integer\n", 0) == 0);
    CHECK(kept.find("hide_keypad = always\n") != std::string::npos);
    // --hide-keypad goes through apply_setting, as main.cpp checks it.
    std::string why;
    CHECK(fe::apply_setting(&s, "hide_keypad", "auto", &why));
    CHECK(s.hide_keypad == KeypadMode::kAuto);
    CHECK_FALSE(fe::apply_setting(&s, "hide_keypad", "1", &why));
}

TEST_CASE("keypad hide: auto hides on gamepad and keyboard input, shows on every kind of touch") {
    Rig r(KeypadMode::kAuto);
    double t = 10;
    r.turn(t);
    CHECK_FALSE(r.hider.hidden());
    CHECK(r.hider.opacity(t) == 1.0f);

    // Touches of every touch source keep it shown.
    for (const InputSource s :
         {InputSource::kMouse, InputSource::kFinger, InputSource::kScripted}) {
        r.tap(s);
        r.turn(t += 0.01);
        CHECK_FALSE(r.hider.hidden());
    }
    CHECK(r.hider.hides() == 0);

    // A gamepad button (Input::note, as Gamepads and Input::handle report it): fades out.
    r.input.note(InputSource::kGamepad);
    CHECK_FALSE(r.hider.hidden()); // nothing before the turn
    r.turn(t += 0.01);
    CHECK(r.hider.hidden());
    CHECK(r.hider.hides() == 1);
    CHECK(r.hider.fading(t));
    CHECK(r.hider.opacity(t) == 1.0f);
    CHECK(r.hider.opacity(t + fe::kKeypadFadeSeconds / 2) == doctest::Approx(0.5f));
    CHECK(r.hider.opacity(t + fe::kKeypadFadeSeconds) == 0.0f);
    CHECK_FALSE(r.hider.fading(t + fe::kKeypadFadeSeconds));
    t += 1;
    // More non-touch input changes nothing.
    r.input.note(InputSource::kKeyboard);
    r.turn(t);
    CHECK(r.hider.hides() == 1);

    // A mouse click shows it: the touch was delivered too (it went to the sink: one live
    // touch until it ends).
    r.touch(InputSource::kMouse, MT_UIKIT_TOUCH_BEGAN);
    CHECK(r.input.live() == 1);
    r.turn(t += 0.01);
    CHECK_FALSE(r.hider.hidden());
    CHECK(r.hider.shows() == 1);
    CHECK(r.hider.opacity(t + fe::kKeypadFadeSeconds) == 1.0f);
    r.touch(InputSource::kMouse, MT_UIKIT_TOUCH_ENDED);

    // The keyboard hides it as the gamepad does; a finger shows it.
    t += 1;
    r.input.note(InputSource::kKeyboard);
    r.turn(t);
    CHECK(r.hider.hidden());
    t += 1;
    r.tap(InputSource::kFinger);
    r.turn(t);
    CHECK_FALSE(r.hider.hidden());
    CHECK(r.hider.hides() == 2);
    CHECK(r.hider.shows() == 2);

    // A gamepad's synthetic touch (a touch fallback, Input::touch with the gamepad's source) is
    // gamepad input, not a touch.
    t += 1;
    r.touch(InputSource::kGamepad, MT_UIKIT_TOUCH_BEGAN, 0x10);
    r.touch(InputSource::kGamepad, MT_UIKIT_TOUCH_ENDED, 0x10);
    r.turn(t);
    CHECK(r.hider.hidden());
}

TEST_CASE("keypad hide: a hide waits while a touch is down, so a hidden pad never holds a key") {
    Rig r(KeypadMode::kAuto);
    double t = 5;
    // The mouse holds the d-pad; a gamepad button is pressed meanwhile.
    r.touch(InputSource::kMouse, MT_UIKIT_TOUCH_BEGAN);
    r.input.note(InputSource::kGamepad);
    for (int i = 0; i < 10; i++) {
        r.turn(t += 0.05);
        CHECK_FALSE(r.hider.hidden());
        CHECK(r.hider.opacity(t) == 1.0f);
    }
    // It lets go: the pad fades out from that turn.
    r.touch(InputSource::kMouse, MT_UIKIT_TOUCH_ENDED);
    CHECK(r.input.live() == 0);
    r.turn(t += 0.05);
    CHECK(r.hider.hidden());
    CHECK(r.hider.opacity(t + fe::kKeypadFadeSeconds) == 0.0f);
    // A touch that begins while hidden shows it at once (no waiting).
    r.touch(InputSource::kFinger, MT_UIKIT_TOUCH_BEGAN, 3);
    r.turn(t += 0.05);
    CHECK_FALSE(r.hider.hidden());
    r.touch(InputSource::kFinger, MT_UIKIT_TOUCH_ENDED, 3);
}

TEST_CASE("keypad hide: a fade reversed half way takes its share of the time") {
    Rig r(KeypadMode::kAuto);
    const double f = fe::kKeypadFadeSeconds;
    r.input.note(InputSource::kGamepad);
    r.turn(0);
    CHECK(r.hider.opacity(f * 0.75) == doctest::Approx(0.25f));
    // A touch at 3/4 of the fade-out: back from 0.25 to 1 in 3/4 of the time.
    r.tap(InputSource::kMouse);
    r.turn(f * 0.75);
    CHECK(r.hider.opacity(f * 0.75) == doctest::Approx(0.25f));
    CHECK(r.hider.opacity(f * 0.75 + f * 0.375) == doctest::Approx(0.625f));
    CHECK(r.hider.fading(f * 0.75 + f * 0.7));
    CHECK(r.hider.opacity(f * 0.75 + f * 0.75) == 1.0f);
    CHECK_FALSE(r.hider.fading(f * 0.75 + f * 0.75));
}

TEST_CASE("keypad hide: always is hidden from the start, never is never hidden") {
    Rig always(KeypadMode::kAlways);
    CHECK(always.hider.hidden());
    CHECK(always.hider.opacity(0) == 0.0f);
    CHECK_FALSE(always.hider.fading(0));
    double t = 1;
    for (const InputSource s :
         {InputSource::kMouse, InputSource::kFinger, InputSource::kScripted}) {
        always.tap(s);
        always.turn(t += 0.1);
        CHECK(always.hider.hidden());
        CHECK(always.hider.opacity(t + 1) == 0.0f);
    }
    always.input.note(InputSource::kGamepad);
    always.turn(t += 0.1);
    CHECK(always.hider.opacity(t) == 0.0f);
    CHECK(always.hider.hides() + always.hider.shows() == 0);

    Rig never(KeypadMode::kNever);
    for (const InputSource s : {InputSource::kGamepad, InputSource::kKeyboard}) {
        never.input.note(s);
        never.turn(t += 0.1);
        CHECK_FALSE(never.hider.hidden());
        CHECK(never.hider.opacity(t + 1) == 1.0f);
    }
    never.tap(InputSource::kMouse);
    never.turn(t += 0.1);
    CHECK(never.hider.hides() + never.hider.shows() == 0);

    // A mode set at run time fades to what it asks for.
    KeypadHider h(KeypadMode::kNever);
    h.set_mode(KeypadMode::kAlways, 100);
    CHECK(h.hidden());
    CHECK(h.opacity(100 + fe::kKeypadFadeSeconds) == 0.0f);
    h.set_mode(KeypadMode::kAuto, 200);
    CHECK_FALSE(h.hidden());
    CHECK(h.opacity(200 + fe::kKeypadFadeSeconds) == 1.0f);
}

namespace {

u32 fb(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// UIView's SDK size is 46: EAGLView's ivars up to +92, the pad's to touchKeyPadEnable at +228.
const mt_objc_host_class k_eagl = {"EAGLView", "UIView", 96, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_pad = {"TouchKeyPadView", "UIView", 232, nullptr, 0, nullptr, 0};

double g_now = 1000;
double test_clock(void *) { return g_now; }

// A window with an EAGLView holding a TouchKeyPadView with two panels, as -[EAGLView
// startApp] and -[TouchKeyPadView initWithFrame:] build them (eleven in the game).
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0, eagl = 0, pad = 0, panel0 = 0, panel1 = 0;

    Guest() {
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        REQUIRE(mt_objc_add_host_class(rt, &k_eagl, 0, 0) == 0);
        REQUIRE(mt_objc_add_host_class(rt, &k_pad, 0, 0) == 0);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
        mt_ui::State &st = mt_ui::state(&cpu);
        st.config.clock = test_clock;
        st.app = send(send(klass("UIApplication"), "alloc"), "init");
        const u32 window = view("UIWindow", 320, 480);
        st.windows.push_back(window);
        eagl = view("EAGLView", 320, 480);
        send(window, "addSubview:", {eagl});
        pad = view("TouchKeyPadView", 480, 320);
        send(eagl, "addSubview:", {pad});
        std::memcpy(cpu.mem + eagl + 84, &pad, 4);
        panel0 = view("UIView", 480, 320);
        send(pad, "addSubview:", {panel0});
        panel1 = view("UIView", 480, 320);
        send(pad, "addSubview:", {panel1});
        mt_input_configure(nullptr);
    }
    ~Guest() {
        mt_input_configure(nullptr);
        mt_objc_pool_pop(rt, pool);
        mt_ui::state(&cpu).config.clock = nullptr;
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;

    u32 klass(const char *name) const { return mt_objc_class_named(rt, name); }
    u32 send(u32 self, const char *sel, std::initializer_list<u32> args = {}) {
        const std::vector<u32> v(args);
        return mt_objc_msg(&cpu, self, mt_objc_sel(rt, sel), v.data(), static_cast<u32>(v.size()));
    }
    u32 view(const char *cls, float w, float h) {
        return send(send(klass(cls), "alloc"), "initWithFrame:", {fb(0), fb(0), fb(w), fb(h)});
    }
};

} // namespace

TEST_CASE("keypad hide: the opacity reaches the game pad's panel, never its alpha") {
    Guest g;
    // Before the first turn the runtime has seen no pad.
    CHECK(mt_input_pad_panel_view(0) == 0);
    CHECK_FALSE(fe::apply_keypad_opacity(0));
    mt_uikit_turn(&g.cpu, g_now);
    CHECK(mt_input_pad_panel_view(0) == g.panel0);
    CHECK(mt_input_pad_panel_view(1) == g.panel1);
    CHECK(mt_input_pad_panel_view(2) == 0);
    CHECK(mt_input_pad_panel_view(-1) == 0);

    KeypadHider h(KeypadMode::kAlways);
    REQUIRE(fe::apply_keypad_opacity(h.opacity(g_now)));
    CHECK(mt_uikit_host_opacity(g.panel0) == 0.0f);
    CHECK(mt_uikit_host_opacity(g.panel1) == 1.0f);
    CHECK(mt_ui::view_of(&g.cpu, g.panel0)->model.alpha == 1.0);
    CHECK_FALSE(mt_ui::view_of(&g.cpu, g.panel0)->hidden);
    REQUIRE(fe::apply_keypad_opacity(1));
    CHECK(mt_uikit_host_opacity(g.panel0) == 1.0f);
}
