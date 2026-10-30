// Button prompts and the focus markers (#146, ADR 0051's addendum) with synthetic data only: the
// prompt table and the glyph's side, the control a prompt shows (the defaults, the preferred
// keys, a remap), the device in use, the popup menu's list cursor read from a synthetic guest
// memory, the hints a turn makes (and nothing for a touch player), the glyphs switching live
// between the keyboard and virtual Xbox 360, PS5 and Switch Pro pads (by hash), and the prompts'
// layer on a software renderer against the same drawing on the CPU.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "actions.h"
#include "canvas.h"
#include "focus.h"
#include "gamepad.h"
#include "glyphs.h"
#include "input.h"
#include "maplethief/input.h"
#include "prompts.h"
#include "remap.h"
#include "scaling.h"
#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u32 = std::uint32_t;
using fe::Control;
using fe::ControlKind;
using fe::FocusTarget;
using fe::Prompt;
using fe::PromptDevice;
using virtual_pad::Kind;

FocusTarget control(u32 object, float x, float y, float w, float h, const char *target,
                    const char *action) {
    FocusTarget t;
    t.object = object;
    t.order = object;
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    t.px = x + w / 2;
    t.py = y + h / 2;
    t.action = action;
    t.target = target;
    t.view_class = "UIButton";
    return t;
}

// A popup's two buttons (docs/controls/keypad.md: the touch rects reach past the art).
std::vector<FocusTarget> ok_cancel() {
    return {control(1, 100, 200, 120, 40, "PopupKeyPadView", "releaseOkButton:"),
            control(2, 250, 200, 120, 40, "PopupKeyPadView", "releaseCancelButton:")};
}

SDL_Event key_event(SDL_Keycode k, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}
SDL_Event typed(SDL_EventType type) {
    SDL_Event e{};
    e.type = type;
    return e;
}
SDL_Event pad_event(SDL_EventType type, SDL_JoystickID which) {
    SDL_Event e{};
    e.type = type;
    e.gdevice.which = which;
    return e;
}
SDL_Event axis_event(SDL_JoystickID which, Sint16 v) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = which;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    e.gaxis.value = v;
    return e;
}

Control key(SDL_Keycode k) { return Control{ControlKind::kKey, static_cast<int>(k)}; }
Control button(SDL_GamepadButton b) { return Control{ControlKind::kGamepadButton, b}; }

// The control `action` shows for `device` under the default bindings (or `bindings`).
Control shown(std::uint32_t action, PromptDevice device,
              const std::vector<fe::Binding> &bindings = fe::default_bindings()) {
    Control c{ControlKind::kKey, 0};
    REQUIRE(fe::prompt_control(bindings, action, device, &c));
    return c;
}
bool same(const Control &a, const Control &b) { return a.kind == b.kind && a.code == b.code; }

// FNV-1a, 64 bits.
std::uint64_t fnv1a(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::uint8_t b : bytes) {
        h = (h ^ b) * 0x100000001B3ull;
    }
    return h;
}

// A synthetic screen: bands of colour, no game art.
std::vector<std::uint8_t> backdrop(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    for (std::uint32_t y = 0; y < h; y++) {
        for (std::uint32_t x = 0; x < w; x++) {
            std::uint8_t *p = &px[(std::size_t{y} * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(40 + (x * 160) / w);
            p[1] = static_cast<std::uint8_t>(90 + (y * 120) / h);
            p[2] = static_cast<std::uint8_t>(((x / 32 + y / 32) % 2) != 0 ? 150 : 110);
            p[3] = 255;
        }
    }
    return px;
}

std::string output_dir() {
    const char *env = std::getenv("MAPLETHIEF_TEST_OUTPUT_DIR");
    return env != nullptr && *env != '\0' ? env : MT_TEST_OUTPUT_DIR;
}

bool write_png(const fs::path &path, std::vector<std::uint8_t> &rgba, std::uint32_t w,
               std::uint32_t h) {
    SDL_Surface *s =
        SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h), SDL_PIXELFORMAT_ABGR8888,
                              rgba.data(), static_cast<int>(w * 4));
    const bool ok = s != nullptr && SDL_SavePNG(s, path.string().c_str());
    SDL_DestroySurface(s);
    return ok;
}

} // namespace

