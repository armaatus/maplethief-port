// The remap overlay (#70, ADR 0054) driven by synthetic SDL events, and the button glyphs on
// SDL3 virtual joysticks: F1 opens it, Down selects Attack, Enter waits, K binds, Save applies
// and writes the bindings file, which reads back; Cancel and Esc change nothing; Reset; a pad
// button and a trigger rebinding; the 5 s timeout; the overlay keeps input from the game; Guide
// and L3 + R3 open it. The glyph set follows the active pad (Xbox 360, PS5, Switch Pro) and
// changes on hot-plug; Nintendo's face buttons by label; and a capture of the overlay per set,
// written to the test output directory and checked by hash.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "canvas.h"
#include "gamepad.h"
#include "glyphs.h"
#include "input.h"
#include "maplethief/input.h"
#include "remap.h"

#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::Context;
using fe::Control;
using fe::ControlKind;
using virtual_pad::Kind;

std::vector<std::string> g_sent;
void record(void *, std::uint32_t action, bool down) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    g_sent.push_back(std::string(down ? "+" : "-") + (info != nullptr ? info->name : "?"));
}
std::int32_t field_panel(void *) { return 0; }

SDL_Event key_event(SDL_Keycode k, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}
SDL_Event button_event(SDL_JoystickID which, SDL_GamepadButton b, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = which;
    e.gbutton.button = static_cast<Uint8>(b);
    e.gbutton.down = down;
    return e;
}
SDL_Event axis_event(SDL_JoystickID which, SDL_GamepadAxis a, Sint16 v) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = which;
    e.gaxis.axis = static_cast<Uint8>(a);
    e.gaxis.value = v;
    return e;
}

// The frontend's routing (game.cpp's handle_event): the overlay first, the devices only with
// what it does not take, then the glyph tracking.
struct Front {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Remap remap{&actions, &gamepads};
    double now = 100.0;

    Front() {
        g_sent.clear();
        actions.set_sink(record, nullptr);
        actions.set_panel_source(field_panel, nullptr);
    }
    void send(const SDL_Event &e) {
        if (!remap.handle(e, now)) {
            input.handle(e);
            gamepads.handle(e);
            if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
                actions.control(fe::InputSource::kKeyboard,
                                Control{ControlKind::kKey, static_cast<int>(e.key.key)},
                                e.type == SDL_EVENT_KEY_DOWN);
            }
        }
        remap.track(e);
    }
    void press(SDL_Keycode k) {
        send(key_event(k, true));
        send(key_event(k, false));
    }
    // SDL's pending events through send().
    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            send(e);
        }
    }
    const fe::Remap::Row &row() const { return remap.rows()[remap.selected()]; }
    // Down until the selected row is `kind` (and, for an action, `action` in `context`).
    void select(fe::Remap::RowKind kind, std::uint32_t action = MT_ACTION_NONE,
                Context context = Context::kField) {
        for (std::size_t i = 0; i < remap.rows().size() + 1; i++) {
            const fe::Remap::Row &r = row();
            if (r.kind == kind && (kind != fe::Remap::RowKind::kAction ||
                                   (r.slot.action == action && r.slot.context == context))) {
                return;
            }
            press(SDLK_DOWN);
        }
        FAIL("no such row");
    }
};

