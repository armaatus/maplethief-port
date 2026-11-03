// The keyboard (#68, ADR 0048's addendum) with synthetic SDL key events: the default rows
// against docs/controls/keyboard.md's table, every key in the field and in a menu, the menu keys
// against the gamepad's, every action reachable, repeats, system shortcuts, focus loss, a release
// across a context change, the activity listener, and the runtime's queue behind the default
// sink.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "actions.h"
#include "input.h"
#include "keyboard.h"
#include "maplethief/input.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using Lines = std::vector<std::string>;

std::string action_name(u32 action) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    return info != nullptr ? info->name : "none";
}

// "+jump", "-jump": what Actions sent to the runtime.
Lines g_sent;
i32 g_panel = 0;
void record(void *, u32 action, bool down) {
    g_sent.push_back((down ? "+" : "-") + action_name(action));
}
i32 panel(void *) { return g_panel; }

std::vector<fe::InputSource> g_activity;
void activity(void *, fe::InputSource source) { g_activity.push_back(source); }

SDL_Event key_event(SDL_Keycode key, bool down, bool repeat = false, SDL_Keymod mod = 0) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.down = down;
    e.key.repeat = repeat;
    e.key.mod = mod;
    return e;
}

// Input (for the activity listener), Actions into the recording sink on panel g_panel, and
// Keyboard; every event goes to Input and Keyboard, as game.cpp hands it over.
struct Keys {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Keyboard keyboard{&actions};

    Keys() {
        g_sent.clear();
        g_activity.clear();
        g_panel = 0;
        actions.set_sink(record, nullptr);
        actions.set_panel_source(panel, nullptr);
        input.set_activity_listener(activity, nullptr);
    }
    void handle(const SDL_Event &e) {
        input.handle(e);
        keyboard.handle(e);
    }
    Lines sent(const SDL_Event &e) {
        g_sent.clear();
        handle(e);
        return g_sent;
    }
    Lines down(SDL_Keycode k) { return sent(key_event(k, true)); }
    Lines up(SDL_Keycode k) { return sent(key_event(k, false)); }
};

fe::Control key(SDL_Keycode k) { return fe::Control{fe::ControlKind::kKey, static_cast<int>(k)}; }

// docs/controls/keyboard.md's table between its keyboard-defaults markers: "Key|field|menu".
std::vector<std::string> doc_rows() {
    std::ifstream in(std::string(MT_REPO_DIR) + "/docs/controls/keyboard.md");
    std::vector<std::string> rows;
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (line.find("BEGIN keyboard-defaults") != std::string::npos) {
            inside = true;
        } else if (line.find("END keyboard-defaults") != std::string::npos) {
            inside = false;
        } else if (inside && line.rfind("| `", 0) == 0) {
            // | `Left Alt` | jump | — |
            std::vector<std::string> cells;
            std::stringstream ss(line.substr(1));
            std::string cell;
            while (std::getline(ss, cell, '|')) {
                const auto b = cell.find_first_not_of(" `");
                const auto e = cell.find_last_not_of(" `");
                cells.push_back(b == std::string::npos ? "" : cell.substr(b, e - b + 1));
            }
            REQUIRE(cells.size() >= 3);
            const auto name = [](const std::string &c) { return c == "—" ? "none" : c; };
            rows.push_back(cells[0] + "|" + name(cells[1]) + "|" + name(cells[2]));
        }
    }
    return rows;
}

} // namespace