TEST_CASE("prompts: the table's actions, and each glyph beside its button's art") {
    CHECK(fe::prompt_action("PopupKeyPadView", "releaseOkButton:") == MT_ACTION_OK);
    CHECK(fe::prompt_action("PopupKeyPadView", "releaseNoButton:") == MT_ACTION_BACK);
    CHECK(fe::prompt_action("PopupKeyPadView", "releaseRevivalButton:") == MT_ACTION_REVIVE);
    CHECK(fe::prompt_action("NpcKeyPadView", "releaseRefuseButton:") == MT_ACTION_BACK);
    CHECK(fe::prompt_action("MarketKeyPadView", "releaseSellTabButton:") == MT_ACTION_PLUS);
    CHECK(fe::prompt_action("MarketKeyPadView", "releaseChargeButton:") == MT_ACTION_SLOT4);
    CHECK(fe::prompt_action("TouchKeyPadView", "releaseAsteriskButton:") == MT_ACTION_MAP);
    CHECK(fe::prompt_action("ChooseCharacterView", "clickNew:") == MT_ACTION_SLOT4);
    CHECK(fe::prompt_action("TestView", "releaseOkButton:") == MT_ACTION_NONE);
    CHECK(fe::prompt_action("PopupKeyPadView", "tap:") == MT_ACTION_NONE);

    // A popup's OK and Cancel: OK's glyph left of its art, Cancel's right of it.
    const std::vector<Prompt> p = fe::prompts_for(ok_cancel(), 7, false);
    REQUIRE(p.size() == 2);
    CHECK(p[0] == Prompt{MT_ACTION_OK, 118, 192, 220, false});
    CHECK(p[1] == Prompt{MT_ACTION_BACK, 251, 350, 220, true});
    // The field's panel: none (its key pad is the game's own).
    CHECK(fe::prompts_for(ok_cancel(), 0, false).empty());
    // A scroll view gets none; a control outside the table gets none.
    std::vector<FocusTarget> t = ok_cancel();
    t[0].kind = FocusTarget::Kind::kScroll;
    t[1].target = "TestView";
    CHECK(fe::prompts_for(t, 7, false).empty());

    // A control over a text box (the popups' Empty) or the screen's width (the cut scenes'
    // strip): in its bottom-right corner.
    const std::vector<Prompt> corner = fe::prompts_for(
        {control(1, 60, 80, 360, 160, "PopupKeyPadView", "releaseEmptyButton:"),
         control(2, 0, 270, 480, 50, "TouchKeyPadView", "releaseOkButton:")},
        3, false);
    REQUIRE(corner.size() == 2);
    CHECK(corner[0] == Prompt{MT_ACTION_OK, 414, 414, 228, false});
    CHECK(corner[1] == Prompt{MT_ACTION_OK, 474, 474, 308, false});

    // The character select's New: on its screen, not under a popup (panel 7).
    const std::vector<FocusTarget> select = {
        control(1, 380, 270, 90, 40, "ChooseCharacterView", "clickNew:"),
        control(2, 10, 270, 90, 40, "ChooseCharacterView", "clickBack:")};
    CHECK(fe::prompts_for(select, -1, false).size() == 1);
    CHECK(fe::prompts_for(select, 7, false).empty());
    // With the controller focus active: back at the control back touches, as Focus does.
    const std::vector<Prompt> focused = fe::prompts_for(select, -1, true);
    REQUIRE(focused.size() == 2);
    CHECK(fe::focus_back_target(select) == 1);
    CHECK(focused[1] == Prompt{MT_ACTION_BACK, 8, 102, 290, false});
    // Not a second one when the table gave back already (Cancel).
    CHECK(fe::prompts_for(ok_cancel(), 9, true).size() == 2);
}

TEST_CASE("prompts: the glyph goes on the side asked for, or the other where it leaves the screen") {
    // Left of the button: ends at `before`.
    CHECK(fe::prompt_left(Prompt{MT_ACTION_OK, 118, 192, 220, false}, 12, 480) == 106);
    // Right of it: starts at `after`.
    CHECK(fe::prompt_left(Prompt{MT_ACTION_BACK, 251, 350, 220, true}, 12, 480) == 350);
    // No room on the left: right of it.
    CHECK(fe::prompt_left(Prompt{MT_ACTION_OK, 8, 60, 100, false}, 12, 480) == 60);
    // No room on the right: left of it.
    CHECK(fe::prompt_left(Prompt{MT_ACTION_BACK, 400, 472, 100, true}, 12, 480) == 388);
    // A wide glyph that fits on neither side stays on the screen.
    CHECK(fe::prompt_left(Prompt{MT_ACTION_OK, 20, 470, 100, false}, 40, 480) == 1);
}