struct TempDir {
    fs::path path;
    explicit TempDir(const char *name) {
        path = fs::temp_directory_path() / (std::string("mt_remap_") + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Control key(SDL_Keycode k) { return Control{ControlKind::kKey, static_cast<int>(k)}; }
Control button(SDL_GamepadButton b) { return Control{ControlKind::kGamepadButton, b}; }

// FNV-1a, 64 bits.
std::uint64_t fnv1a(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::uint8_t b : bytes) {
        h = (h ^ b) * 0x100000001B3ull;
    }
    return h;
}

// A synthetic screen behind the overlay: bands of colour, no game art.
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

TEST_CASE("remap: open with F1, select Attack, press K, Save: applied and written") {
    TempDir dir("save");
    Front f;
    const std::string path = fe::bindings_path(dir.path.string(), "");
    f.remap.set_path(path);
    CHECK_FALSE(f.remap.is_open());
    // Z attacks before.
    f.press(SDLK_Z);
    CHECK(g_sent == std::vector<std::string>{"+attack", "-attack"});
    g_sent.clear();

    f.send(key_event(SDLK_F1, true));
    REQUIRE(f.remap.is_open());
    f.send(key_event(SDLK_F1, false));
    CHECK(f.row().kind == fe::Remap::RowKind::kAction); // the first action, not the header
    CHECK(f.row().slot.action == MT_ACTION_UP);
    f.select(fe::Remap::RowKind::kAction, MT_ACTION_ATTACK, Context::kField);
    f.press(SDLK_RETURN);
    CHECK(f.remap.capturing());
    CHECK(f.remap.status().find("Press a key or a gamepad button for Attack in the field") == 0);
    f.send(key_event(SDLK_K, true));
    CHECK_FALSE(f.remap.capturing());
    f.send(key_event(SDLK_K, false));
    CHECK(f.remap.status() == "Attack in the field: key K");
    // While open nothing reached the game, and the bindings in effect are still the old ones.
    CHECK(g_sent.empty());
    CHECK(f.actions.action_for(key(SDLK_Z), Context::kField) == MT_ACTION_ATTACK);
    CHECK_FALSE(fs::exists(path));

    f.select(fe::Remap::RowKind::kSave);
    f.press(SDLK_RETURN);
    CHECK_FALSE(f.remap.is_open());
    CHECK(f.remap.saves() == 1);
    // The saved file.
    REQUIRE(fs::exists(path));
    const std::string text = read(path);
    CHECK(text.find("[field]\n") != std::string::npos);
    CHECK(text.find("\nattack = button x, trigger righttrigger, key K\n") != std::string::npos);
    CHECK(text.find("\nnintendo_buttons = position\n") != std::string::npos);
    fe::BindingSet back;
    CHECK(fe::load_bindings(path, &back));
    CHECK(fe::same_bindings(back.table, f.actions.bindings()));
    // And in effect: K attacks, Z does not.
    f.press(SDLK_K);
    CHECK(g_sent == std::vector<std::string>{"+attack", "-attack"});
    g_sent.clear();
    f.press(SDLK_Z);
    CHECK(g_sent.empty());
}

TEST_CASE("remap: Cancel, Esc and F1 close it and change nothing; Reset restores the defaults") {
    TempDir dir("cancel");
    Front f;
    const std::string path = fe::bindings_path(dir.path.string(), "");
    f.remap.set_path(path);
    const std::vector<fe::Binding> before = f.actions.bindings();
    for (const SDL_Keycode closer : {SDLK_ESCAPE, SDLK_F1, SDLK_UNKNOWN}) {
        f.press(SDLK_F1);
        REQUIRE(f.remap.is_open());
        f.select(fe::Remap::RowKind::kAction, MT_ACTION_JUMP, Context::kField);
        f.press(SDLK_RETURN);
        f.press(SDLK_J);
        CHECK(fe::controls_for(f.remap.edit().table, Context::kField, MT_ACTION_JUMP).back() ==
              key(SDLK_J));
        if (closer == SDLK_UNKNOWN) {
            f.select(fe::Remap::RowKind::kCancel);
            f.press(SDLK_RETURN);
        } else {
            f.press(closer);
        }
        CHECK_FALSE(f.remap.is_open());
        CHECK(fe::same_bindings(f.actions.bindings(), before));
        CHECK_FALSE(fs::exists(path));
    }
    // Reset, then Save: the defaults written.
    fe::set_controls(&f.actions.bindings(), Context::kField, MT_ACTION_ATTACK, {key(SDLK_K)});
    f.press(SDLK_F1);
    CHECK(fe::controls_for(f.remap.edit().table, Context::kField, MT_ACTION_ATTACK) ==
          std::vector<Control>{key(SDLK_K)});
    f.select(fe::Remap::RowKind::kReset);
    f.press(SDLK_RETURN);
    CHECK(fe::same_bindings(f.remap.edit().table, fe::default_bindings()));
    f.select(fe::Remap::RowKind::kSave);
    f.press(SDLK_RETURN);
    CHECK(fe::same_bindings(f.actions.bindings(), fe::default_bindings()));
    CHECK(read(path) == fe::serialise_bindings(fe::BindingSet{}));
}

TEST_CASE("remap: a capture by gamepad button, trigger or d-pad; F1 and the timeout stop it") {
    Front f;
    f.remap.set_path("");
    f.press(SDLK_F1);
    // Quick slot 1 by the right shoulder: its pad control (North) goes, its keys stay.
    f.select(fe::Remap::RowKind::kAction, MT_ACTION_SLOT1, Context::kField);
    f.press(SDLK_RETURN);
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true));
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false));
    CHECK(f.remap.status() ==
          "Quick slot 1 in the field: button rightshoulder, taken from Quick slot 4 #. Warning: "
          "slot4 has no gamepad control in the field");
    CHECK(
        fe::controls_for(f.remap.edit().table, Context::kField, MT_ACTION_SLOT1) ==
        std::vector<Control>{button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), key(SDLK_A), key(SDLK_1)});
    // Quick slot 4 by the left trigger (it was the cash shop's).
    f.select(fe::Remap::RowKind::kAction, MT_ACTION_SLOT4, Context::kField);
    f.press(SDLK_RETURN);
    f.send(axis_event(7, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 30000));
    f.send(axis_event(7, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0));
    CHECK(fe::controls_for(f.remap.edit().table, Context::kField, MT_ACTION_SLOT4).front() ==
          Control{ControlKind::kGamepadTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER});
    // Jump by the d-pad's up.
    f.select(fe::Remap::RowKind::kAction, MT_ACTION_JUMP, Context::kField);
    f.press(SDLK_RETURN);
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_DPAD_UP, true));
    CHECK(fe::controls_for(f.remap.edit().table, Context::kField, MT_ACTION_JUMP).front() ==
          Control{ControlKind::kDirection, static_cast<int>(fe::Direction::kUp)});
    // F1 stops waiting; Guide too; so does the timeout.
    const std::vector<fe::Binding> table = f.remap.edit().table;
    f.press(SDLK_RETURN);
    REQUIRE(f.remap.capturing());
    f.press(SDLK_F1);
    CHECK_FALSE(f.remap.capturing());
    CHECK(f.remap.is_open());
    f.press(SDLK_RETURN);
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_GUIDE, true));
    CHECK_FALSE(f.remap.capturing());
    f.press(SDLK_RETURN);
    f.remap.tick(f.now + 4.9);
    CHECK(f.remap.capturing());
    f.remap.tick(f.now + fe::Remap::kCaptureSeconds);
    CHECK_FALSE(f.remap.capturing());
    CHECK(f.remap.status() == "No key or button: nothing changed");
    CHECK(fe::same_bindings(f.remap.edit().table, table));
    // A key's repeat does not bind: only a fresh press.
    f.press(SDLK_RETURN);
    f.send(key_event(SDLK_RETURN, true, true));
    CHECK(f.remap.capturing());
    f.remap.cancel();
}