TEST_CASE("keyboard: the default rows are docs/controls/keyboard.md's table") {
    std::vector<std::string> code;
    for (const fe::Binding &b : fe::default_keyboard_bindings()) {
        REQUIRE(b.control.kind == fe::ControlKind::kKey);
        code.push_back(std::string(SDL_GetKeyName(static_cast<SDL_Keycode>(b.control.code))) + "|" +
                       action_name(b.field) + "|" + action_name(b.menu));
    }
    const std::vector<std::string> doc = doc_rows();
    CHECK(doc.size() == 32);
    CHECK(code == doc);
    // Each key once; the frontend's table holds the gamepad's rows and then these.
    std::set<int> keys;
    for (const fe::Binding &b : fe::default_keyboard_bindings()) {
        CHECK_MESSAGE(keys.insert(b.control.code).second, fe::control_name(b.control));
    }
    const std::vector<fe::Binding> all = fe::default_bindings();
    CHECK(all.size() ==
          fe::default_gamepad_bindings().size() + fe::default_keyboard_bindings().size());
    fe::Input input;
    fe::Actions actions(&input);
    CHECK(actions.bindings().size() == all.size());
    CHECK(fe::control_name(key(SDLK_Z)) == "key Z");
    CHECK(fe::control_name(key(SDLK_LCTRL)) == "key Left Ctrl");
}

TEST_CASE("keyboard: every default key presses and releases its action, in the field and menus") {
    Keys k;
    for (const i32 p : {0, 1, 3, 5, 6, 7, 8, 9, -1}) {
        g_panel = p;
        const bool field = p == 0;
        CAPTURE(p);
        for (const fe::Binding &b : fe::default_keyboard_bindings()) {
            const SDL_Keycode code = static_cast<SDL_Keycode>(b.control.code);
            CAPTURE(SDL_GetKeyName(code));
            const u32 action = field ? b.field : b.menu;
            // The port's own (fast_forward, #151): held in Actions, never sent to the guest.
            if (action == MT_ACTION_NONE || fe::port_action(action)) {
                CHECK(k.down(code).empty());
                CHECK(k.up(code).empty());
            } else {
                CHECK(k.down(code) == Lines{"+" + action_name(action)});
                CHECK(k.up(code) == Lines{"-" + action_name(action)});
            }
        }
        CHECK(k.actions.held() == 0);
    }
    // The ones that matter most, spelled out (docs/controls/keyboard.md).
    g_panel = 0;
    const struct {
        SDL_Keycode key;
        const char *field, *menu;
    } spot[] = {
        {SDLK_LEFT, "left", "left"},
        {SDLK_RIGHT, "right", "right"},
        {SDLK_UP, "up", "up"},
        {SDLK_DOWN, "down", "down"},
        {SDLK_X, "jump", "back"},
        {SDLK_Z, "attack", "ok"},
        {SDLK_SPACE, "jump", "ok"},
        {SDLK_A, "slot1", nullptr},
        {SDLK_S, "slot2", nullptr},
        {SDLK_D, "slot3", nullptr},
        {SDLK_F, "slot4", nullptr},
        {SDLK_4, "slot4", nullptr},
        {SDLK_RETURN, "ok", "ok"},
        {SDLK_ESCAPE, "menu", "back"},
        {SDLK_M, "map", "map"},
        {SDLK_C, "cash_shop", nullptr},
        {SDLK_Q, nullptr, "minus"},
        {SDLK_E, nullptr, "plus"},
        {SDLK_R, "auto_run", "revive"},
        {SDLK_G, nullptr, "grade_revive"},
        {SDLK_N, nullptr, "slot4"},
        {SDLK_DELETE, nullptr, "map"},
        {SDLK_BACKSPACE, nullptr, "back"},
    };
    for (const auto &s : spot) {
        CAPTURE(SDL_GetKeyName(s.key));
        CHECK(action_name(k.actions.action_for(key(s.key), fe::Context::kField)) ==
              (s.field != nullptr ? s.field : "none"));
        CHECK(action_name(k.actions.action_for(key(s.key), fe::Context::kMenu)) ==
              (s.menu != nullptr ? s.menu : "none"));
    }
    // An unbound key does nothing, and nothing stays held from it.
    CHECK(k.down(SDLK_P).empty());
    CHECK(k.up(SDLK_P).empty());
    CHECK(k.actions.held() == 0);
}