TEST_CASE("prompts: the control shown: the gamepad's first, the keyboard's preferred, a remap") {
    // OK: A (South) on a pad; Enter on the keyboard, though Space and Z confirm too.
    CHECK(same(shown(MT_ACTION_OK, PromptDevice::kGamepad), button(SDL_GAMEPAD_BUTTON_SOUTH)));
    CHECK(same(shown(MT_ACTION_OK, PromptDevice::kKeyboard), key(SDLK_RETURN)));
    // Back: B (East), not Start; Esc, not X or Backspace.
    CHECK(same(shown(MT_ACTION_BACK, PromptDevice::kGamepad), button(SDL_GAMEPAD_BUTTON_EAST)));
    CHECK(same(shown(MT_ACTION_BACK, PromptDevice::kKeyboard), key(SDLK_ESCAPE)));
    // The map: View / Back, M. The tab keys: LB / RB, Q / E. Charge and New ('#'): RT, N.
    CHECK(same(shown(MT_ACTION_MAP, PromptDevice::kGamepad), button(SDL_GAMEPAD_BUTTON_BACK)));
    CHECK(same(shown(MT_ACTION_MAP, PromptDevice::kKeyboard), key(SDLK_M)));
    CHECK(same(shown(MT_ACTION_MINUS, PromptDevice::kGamepad),
               button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)));
    CHECK(same(shown(MT_ACTION_PLUS, PromptDevice::kKeyboard), key(SDLK_E)));
    CHECK(same(shown(MT_ACTION_SLOT4, PromptDevice::kGamepad),
               Control{ControlKind::kGamepadTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER}));
    CHECK(same(shown(MT_ACTION_SLOT4, PromptDevice::kKeyboard), key(SDLK_N)));
    // The popup's minus: the d-pad's left; the left arrow.
    CHECK(shown(MT_ACTION_LEFT, PromptDevice::kGamepad).kind == ControlKind::kDirection);
    CHECK(same(shown(MT_ACTION_LEFT, PromptDevice::kKeyboard), key(SDLK_LEFT)));

    Control c;
    CHECK_FALSE(
        fe::prompt_control(fe::default_bindings(), MT_ACTION_OK, PromptDevice::kNone, &c));
    CHECK_FALSE(
        fe::prompt_control(fe::default_bindings(), MT_ACTION_NONE, PromptDevice::kGamepad, &c));

    // A remap (the bindings file, the overlay): OK moved to X (West) and off Enter.
    std::vector<fe::Binding> b = fe::default_bindings();
    for (fe::Binding &row : b) {
        if (same(row.control, button(SDL_GAMEPAD_BUTTON_SOUTH)) ||
            same(row.control, key(SDLK_RETURN))) {
            row.menu = MT_ACTION_NONE;
        }
        if (same(row.control, button(SDL_GAMEPAD_BUTTON_WEST))) {
            row.menu = MT_ACTION_OK;
        }
    }
    CHECK(same(shown(MT_ACTION_OK, PromptDevice::kGamepad, b), button(SDL_GAMEPAD_BUTTON_WEST)));
    CHECK(same(shown(MT_ACTION_OK, PromptDevice::kKeyboard, b), key(SDLK_SPACE)));
    // Nothing of the device bound to it: no prompt.
    for (fe::Binding &row : b) {
        if (row.menu == MT_ACTION_OK) {
            row.menu = MT_ACTION_NONE;
        }
    }
    CHECK_FALSE(fe::prompt_control(b, MT_ACTION_OK, PromptDevice::kKeyboard, &c));
}