TEST_CASE("remap: while open the game gets no key, button or touch; the pad moves and chooses") {
    Front f;
    f.press(SDLK_F1);
    REQUIRE(f.remap.is_open());
    for (const SDL_Keycode k : {SDLK_Z, SDLK_X, SDLK_A, SDLK_M}) {
        f.press(k);
    }
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_WEST, true));
    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.button.button = SDL_BUTTON_LEFT;
    CHECK(f.remap.handle(click, f.now));
    CHECK(g_sent.empty());
    // The pad: d-pad down, the stick, South chooses (a capture), East cancels.
    const std::size_t first = f.remap.selected();
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true));
    CHECK(f.remap.selected() == first + 1);
    f.send(axis_event(7, SDL_GAMEPAD_AXIS_LEFTY, 30000));
    f.send(axis_event(7, SDL_GAMEPAD_AXIS_LEFTY, 31000)); // held: one step
    CHECK(f.remap.selected() == first + 2);
    f.send(axis_event(7, SDL_GAMEPAD_AXIS_LEFTY, 0));
    f.send(axis_event(7, SDL_GAMEPAD_AXIS_LEFTY, -30000));
    CHECK(f.remap.selected() == first + 1);
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_SOUTH, true));
    CHECK(f.remap.capturing());
    f.remap.tick(f.now + 10);
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_EAST, true));
    CHECK_FALSE(f.remap.is_open());
    // Guide opens it too, and closes it.
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_GUIDE, true));
    CHECK(f.remap.is_open());
    f.send(button_event(7, SDL_GAMEPAD_BUTTON_GUIDE, true));
    CHECK_FALSE(f.remap.is_open());
    // Closed, keys reach the game again.
    f.press(SDLK_Z);
    CHECK(g_sent == std::vector<std::string>{"+attack", "-attack"});
}