TEST_CASE("keyboard: in menus confirm, back and the arrows are the gamepad's OK, back and d-pad") {
    fe::Input input;
    fe::Actions a(&input);
    const auto pad = [](SDL_GamepadButton b) {
        return fe::Control{fe::ControlKind::kGamepadButton, static_cast<int>(b)};
    };
    const auto dir = [](fe::Direction d) {
        return fe::Control{fe::ControlKind::kDirection, static_cast<int>(d)};
    };
    const fe::Context m = fe::Context::kMenu;
    const u32 ok = a.action_for(pad(SDL_GAMEPAD_BUTTON_SOUTH), m);
    const u32 back = a.action_for(pad(SDL_GAMEPAD_BUTTON_EAST), m);
    CHECK(ok == MT_ACTION_OK);
    CHECK(back == MT_ACTION_BACK);
    for (const SDL_Keycode k : {SDLK_RETURN, SDLK_KP_ENTER, SDLK_Z, SDLK_SPACE}) {
        CHECK(a.action_for(key(k), m) == ok);
    }
    for (const SDL_Keycode k : {SDLK_ESCAPE, SDLK_X, SDLK_BACKSPACE}) {
        CHECK(a.action_for(key(k), m) == back);
    }
    CHECK(a.action_for(key(SDLK_UP), m) == a.action_for(dir(fe::Direction::kUp), m));
    CHECK(a.action_for(key(SDLK_DOWN), m) == a.action_for(dir(fe::Direction::kDown), m));
    CHECK(a.action_for(key(SDLK_LEFT), m) == a.action_for(dir(fe::Direction::kLeft), m));
    CHECK(a.action_for(key(SDLK_RIGHT), m) == a.action_for(dir(fe::Direction::kRight), m));
    // Esc in the field is the gamepad's Start: the popup menu.
    CHECK(a.action_for(key(SDLK_ESCAPE), fe::Context::kField) ==
          a.action_for(pad(SDL_GAMEPAD_BUTTON_START), fe::Context::kField));
}

TEST_CASE("keyboard: every action has a key") {
    std::set<u32> reached;
    for (const fe::Binding &b : fe::default_keyboard_bindings()) {
        reached.insert(b.field);
        reached.insert(b.menu);
    }
    for (u32 a = 1; a < MT_ACTION_COUNT; a++) {
        CHECK_MESSAGE(reached.count(a) == 1, action_name(a));
    }
}

TEST_CASE("keyboard: repeats are one press, system shortcuts are not pressed") {
    Keys k;
    CHECK(k.down(SDLK_LEFT) == Lines{"+left"});
    for (int i = 0; i < 5; i++) {
        CHECK(k.sent(key_event(SDLK_LEFT, true, true)).empty());
    }
    CHECK(k.actions.held() == 1);
    CHECK(k.up(SDLK_LEFT) == Lines{"-left"});
    // A second down without an up (a lost key-up) is still one press.
    CHECK(k.down(SDLK_Z) == Lines{"+attack"});
    CHECK(k.down(SDLK_Z).empty());
    CHECK(k.up(SDLK_Z) == Lines{"-attack"});
    CHECK(k.up(SDLK_Z).empty());
    // Cmd (macOS) or the Windows key held: the system's, not the game's.
    for (const SDL_Keymod mod : std::initializer_list<SDL_Keymod>{SDL_KMOD_LGUI, SDL_KMOD_RGUI}) {
        CHECK(k.sent(key_event(SDLK_M, true, false, mod)).empty());
        CHECK(k.sent(key_event(SDLK_M, false, false, mod)).empty());
    }
    // Shift, Ctrl and Alt do not stop a key (Ctrl is attack: Ctrl held, X still jumps).
    CHECK(k.down(SDLK_LCTRL) == Lines{"+attack"});
    CHECK(k.sent(key_event(SDLK_X, true, false, SDL_KMOD_LCTRL)) == Lines{"+jump"});
    CHECK(k.up(SDLK_X) == Lines{"-jump"});
    CHECK(k.up(SDLK_LCTRL) == Lines{"-attack"});
    CHECK(k.actions.held() == 0);
}