TEST_CASE("prompts: the device used last") {
    fe::DeviceTracker d;
    CHECK(d.device() == PromptDevice::kNone);
    d.handle(key_event(SDLK_RETURN, true));
    CHECK(d.device() == PromptDevice::kKeyboard);
    d.handle(typed(SDL_EVENT_MOUSE_BUTTON_DOWN));
    CHECK(d.device() == PromptDevice::kNone);
    d.handle(key_event(SDLK_RETURN, true, true)); // a repeat: not a new press
    CHECK(d.device() == PromptDevice::kNone);
    d.handle(key_event(SDLK_RETURN, false));
    CHECK(d.device() == PromptDevice::kNone);
    d.handle(pad_event(SDL_EVENT_GAMEPAD_ADDED, 7));
    CHECK(d.device() == PromptDevice::kGamepad);
    d.handle(typed(SDL_EVENT_FINGER_DOWN));
    CHECK(d.device() == PromptDevice::kNone);
    d.handle(axis_event(7, 9000)); // a stick resting off centre: nothing
    CHECK(d.device() == PromptDevice::kNone);
    d.handle(axis_event(7, -30000));
    CHECK(d.device() == PromptDevice::kGamepad);
    d.handle(key_event(SDLK_Z, true));
    CHECK(d.device() == PromptDevice::kKeyboard);
    d.handle(pad_event(SDL_EVENT_GAMEPAD_BUTTON_DOWN, 7));
    CHECK(d.device() == PromptDevice::kGamepad);
    // A second pad, then both go: none once the last one has.
    d.handle(pad_event(SDL_EVENT_GAMEPAD_ADDED, 8));
    d.handle(pad_event(SDL_EVENT_GAMEPAD_REMOVED, 7));
    CHECK(d.device() == PromptDevice::kGamepad);
    d.handle(pad_event(SDL_EVENT_GAMEPAD_REMOVED, 8));
    CHECK(d.device() == PromptDevice::kNone);
    // A pad going while the keyboard is in use leaves the keyboard's.
    d.handle(pad_event(SDL_EVENT_GAMEPAD_ADDED, 9));
    d.handle(key_event(SDLK_Z, true));
    d.handle(pad_event(SDL_EVENT_GAMEPAD_REMOVED, 9));
    CHECK(d.device() == PromptDevice::kKeyboard);
    CHECK(d.changes() == 10);
    CHECK(std::string(fe::prompt_device_name(PromptDevice::kGamepad)) == "gamepad");
}

namespace {

// A synthetic guest memory with a CDataMgr and the popup menu's two list widgets.
struct Guest {
    static constexpr u32 kSize = 0x200000;
    static constexpr u32 kMgr = 0x180000;
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(kSize, 0);
    mt_cpu cpu{};