TEST_CASE("remap: opening lifts what the game holds; the Nintendo row toggles the face buttons") {
    Front f;
    f.send(key_event(SDLK_RIGHT, true));
    CHECK(g_sent == std::vector<std::string>{"+right"});
    CHECK(f.actions.held() == 1);
    f.press(SDLK_F1);
    CHECK(f.actions.held() == 0);
    CHECK(g_sent == std::vector<std::string>{"+right", "-right"});
    f.select(fe::Remap::RowKind::kFace);
    CHECK(f.remap.edit().face == fe::FaceButtons::kPosition);
    f.press(SDLK_RIGHT);
    CHECK(f.remap.edit().face == fe::FaceButtons::kLabel);
    f.press(SDLK_RETURN);
    CHECK(f.remap.edit().face == fe::FaceButtons::kPosition);
    f.press(SDLK_LEFT);
    f.select(fe::Remap::RowKind::kSave);
    f.press(SDLK_RETURN); // no file: it stays open, but the new bindings apply
    CHECK(f.remap.is_open());
    CHECK(f.remap.status().find("Could not write") == 0);
    CHECK(f.gamepads.face_buttons() == fe::FaceButtons::kLabel);
    f.press(SDLK_ESCAPE);
    CHECK_FALSE(f.remap.is_open());
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

} // namespace

TEST_CASE("glyphs: the set follows the active pad, and changes when a pad connects or goes") {
    Sdl sdl;
    Front f;
    f.pump();
    CHECK(f.remap.glyphs().pad() == 0);
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kXbox);

    auto xbox = std::make_unique<virtual_pad::Pad>(Kind::kXbox360);
    REQUIRE_MESSAGE(xbox->ok(), xbox->error);
    f.pump();
    CHECK(SDL_GetGamepadTypeForID(xbox->id) == SDL_GAMEPAD_TYPE_XBOX360);
    CHECK(f.remap.glyphs().pad() == xbox->id);
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kXbox);

    // A PS5 pad plugged in: its set at once.
    auto ps5 = std::make_unique<virtual_pad::Pad>(Kind::kPs5, true);
    REQUIRE_MESSAGE(ps5->ok(), ps5->error);
    f.pump();
    CHECK(SDL_GetGamepadTypeForID(ps5->id) == SDL_GAMEPAD_TYPE_PS5);
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kPlayStation);

    // A press on the Xbox pad makes it the active one: its set again.
    xbox->button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    f.pump();
    CHECK(f.gamepads.active_id() == xbox->id);
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kXbox);
    // A press on the PS5 pad while the Xbox pad holds A: Gamepads ignores it, so do the glyphs.
    ps5->button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    f.pump();
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kXbox);
    ps5->button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    xbox->button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    f.pump();

    // A Switch Pro pad: Nintendo.
    auto sw = std::make_unique<virtual_pad::Pad>(Kind::kSwitchPro);
    REQUIRE_MESSAGE(sw->ok(), sw->error);
    f.pump();
    CHECK(SDL_GetGamepadTypeForID(sw->id) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO);
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kNintendo);
    // It goes: back to the active pad (Xbox). The Xbox pad goes: the last one left (PS5). The
    // PS5 goes: none, the Xbox glyphs.
    sw.reset();
    f.pump();
    CHECK(f.remap.glyphs().pad() == xbox->id);
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kXbox);
    xbox.reset();
    f.pump();
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kPlayStation);
    ps5.reset();
    f.pump();
    CHECK(f.remap.glyphs().pad() == 0);
    CHECK(f.remap.glyphs().set() == fe::GlyphSet::kXbox);
    CHECK(f.remap.glyphs().changes() >= 6);
}