TEST_CASE("keyboard: focus lost, hidden or minimised releases every held key") {
    for (const SDL_EventType type :
         {SDL_EVENT_WINDOW_FOCUS_LOST, SDL_EVENT_WINDOW_HIDDEN, SDL_EVENT_WINDOW_MINIMIZED}) {
        CAPTURE(static_cast<int>(type));
        Keys k;
        CHECK(k.down(SDLK_RIGHT) == Lines{"+right"});
        CHECK(k.down(SDLK_Z) == Lines{"+attack"});
        CHECK(k.down(SDLK_A) == Lines{"+slot1"});
        SDL_Event e{};
        e.type = type;
        Lines lifted = k.sent(e);
        std::sort(lifted.begin(), lifted.end());
        CHECK(lifted == Lines{"-attack", "-right", "-slot1"});
        CHECK(k.actions.held() == 0);
        // The keys still physically down: their repeats press nothing, their ups release
        // nothing; a new press counts.
        CHECK(k.sent(key_event(SDLK_RIGHT, true, true)).empty());
        CHECK(k.up(SDLK_RIGHT).empty());
        CHECK(k.up(SDLK_Z).empty());
        CHECK(k.down(SDLK_RIGHT) == Lines{"+right"});
        CHECK(k.up(SDLK_RIGHT) == Lines{"-right"});
        CHECK(k.up(SDLK_A).empty());
    }
}

TEST_CASE("keyboard: a release follows its press across a context change") {
    Keys k;
    CHECK(k.down(SDLK_X) == Lines{"+jump"});
    g_panel = 7; // a popup opens while X is held
    CHECK(k.up(SDLK_X) == Lines{"-jump"});
    CHECK(k.down(SDLK_X) == Lines{"+back"});
    g_panel = 0;
    CHECK(k.up(SDLK_X) == Lines{"-back"});
    // The help pages (panel 10): the arrows turn the page by a touch, not a key.
    g_panel = 10;
    CHECK(k.down(SDLK_RIGHT).empty());
    CHECK(k.actions.gestures() == 1);
    CHECK(k.up(SDLK_RIGHT).empty());
}

TEST_CASE("keyboard: presses reach the activity listener once, repeats and releases do not") {
    Keys k;
    k.handle(key_event(SDLK_Z, true));
    k.handle(key_event(SDLK_Z, true, true));
    k.handle(key_event(SDLK_Z, true, true));
    k.handle(key_event(SDLK_Z, false));
    k.handle(key_event(SDLK_P, true)); // unbound: still keyboard input (#71)
    k.handle(key_event(SDLK_P, false));
    CHECK(g_activity ==
          std::vector<fe::InputSource>{fe::InputSource::kKeyboard, fe::InputSource::kKeyboard});
    CHECK_FALSE(fe::is_touch_source(fe::InputSource::kKeyboard));
}

TEST_CASE("keyboard: the default sink is the runtime's action queue") {
    mt_input_configure(nullptr);
    fe::Input input;
    fe::Actions actions(&input);
    fe::Keyboard keyboard(&actions);
    CHECK(mt_input_get_stats().queued == 0);
    CHECK(keyboard.handle(key_event(SDLK_Z, true)));
    CHECK_FALSE(keyboard.handle(key_event(SDLK_Z, true, true)));
    CHECK(mt_input_get_stats().queued == 1); // delivered at the next game tick (ADR 0048)
    CHECK(keyboard.handle(key_event(SDLK_Z, false)));
    CHECK(mt_input_get_stats().queued == 2);
    mt_input_configure(nullptr);
}