    Guest() {
        cpu.mem = mem.data();
        cpu.mem_size = kSize;
        put(0x11A984, kMgr);
        // The item list (0xF14B8): 6 cells of 70 x 36 in rows of 4 from (0, 0), the list at
        // (83, 62), its rect (70, 50, 330, 200). The equipment list (0xF14C0): 2 cells.
        widget(0xF14B8, 0x181000, 6, 83, 62);
        widget(0xF14C0, 0x188000, 2, 83, 172);
    }
    void put(u32 addr, u32 v) {
        for (int i = 0; i < 4; i++) {
            mem[addr + static_cast<u32>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    void widget(u32 var, u32 base, u32 cells, u32 ox, u32 oy) {
        put(var, base);             // the variable that holds the widget
        put(base, base + 0x100);    // the widget
        const u32 w = base + 0x100; // its cells at +0x00, the count at +0x04, the rect at +0x1C
        put(w, base + 0x200);
        put(w + 4, cells);
        put(w + 0x1C, 70);
        put(w + 0x20, 50);
        put(w + 0x24, 330);
        put(w + 0x28, 200);
        for (u32 i = 0; i < cells; i++) {
            const u32 c = base + 0x1000 + 0x100 * i;
            put(base + 0x200 + 4 * i, c);
            put(c + 0x08, ox); // the list's offset
            put(c + 0x0C, oy);
            put(c + 0x20, 70 * (i % 4)); // the cell's rect
            put(c + 0x24, 36 * (i / 4));
            put(c + 0x28, 70);
            put(c + 0x2C, 36);
        }
    }
    void menu(u32 tab, u32 open, u32 items, u32 equip) {
        put(kMgr + 0xB0, tab);
        put(kMgr + 0xA8, open);
        put(kMgr + 0xD8, items);
        put(kMgr + 0xDC, equip);
    }
};

bool same_rect(const fe::Rect &a, const fe::Rect &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

} // namespace

TEST_CASE("prompts: the popup menu's list cursor, read from guest memory") {
    Guest g;
    fe::PopupMenu m;
    // The tab bar: no list open, no cursor (the engine highlights the tab itself).
    g.menu(1, 0, 2, 0);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK(m.tab == 1);
    CHECK(m.open == 0);
    CHECK_FALSE(m.cursor);
    // The item list, cell 2: (83 + 140, 62).
    g.menu(1, 1, 2, 0);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK(m.cursor);
    CHECK(same_rect(m.cell, fe::Rect{223, 62, 70, 36}));
    // A cell reaching past the list's right edge (400): clipped.
    g.put(0x182300 + 0x20, 300);
    g.menu(1, 1, 3, 0);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK(same_rect(m.cell, fe::Rect{383, 62, 17, 36}));
    // Past the cells, or negative: none.
    g.menu(1, 1, 6, 0);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK_FALSE(m.cursor);
    g.menu(1, 1, 0xFFFFFFFF, 0);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK_FALSE(m.cursor);
    // The equipment list (open 2): its own cursor (+0xDC) over its own widget.
    g.menu(2, 2, 3, 1);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK(m.cursor);
    CHECK(same_rect(m.cell, fe::Rect{153, 172, 70, 36}));
    // Below the list's bottom (250): scrolled out.
    g.put(Guest::kMgr + 0xDC, 0);
    g.put(0x188000 + 0x1000 + 0x0C, 260);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK_FALSE(m.cursor);
    // The other tabs (skills, quests, ...): no list cursor read.
    g.menu(3, 3, 0, 0);
    REQUIRE(fe::read_popup_menu(&g.cpu, &m));
    CHECK_FALSE(m.cursor);
    // No CDataMgr yet, or no memory: false.
    g.put(0x11A984, 0);
    CHECK_FALSE(fe::read_popup_menu(&g.cpu, &m));
    CHECK_FALSE(fe::read_popup_menu(nullptr, &m));

    // Its own prompts: back by the close; the tab keys only on the tab bar.
    CHECK(fe::popup_menu_prompts(fe::PopupMenu{1, 0, false, {}}) ==
          std::vector<Prompt>{Prompt{MT_ACTION_BACK, 384, 412, 28, true},
                              Prompt{MT_ACTION_MINUS, 73, 73, 271, false},
                              Prompt{MT_ACTION_PLUS, 403, 403, 271, true}});
    CHECK(fe::popup_menu_prompts(fe::PopupMenu{1, 1, true, {}}).size() == 1);
}

namespace {

// Focus on a scripted panel and target list, and the hints of each turn.
struct HintRig {
    fe::Input input;
    fe::Focus focus{&input};
    fe::HintTracker hints;
    std::vector<FocusTarget> targets;
    std::int32_t panel = 7;
    fe::PopupMenu menu;
    bool prompts = true, markers = true;
    double now = 100;

    HintRig() {
        input.set_sink([](void *, u32, u32, float, float) { return 0; }, nullptr);
        focus.set_forward([](void *, u32, bool) {}, nullptr);
        focus.set_panel_source([](void *user) { return static_cast<HintRig *>(user)->panel; },
                               this);
        focus.set_target_source(
            [](void *user, mt_cpu *, std::vector<FocusTarget> *out) {
                *out = static_cast<HintRig *>(user)->targets;
            },
            this);
        focus.set_menu_source(
            [](void *user, mt_cpu *, std::int32_t *tab, std::int32_t *open) {
                *tab = static_cast<HintRig *>(user)->menu.tab;
                *open = static_cast<HintRig *>(user)->menu.open;
                return true;
            },
            this);
        hints.set_menu_source(
            [](void *user, const mt_cpu *, fe::PopupMenu *out) {
                *out = static_cast<HintRig *>(user)->menu;
                return true;
            },
            this);
    }
    void turn() {
        now += 1.0 / 60;
        focus.turn(nullptr, now);
        hints.turn(nullptr, focus, panel, prompts, markers);
    }
    std::size_t prompt_count() const { return hints.hints().prompts.size(); }
    std::size_t marker_count() const { return hints.hints().markers.size(); }
};

} // namespace

TEST_CASE("prompts: a turn's hints, none for a touch player, each part by its setting") {
    HintRig r;
    r.targets = ok_cancel();
    r.turn();
    // Nothing used yet (or a touch): nothing new on the screen.
    CHECK(r.hints.device() == PromptDevice::kNone);
    CHECK(r.prompt_count() == 0);
    // A key press: the popup's prompts.
    r.hints.handle(key_event(SDLK_RETURN, true));
    r.turn();
    CHECK(r.hints.device() == PromptDevice::kKeyboard);
    CHECK(r.prompt_count() == 2);
    r.prompts = false;
    r.turn();
    CHECK(r.prompt_count() == 0);
    r.prompts = true;

    // The popup menu (panel 1): its own prompts, and the list's cursor as a marker.
    r.panel = 1;
    r.targets.clear();
    r.menu = fe::PopupMenu{1, 1, true, fe::Rect{223, 62, 70, 36}};
    r.turn();
    CHECK(r.prompt_count() == 1);
    REQUIRE(r.marker_count() == 1);
    CHECK(same_rect(r.hints.hints().markers[0], fe::Rect{223, 62, 70, 36}));
    r.markers = false;
    r.turn();
    CHECK(r.marker_count() == 0);
    CHECK(r.prompt_count() == 1);
    r.markers = true;
    r.menu.open = 0;
    r.menu.cursor = false;
    r.turn();
    CHECK(r.prompt_count() == 3);
    CHECK(r.marker_count() == 0);
    // A touch: gone.
    r.hints.handle(typed(SDL_EVENT_FINGER_DOWN));
    r.turn();
    CHECK(r.prompt_count() == 0);
    // The field: none.
    r.hints.handle(key_event(SDLK_RETURN, true));
    r.panel = 0;
    r.turn();
    CHECK(r.prompt_count() == 0);
    CHECK(r.marker_count() == 0);
}

TEST_CASE("prompts: the focus shows before the first press while a pad or the keyboard is in use") {
    HintRig r;
    r.panel = -1;
    r.targets = {control(1, 46, 148, 120, 48, "MainMenuView", "clickPlay:"),
                 control(2, 46, 198, 120, 48, "MainMenuView", "clickHelp:")};
    r.turn();
    REQUIRE(r.focus.active());
    CHECK(r.focus.highlighted() == nullptr); // not used yet
    CHECK(fe::focus_highlight_target(r.focus, PromptDevice::kNone) == nullptr);
    const FocusTarget *f = fe::focus_highlight_target(r.focus, PromptDevice::kGamepad);
    REQUIRE(f != nullptr);
    CHECK(f->action == "clickPlay:");
    CHECK(fe::focus_highlight_target(r.focus, PromptDevice::kKeyboard) == f);
    // Used: Focus's own highlight, whatever the device.
    r.focus.action(MT_ACTION_DOWN, true);
    r.turn();
    r.focus.action(MT_ACTION_DOWN, false);
    r.turn();
    REQUIRE(r.focus.highlighted() != nullptr);
    CHECK(fe::focus_highlight_target(r.focus, PromptDevice::kNone)->action == "clickHelp:");
    // The field: none.
    r.panel = 0;
    r.targets.clear();
    r.turn();
    CHECK(fe::focus_highlight_target(r.focus, PromptDevice::kGamepad) == nullptr);
}

// --- on SDL3 virtual joysticks ------------------------------------------------------------

namespace {

struct Sdl {
    Sdl() {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
    }
    ~Sdl() { SDL_Quit(); }
};

// The frontend's routing (game.cpp's handle_event): the overlay, the devices, the glyphs, then
// the hints.
struct Front {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Remap remap{&actions, &gamepads};
    fe::HintTracker hints;
    double now = 100.0;

    Front() {
        actions.set_sink([](void *, std::uint32_t, bool) {}, nullptr);
        actions.set_panel_source([](void *) { return std::int32_t{7}; }, nullptr);
    }
    void send(const SDL_Event &e) {
        if (!remap.handle(e, now)) {
            input.handle(e);
            gamepads.handle(e);
        }
        remap.track(e);
        hints.handle(e);
    }
    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            send(e);
        }
    }
    void press(SDL_Keycode k) {
        send(key_event(k, true));
        send(key_event(k, false));
    }
    fe::PromptStyle style() const {
        return fe::PromptStyle{hints.device(), remap.glyphs().type(), gamepads.face_buttons()};
    }
    // The label of the control OK's prompt shows now ("" for none).
    std::string ok_label() const {
        Control c;
        if (!fe::prompt_control(actions.bindings(), MT_ACTION_OK, hints.device(), &c)) {
            return "";
        }
        return c.kind == ControlKind::kKey
                   ? fe::key_label(c.code)
                   : fe::pad_glyph_label(c, remap.glyphs().type(), gamepads.face_buttons());
    }
    // The popup's prompts over a synthetic screen at 2 pixels a point: their hash.
    std::uint64_t draw(const char *name, int *drawn) {
        std::vector<std::uint8_t> px = backdrop(960, 640);
        fe::Canvas canvas(px.data(), 960, 640, 2);
        *drawn = fe::draw_prompts(canvas, fe::prompts_for(ok_cancel(), 7, false), style(),
                                  actions.bindings());
        const fs::path out = fs::path(output_dir()) / "button_prompts";
        std::error_code ec;
        fs::create_directories(out, ec);
        CHECK(write_png(out / (std::string(name) + ".png"), px, 960, 640));
        const std::uint64_t h = fnv1a(px);
        char hex[32];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        MESSAGE(name << ": " << (out / name).string() << ".png hash " << hex);
        return h;
    }
};

} // namespace

TEST_CASE("prompts: the glyphs switch live between the keyboard and each pad's set") {
    Sdl sdl;
    Front f;
    f.pump();
    int drawn = 0;
    const std::uint64_t none = f.draw("none", &drawn);
    CHECK(drawn == 0);
    CHECK(none == fnv1a(backdrop(960, 640)));

    // The keyboard: key caps (Enter, Esc).
    f.press(SDLK_RETURN);
    CHECK(f.hints.device() == PromptDevice::kKeyboard);
    CHECK(f.ok_label() == "Enter");
    const std::uint64_t keys = f.draw("keyboard", &drawn);
    CHECK(drawn == 2);

    // An Xbox 360 pad connects: its set at once (A, B).
    auto xbox = std::make_unique<virtual_pad::Pad>(Kind::kXbox360);
    REQUIRE_MESSAGE(xbox->ok(), xbox->error);
    f.pump();
    CHECK(f.hints.device() == PromptDevice::kGamepad);
    CHECK(f.ok_label() == "A");
    const std::uint64_t x = f.draw("xbox", &drawn);
    CHECK(drawn == 2);

    // A key press: the keyboard's again; a press on the pad: the pad's.
    f.press(SDLK_ESCAPE);
    CHECK(f.ok_label() == "Enter");
    CHECK(f.draw("keyboard_again", &drawn) == keys);
    xbox->button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    f.pump();
    xbox->button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    f.pump();
    CHECK(f.ok_label() == "A");
    CHECK(f.draw("xbox_again", &drawn) == x);

    // A PS5 pad: Cross, Circle.
    auto ps5 = std::make_unique<virtual_pad::Pad>(Kind::kPs5, true);
    REQUIRE_MESSAGE(ps5->ok(), ps5->error);
    f.pump();
    CHECK(f.ok_label() == "Cross");
    const std::uint64_t ps = f.draw("playstation", &drawn);
    CHECK(drawn == 2);

    // A Switch Pro pad: by position the bottom button (B) confirms; by label A does.
    auto sw = std::make_unique<virtual_pad::Pad>(Kind::kSwitchPro);
    REQUIRE_MESSAGE(sw->ok(), sw->error);
    f.pump();
    CHECK(f.ok_label() == "B");
    const std::uint64_t ns = f.draw("nintendo", &drawn);
    f.gamepads.set_face_buttons(fe::FaceButtons::kLabel);
    CHECK(f.ok_label() == "A");
    const std::uint64_t ns_label = f.draw("nintendo_by_label", &drawn);
    f.gamepads.set_face_buttons(fe::FaceButtons::kPosition);

    // Each set draws its own glyphs (Nintendo's by label are A and B, as Xbox's); the drawing is
    // the port's own (integer arithmetic only), the same bytes everywhere.
    CHECK(ns_label == x);
    const std::vector<std::uint64_t> all = {none, keys, x, ps, ns};
    for (std::size_t i = 0; i < all.size(); i++) {
        for (std::size_t j = i + 1; j < all.size(); j++) {
            CHECK(all[i] != all[j]);
        }
    }
    CHECK(keys == 0x89b39765c23ac719ull);
    CHECK(x == 0x00fa0cfc1625f05dull);
    CHECK(ps == 0x80b7c8dd9ad4120dull);
    CHECK(ns == 0xc71ce87d5ff651edull);

    // A click: a touch player, no prompts. The pads going: none either.
    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    f.send(click);
    CHECK(f.ok_label().empty());
    f.press(SDLK_RETURN);
    sw.reset();
    ps5.reset();
    xbox.reset();
    f.pump();
    CHECK(f.hints.device() == PromptDevice::kKeyboard);
}

TEST_CASE("prompts: the layer on a renderer equals the drawing over the image") {
    // A 960 x 640 window: the image placed at 2x, the layer 960 x 640.
    SDL_Surface *target = SDL_CreateSurface(960, 640, SDL_PIXELFORMAT_ABGR8888);
    REQUIRE(target != nullptr);
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(target);
    REQUIRE_MESSAGE(renderer != nullptr, SDL_GetError());
    const fe::Placement placement = fe::place(fe::ScaleMode::kInteger, 960, 640, 480, 320);
    REQUIRE(placement.integer_scale == 2);
    const std::vector<Prompt> prompts = fe::prompts_for(ok_cancel(), 7, false);
    const fe::PromptStyle pad{PromptDevice::kGamepad, SDL_GAMEPAD_TYPE_PS5,
                              fe::FaceButtons::kPosition};
    const std::vector<fe::Binding> bindings = fe::default_bindings();

    const auto frame = [&](const std::vector<Prompt> &p, const fe::PromptStyle &s,
                           fe::HintLayer &layer) {
        SDL_SetRenderDrawColor(renderer, 60, 110, 150, 255);
        SDL_RenderClear(renderer);
        CHECK(layer.draw(renderer, placement, p, s, bindings));
        SDL_FlushRenderer(renderer);
        const auto *src = static_cast<const std::uint8_t *>(target->pixels);
        std::vector<std::uint8_t> px(960u * 640u * 4u);
        for (int y = 0; y < 640; y++) {
            std::memcpy(&px[static_cast<std::size_t>(y) * 960 * 4],
                        src + static_cast<std::size_t>(y) * static_cast<std::size_t>(target->pitch),
                        960 * 4);
        }
        return px;
    };

    fe::HintLayer layer;
    const std::vector<std::uint8_t> drawn = frame(prompts, pad, layer);
    CHECK(layer.redraws() == 1);
    // The same drawing on the CPU, over the same colour.
    std::vector<std::uint8_t> cpu(960u * 640u * 4u);
    for (std::size_t i = 0; i < cpu.size(); i += 4) {
        cpu[i] = 60;
        cpu[i + 1] = 110;
        cpu[i + 2] = 150;
        cpu[i + 3] = 255;
    }
    fe::Canvas canvas(cpu.data(), 960, 640, 2);
    CHECK(fe::draw_prompts(canvas, prompts, pad, bindings) == 2);
    int worst = 0, changed = 0;
    for (std::size_t i = 0; i < cpu.size(); i++) {
        worst = std::max(worst, std::abs(static_cast<int>(drawn[i]) - static_cast<int>(cpu[i])));
        changed += (i % 4 != 3 && cpu[i] != (i % 4 == 0 ? 60 : i % 4 == 1 ? 110 : 150)) ? 1 : 0;
    }
    CHECK(changed > 0);
    CHECK(worst <= 2);

    // Drawn anew only when something changed.
    frame(prompts, pad, layer);
    CHECK(layer.redraws() == 1);
    fe::PromptStyle keys = pad;
    keys.device = PromptDevice::kKeyboard;
    frame(prompts, keys, layer);
    CHECK(layer.redraws() == 2);
    // No prompts: the image untouched.
    const std::vector<std::uint8_t> bare = frame({}, keys, layer);
    bool untouched = true;
    for (std::size_t i = 0; i < bare.size(); i += 4) {
        untouched = untouched && bare[i] == 60 && bare[i + 1] == 110 && bare[i + 2] == 150;
    }
    CHECK(untouched);

    layer.stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(target);
}