TEST_CASE("glyphs: each set's labels; Nintendo's face buttons by position and by label") {
    const auto label = [](const Control &c, SDL_GamepadType t,
                          fe::FaceButtons face = fe::FaceButtons::kPosition) {
        return fe::pad_glyph_label(c, t, face);
    };
    const Control S = button(SDL_GAMEPAD_BUTTON_SOUTH), E = button(SDL_GAMEPAD_BUTTON_EAST),
                  W = button(SDL_GAMEPAD_BUTTON_WEST), N = button(SDL_GAMEPAD_BUTTON_NORTH);
    const Control lb = button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const Control rt{ControlKind::kGamepadTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
    const Control back = button(SDL_GAMEPAD_BUTTON_BACK), start = button(SDL_GAMEPAD_BUTTON_START);
    const SDL_GamepadType x = SDL_GAMEPAD_TYPE_XBOXONE, ps = SDL_GAMEPAD_TYPE_PS5,
                          ns = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    CHECK(label(S, x) == "A");
    CHECK(label(E, x) == "B");
    CHECK(label(W, x) == "X");
    CHECK(label(N, x) == "Y");
    CHECK(label(lb, x) == "LB");
    CHECK(label(rt, x) == "RT");
    CHECK(label(back, x) == "View");
    CHECK(label(start, x) == "Menu");
    CHECK(label(back, SDL_GAMEPAD_TYPE_XBOX360) == "Back");
    CHECK(label(S, ps) == "Cross");
    CHECK(label(E, ps) == "Circle");
    CHECK(label(W, ps) == "Square");
    CHECK(label(N, ps) == "Triangle");
    CHECK(label(lb, ps) == "L1");
    CHECK(label(rt, ps) == "R2");
    CHECK(label(back, ps) == "Create");
    CHECK(label(back, SDL_GAMEPAD_TYPE_PS4) == "Share");
    CHECK(label(start, ps) == "Options");
    // Nintendo by position: the bottom button is B.
    CHECK(label(S, ns) == "B");
    CHECK(label(E, ns) == "A");
    CHECK(label(W, ns) == "Y");
    CHECK(label(N, ns) == "X");
    CHECK(label(lb, ns) == "L");
    CHECK(label(rt, ns) == "ZR");
    CHECK(label(back, ns) == "-");
    CHECK(label(start, ns) == "+");
    // By label: South is the A button (on the right).
    CHECK(label(S, ns, fe::FaceButtons::kLabel) == "A");
    CHECK(label(E, ns, fe::FaceButtons::kLabel) == "B");
    CHECK(label(W, ns, fe::FaceButtons::kLabel) == "X");
    CHECK(label(N, ns, fe::FaceButtons::kLabel) == "Y");
    CHECK(label(S, ps, fe::FaceButtons::kLabel) == "Cross");
    CHECK(label(Control{ControlKind::kDirection, 2}, x) == "D-pad left");
    CHECK(fe::key_label(SDLK_LCTRL) == "LCtrl");
    CHECK(fe::key_label(SDLK_RETURN) == "Enter");
    CHECK(fe::key_label(SDLK_Z) == "Z");
    CHECK(fe::glyph_set_for(SDL_GAMEPAD_TYPE_STEAM) == fe::GlyphSet::kXbox);
    CHECK(fe::glyph_set_for(SDL_GAMEPAD_TYPE_PS3) == fe::GlyphSet::kPlayStation);
    CHECK(fe::glyph_set_for(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) ==
          fe::GlyphSet::kNintendo);
}

TEST_CASE("glyphs: a Switch Pro pad's face buttons play by position or by label") {
    Sdl sdl;
    Front f;
    virtual_pad::Pad sw(Kind::kSwitchPro);
    REQUIRE_MESSAGE(sw.ok(), sw.error);
    f.pump();
    REQUIRE(f.gamepads.count() == 1);
    const auto tap = [&](SDL_GamepadButton b) {
        g_sent.clear();
        sw.button(b, true);
        f.pump();
        sw.button(b, false);
        f.pump();
        return g_sent;
    };
    // By position (the default): the bottom button (B) jumps, as an Xbox pad's A.
    CHECK(tap(SDL_GAMEPAD_BUTTON_SOUTH) == std::vector<std::string>{"+jump", "-jump"});
    CHECK(tap(SDL_GAMEPAD_BUTTON_EAST) == std::vector<std::string>{"+slot2", "-slot2"});
    // By label: the A button (East) jumps, B (South) is quick slot 2, X (North) attacks.
    f.gamepads.set_face_buttons(fe::FaceButtons::kLabel);
    CHECK(tap(SDL_GAMEPAD_BUTTON_EAST) == std::vector<std::string>{"+jump", "-jump"});
    CHECK(tap(SDL_GAMEPAD_BUTTON_SOUTH) == std::vector<std::string>{"+slot2", "-slot2"});
    CHECK(tap(SDL_GAMEPAD_BUTTON_NORTH) == std::vector<std::string>{"+attack", "-attack"});
    CHECK(tap(SDL_GAMEPAD_BUTTON_WEST) == std::vector<std::string>{"+slot1", "-slot1"});
    // The overlay by label too: A (East) chooses, B (South) cancels.
    f.press(SDLK_F1);
    REQUIRE(f.remap.is_open());
    f.select(fe::Remap::RowKind::kFace);
    f.press(SDLK_RIGHT); // the overlay's own copy: by label
    sw.button(SDL_GAMEPAD_BUTTON_EAST, true);
    f.pump();
    CHECK(f.remap.edit().face == fe::FaceButtons::kPosition); // A chose the row: toggled back
    sw.button(SDL_GAMEPAD_BUTTON_EAST, false);
    f.pump();
    sw.button(SDL_GAMEPAD_BUTTON_EAST, true); // by position now: East is B, cancel
    f.pump();
    CHECK_FALSE(f.remap.is_open());
}

TEST_CASE("remap: L3 + R3 on a pad opens the overlay; neither alone does") {
    Sdl sdl;
    Front f;
    virtual_pad::Pad pad(Kind::kXbox360);
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    f.pump();
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
    f.pump();
    CHECK_FALSE(f.remap.is_open());
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, false);
    pad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    f.pump();
    CHECK_FALSE(f.remap.is_open());
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
    f.pump();
    CHECK(f.remap.is_open());
    CHECK(g_sent.empty());
    f.press(SDLK_ESCAPE);
}

TEST_CASE("remap: a capture of the overlay per glyph set, by hash") {
    Sdl sdl;
    const fs::path out = fs::path(output_dir()) / "remap_overlay";
    std::error_code ec;
    fs::create_directories(out, ec);
    struct Case {
        Kind kind;
        bool mapping;
        const char *name;
        fe::GlyphSet set;
        std::uint64_t hash;
    };
    // The overlay is the port's own drawing (integer arithmetic only), the same bytes everywhere.
    const Case cases[] = {
        {Kind::kXbox360, false, "xbox", fe::GlyphSet::kXbox, 0xeda8b89adbed3564ull},
        {Kind::kPs5, true, "playstation", fe::GlyphSet::kPlayStation, 0x2edc56f3d96dfa17ull},
        {Kind::kSwitchPro, false, "nintendo", fe::GlyphSet::kNintendo, 0xb60f347e1fce5184ull},
    };
    for (const Case &k : cases) {
        CAPTURE(k.name);
        Front f;
        f.remap.set_path("/data/bindings.ini");
        virtual_pad::Pad pad(k.kind, k.mapping);
        REQUIRE_MESSAGE(pad.ok(), pad.error);
        f.pump();
        REQUIRE(f.remap.glyphs().set() == k.set);
        f.press(SDLK_F1);
        f.select(fe::Remap::RowKind::kAction, MT_ACTION_ATTACK, Context::kField);
        f.press(SDLK_RETURN);
        f.send(button_event(pad.id, SDL_GAMEPAD_BUTTON_LEFT_STICK, true));
        f.send(button_event(pad.id, SDL_GAMEPAD_BUTTON_LEFT_STICK, false));
        f.press(SDLK_UP); // Jump selected; Attack's new control shows below it
        std::vector<std::uint8_t> px = backdrop(480, 320);
        f.remap.draw(px.data(), 480, 320, 1);
        const std::uint64_t h = fnv1a(px);
        char hex[32];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        const fs::path png = out / (std::string(k.name) + ".png");
        CHECK(write_png(png, px, 480, 320));
        MESSAGE(k.name << ": " << png.string() << " hash " << hex);
        CHECK(h == k.hash);
        // At render scale 2 (a 960 x 640 screen): the same layout, finer shapes.
        std::vector<std::uint8_t> px2 = backdrop(960, 640);
        f.remap.draw(px2.data(), 960, 640, 2);
        CHECK(write_png(out / (std::string(k.name) + "_2x.png"), px2, 960, 640));
        f.press(SDLK_ESCAPE);
    }
}

// With GPU presentation (ADR 0068's addendum) the overlay is drawn over UIKit's premultiplied
// layer, which the GPU then puts over the GL frame. Canvas blends "over" there too, so that
// equals drawing it over the composite (the CPU path) up to rounding, and over an opaque image
// it is the same bytes as before (the hashes above).
TEST_CASE("remap: drawn over a premultiplied layer, then composited, as over the composite") {
    constexpr std::uint32_t w = 480, h = 320;
    const std::vector<std::uint8_t> gl = backdrop(w, h);        // the GL frame (opaque)
    std::vector<std::uint8_t> layer(std::size_t{w} * h * 4, 0); // UIKit's: transparent slot
    for (std::uint32_t y = 0; y < h; y++) {
        for (std::uint32_t x = 0; x < w; x++) {
            std::uint8_t *p = &layer[(std::size_t{y} * w + x) * 4];
            if (y < 40) { // an opaque bar
                p[0] = 200, p[1] = 30, p[2] = 30, p[3] = 255;
            } else if (y > 250) { // a translucent panel (premultiplied: 0, 0, 255 at 128)
                p[0] = 0, p[1] = 0, p[2] = 128, p[3] = 128;
            }
        }
    }
    // "over" as the GPU blends a premultiplied layer: s + d * (255 - a) / 255.
    const auto over = [&](const std::vector<std::uint8_t> &top) {
        std::vector<std::uint8_t> out(gl.size());
        for (std::size_t i = 0; i < gl.size(); i += 4) {
            for (int c = 0; c < 3; c++) {
                out[i + c] = static_cast<std::uint8_t>(
                    std::min(255, top[i + c] + (gl[i + c] * (255 - top[i + 3]) + 127) / 255));
            }
            out[i + 3] = 255;
        }
        return out;
    };
    fe::Remap remap(nullptr, nullptr);
    remap.open(0);
    std::vector<std::uint8_t> cpu = over(layer); // the CPU path: the composite, then the overlay
    remap.draw(cpu.data(), w, h, 1);
    std::vector<std::uint8_t> gpu = layer; // the GPU path: the overlay into the layer, then over
    remap.draw(gpu.data(), w, h, 1);
    const std::vector<std::uint8_t> shown = over(gpu);
    int max_delta = 0;
    for (std::size_t i = 0; i < cpu.size(); i++) {
        max_delta = std::max(max_delta, std::abs(static_cast<int>(cpu[i]) - shown[i]));
    }
    CHECK(max_delta <= 2);
    CHECK(shown != over(layer)); // the overlay shows
}
