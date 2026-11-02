// The bindings file (#70, ADR 0054) with synthetic data only: SDL's names for keys, buttons and
// triggers; a good file over the defaults; bad lines reported by file and line and skipped
// (unknown names, malformed lines, sections, reserved controls); duplicates and conflicts; the
// warnings for an action left without a control; the round trip through the writer; the
// overrides applied to Actions (the new key presses the action, the old one no longer does);
// load and save through a temporary data directory; the face buttons by label.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "gamepad.h"
#include "maplethief/input.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::Context;
using fe::Control;
using fe::ControlKind;

Control key(SDL_Keycode k) { return Control{ControlKind::kKey, static_cast<int>(k)}; }
Control button(SDL_GamepadButton b) { return Control{ControlKind::kGamepadButton, b}; }
Control trigger(SDL_GamepadAxis a) { return Control{ControlKind::kGamepadTrigger, a}; }
Control dir(fe::Direction d) { return Control{ControlKind::kDirection, static_cast<int>(d)}; }

std::vector<Control> field(const fe::BindingSet &s, std::uint32_t action) {
    return fe::controls_for(s.table, Context::kField, action);
}
std::vector<Control> menu(const fe::BindingSet &s, std::uint32_t action) {
    return fe::controls_for(s.table, Context::kMenu, action);
}

bool has(const std::vector<std::string> &lines, const std::string &text) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string &l) { return l.find(text) != std::string::npos; });
}

// "+attack": what Actions sent.
std::vector<std::string> g_sent;
void record(void *, std::uint32_t action, bool down) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    g_sent.push_back(std::string(down ? "+" : "-") + (info != nullptr ? info->name : "?"));
}
std::int32_t g_panel = 0;
std::int32_t panel(void *) { return g_panel; }

// Down and up on Actions; what it sent.
std::vector<std::string> tap(fe::Actions &a, fe::InputSource source, const Control &c) {
    g_sent.clear();
    a.control(source, c, true);
    a.control(source, c, false);
    return g_sent;
}

struct TempDir {
    fs::path path;
    explicit TempDir(const char *name) {
        path = fs::temp_directory_path() / (std::string("mt_bindings_") + name);
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

} // namespace

TEST_CASE("bindings: controls by SDL's names, and back") {
    Control c;
    std::string why;
    REQUIRE(fe::parse_control("key Z", &c, &why));
    CHECK(c == key(SDLK_Z));
    REQUIRE(fe::parse_control("key z", &c, &why));
    CHECK(c == key(SDLK_Z));
    REQUIRE(fe::parse_control("KEY   Left Ctrl ", &c, &why));
    CHECK(c == key(SDLK_LCTRL));
    REQUIRE(fe::parse_control("key return", &c, &why));
    CHECK(c == key(SDLK_RETURN));
    REQUIRE(fe::parse_control("key F5", &c, &why));
    CHECK(c == key(SDLK_F5));
    REQUIRE(fe::parse_control("key Comma", &c, &why));
    CHECK(c == key(SDLK_COMMA));
    REQUIRE(fe::parse_control("button a", &c, &why));
    CHECK(c == button(SDL_GAMEPAD_BUTTON_SOUTH));
    REQUIRE(fe::parse_control("button West", &c, &why));
    CHECK(c == button(SDL_GAMEPAD_BUTTON_WEST));
    REQUIRE(fe::parse_control("button leftshoulder", &c, &why));
    CHECK(c == button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    REQUIRE(fe::parse_control("trigger righttrigger", &c, &why));
    CHECK(c == trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    REQUIRE(fe::parse_control("trigger left", &c, &why));
    CHECK(c == trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    REQUIRE(fe::parse_control("direction Up", &c, &why));
    CHECK(c == dir(fe::Direction::kUp));

    CHECK_FALSE(fe::parse_control("key Nonsense", &c, &why));
    CHECK(why == "unknown key 'Nonsense'");
    CHECK_FALSE(fe::parse_control("button z", &c, &why));
    CHECK(why == "unknown gamepad button 'z'");
    CHECK_FALSE(fe::parse_control("button dpup", &c, &why));
    CHECK(why.find("direction up") != std::string::npos);
    CHECK_FALSE(fe::parse_control("trigger leftx", &c, &why));
    CHECK_FALSE(fe::parse_control("direction north", &c, &why));
    CHECK_FALSE(fe::parse_control("mouse left", &c, &why));
    CHECK_FALSE(fe::parse_control("key", &c, &why));

    // Every default control's text reads back as itself.
    for (const fe::Binding &b : fe::default_bindings()) {
        CAPTURE(fe::control_text(b.control));
        Control back;
        REQUIRE(fe::parse_control(fe::control_text(b.control), &back, &why));
        CHECK(back == b.control);
    }
    CHECK(fe::control_text(key(SDLK_LCTRL)) == "key Left Ctrl");
    CHECK(fe::control_text(button(SDL_GAMEPAD_BUTTON_SOUTH)) == "button a");
    CHECK(fe::control_text(trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) == "trigger righttrigger");
    CHECK(fe::control_text(key(SDLK_COMMA)) == "key Comma");
    CHECK(fe::reserved_control(key(SDLK_F1)));
    CHECK(fe::reserved_control(button(SDL_GAMEPAD_BUTTON_GUIDE)));
    CHECK_FALSE(fe::reserved_control(key(SDLK_F2)));
}

TEST_CASE("bindings: a good file overrides rows of the defaults, the rest stay") {
    fe::BindingSet s;
    const std::string text = "\xEF\xBB\xBF# remapped\r\n"
                             "nintendo_buttons = Label\r\n"
                             "\r\n"
                             "[field]\r\n"
                             "  Attack = key K , button leftstick\r\n"
                             "jump = key Space, button a\n"
                             "[MENU]\n"
                             "ok = key Return, button a\n";
    const std::vector<std::string> problems = fe::parse_bindings(text, "bindings.ini", &s);
    CAPTURE(problems.size());
    for (const std::string &p : problems) {
        MESSAGE(p);
    }
    // jump lost X and the Alts (their keys), ok lost Space and Z in menus: no device is left
    // without a control, so no warning.
    CHECK(problems.empty());
    CHECK(s.face == fe::FaceButtons::kLabel);
    CHECK(field(s, MT_ACTION_ATTACK) ==
          std::vector<Control>{key(SDLK_K), button(SDL_GAMEPAD_BUTTON_LEFT_STICK)});
    CHECK(field(s, MT_ACTION_JUMP) ==
          std::vector<Control>{button(SDL_GAMEPAD_BUTTON_SOUTH), key(SDLK_SPACE)});
    CHECK(menu(s, MT_ACTION_OK) ==
          std::vector<Control>{button(SDL_GAMEPAD_BUTTON_SOUTH), key(SDLK_RETURN)});
    // Untouched actions keep their defaults: quick slot 1, menus' back.
    const fe::BindingSet d;
    CHECK(field(s, MT_ACTION_SLOT1) == field(d, MT_ACTION_SLOT1));
    CHECK(menu(s, MT_ACTION_BACK) == menu(d, MT_ACTION_BACK));
    // Z, X, West and the right trigger do nothing in the field now; Z is still nothing in menus
    // (ok gave it up), X is still back in menus.
    fe::Actions a(nullptr);
    a.bindings() = s.table;
    CHECK(a.action_for(key(SDLK_Z), Context::kField) == MT_ACTION_NONE);
    CHECK(a.action_for(key(SDLK_X), Context::kField) == MT_ACTION_NONE);
    CHECK(a.action_for(button(SDL_GAMEPAD_BUTTON_WEST), Context::kField) == MT_ACTION_NONE);
    CHECK(a.action_for(trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), Context::kField) == MT_ACTION_NONE);
    CHECK(a.action_for(key(SDLK_Z), Context::kMenu) == MT_ACTION_NONE);
    CHECK(a.action_for(key(SDLK_X), Context::kMenu) == MT_ACTION_BACK);
    CHECK(a.action_for(button(SDL_GAMEPAD_BUTTON_WEST), Context::kMenu) == MT_ACTION_REVIVE);
}

TEST_CASE("bindings: bad lines are reported by file and line and skipped, never fatal") {
    fe::BindingSet s;
    const std::string text = "attack = key K\n"                       // 1: before a section
                             "[field]\n"                              // 2
                             "atack = key K\n"                        // 3: unknown action
                             "jump = key Nonsense\n"                  // 4: no control left
                             "slot1 = key Q, button z, key W\n"       // 5: one bad control
                             "slot2 key E\n"                          // 6: no '='
                             "slot3 = button dpup\n"                  // 7: the d-pad
                             "slot4 = key F1, key T\n"                // 8: reserved
                             "map = key M, , key N\n"                 // 9: an empty control
                             "[options]\n"                            // 10: unknown section
                             "attack = key L\n"                       // 11: skipped with it
                             "[field\n"                               // 12: malformed
                             "nintendo_buttons = sideways\n"          // 13: bad value
                             "[menu]\n"                               // 14
                             "back = trigger leftx, key Backspace\n"; // 15: not a trigger
    const std::vector<std::string> p = fe::parse_bindings(text, "my.ini", &s);
    for (const std::string &l : p) {
        MESSAGE(l);
    }
    CHECK(has(p, "my.ini:1: 'attack' before [field] or [menu]: skipped"));
    CHECK(has(p, "my.ini:3: unknown action 'atack'"));
    CHECK(has(p, "my.ini:4: unknown key 'Nonsense': skipped"));
    CHECK(has(p, "my.ini:4: jump: no control left on the line"));
    CHECK(has(p, "my.ini:5: unknown gamepad button 'z': skipped"));
    CHECK(has(p, "my.ini:6: not 'action = controls'"));
    CHECK(has(p, "my.ini:7: the d-pad is 'direction up'"));
    CHECK(has(p, "my.ini:7: slot3: no control left on the line"));
    CHECK(has(p, "my.ini:8: key F1 opens the remap overlay and cannot be bound: skipped"));
    CHECK(has(p, "my.ini:9: an empty control"));
    CHECK(has(p, "my.ini:10: unknown section '[options]'"));
    CHECK(has(p, "my.ini:12: malformed section '[field'"));
    CHECK(has(p, "my.ini:13: nintendo_buttons must be position or label, not 'sideways'"));
    CHECK(has(p, "my.ini:15: unknown trigger 'leftx'"));
    CHECK_FALSE(has(p, "my.ini:11:"));
    // What was good still applies; what was bad kept its defaults.
    const fe::BindingSet d;
    CHECK(field(s, MT_ACTION_ATTACK) == field(d, MT_ACTION_ATTACK));
    CHECK(field(s, MT_ACTION_JUMP) == field(d, MT_ACTION_JUMP));
    CHECK(field(s, MT_ACTION_SLOT1) == std::vector<Control>{key(SDLK_Q), key(SDLK_W)});
    CHECK(field(s, MT_ACTION_SLOT3) == field(d, MT_ACTION_SLOT3));
    CHECK(field(s, MT_ACTION_SLOT4) == std::vector<Control>{key(SDLK_T)});
    CHECK(field(s, MT_ACTION_MAP) == std::vector<Control>{key(SDLK_M), key(SDLK_N)});
    CHECK(menu(s, MT_ACTION_BACK) == std::vector<Control>{key(SDLK_BACKSPACE)});
    CHECK(s.face == fe::FaceButtons::kPosition);
    // ... and every action that lost a device is warned about.
    CHECK(has(p, "my.ini: slot1 has no gamepad control in the field"));
    CHECK(has(p, "my.ini: slot4 has no gamepad control in the field"));
    CHECK(has(p, "my.ini: back has no gamepad control in menus"));

    // Nonsense of any kind is never fatal.
    fe::BindingSet junk;
    static const char k_junk[] = "\x00\xFF[\n=\n[field]\n=key\n\xC3\x28 = \x80\n";
    const std::string bytes(k_junk, sizeof k_junk - 1);
    CHECK_FALSE(fe::parse_bindings(bytes, "junk.ini", &junk).empty());
    CHECK(fe::same_bindings(junk.table, fe::default_bindings()));
    // A file over the limit is ignored whole.
    fe::BindingSet big;
    const std::string huge = "[field]\nattack = key K\n" + std::string(fe::kMaxBindingsBytes, '#');
    const std::vector<std::string> bp = fe::parse_bindings(huge, "big.ini", &big);
    REQUIRE(bp.size() == 1);
    CHECK(bp[0].find("ignored") != std::string::npos);
    CHECK(fe::same_bindings(big.table, fe::default_bindings()));
}

TEST_CASE("bindings: duplicates and conflicts: the earlier line wins, the later is reported") {
    fe::BindingSet s;
    const std::string text = "[field]\n"
                             "attack = key K, key K\n"   // 2: K twice on one line
                             "jump = key K, key J\n"     // 3: K is attack's (line 2)
                             "attack = key L\n"          // 4: attack again
                             "[menu]\n"                  // 5
                             "ok = key K\n"              // 6: another context: fine
                             "back = key K, button b\n"; // 7: K is ok's in menus
    const std::vector<std::string> p = fe::parse_bindings(text, "b.ini", &s);
    for (const std::string &l : p) {
        MESSAGE(l);
    }
    CHECK(has(p, "b.ini:2: key K is listed twice"));
    CHECK(has(p, "b.ini:3: key K is already bound to attack in the field on line 2: skipped here"));
    CHECK(has(p, "b.ini:4: attack in the field is already set on line 2: this line is skipped"));
    CHECK(has(p, "b.ini:7: key K is already bound to ok in menus on line 6: skipped here"));
    CHECK(field(s, MT_ACTION_ATTACK) == std::vector<Control>{key(SDLK_K)});
    CHECK(field(s, MT_ACTION_JUMP) == std::vector<Control>{key(SDLK_J)});
    CHECK(menu(s, MT_ACTION_OK) == std::vector<Control>{key(SDLK_K)});
    CHECK(menu(s, MT_ACTION_BACK) == std::vector<Control>{button(SDL_GAMEPAD_BUTTON_EAST)});
    // One control, one action per context.
    fe::Actions a(nullptr);
    a.bindings() = s.table;
    CHECK(a.action_for(key(SDLK_K), Context::kField) == MT_ACTION_ATTACK);
    CHECK(a.action_for(key(SDLK_K), Context::kMenu) == MT_ACTION_OK);
}

TEST_CASE("bindings: an action left without a control is always warned about") {
    fe::BindingSet s;
    const std::vector<std::string> p = fe::parse_bindings(
        "[field]\nattack = none\nslot1 = key Q\n[menu]\nback = button b\n", "w.ini", &s);
    for (const std::string &l : p) {
        MESSAGE(l);
    }
    CHECK(field(s, MT_ACTION_ATTACK).empty());
    CHECK(has(p, "w.ini: attack has no control in the field: it cannot be used there"));
    CHECK(has(p, "w.ini: slot1 has no gamepad control in the field"));
    CHECK(has(p, "w.ini: back has no key in menus"));
    CHECK(p.size() == 3);
    // Taking an action's only control with another line warns too.
    fe::BindingSet t;
    const std::vector<std::string> q =
        fe::parse_bindings("[field]\ncash_shop = key P\nmap = button lefttrigger\n", "t.ini", &t);
    CHECK(has(q, "t.ini:3: unknown gamepad button 'lefttrigger'"));
    fe::BindingSet u;
    const std::vector<std::string> r =
        fe::parse_bindings("[field]\nmap = key M, trigger lefttrigger, button back\n", "u.ini", &u);
    CHECK(has(r, "u.ini: cash_shop has no gamepad control in the field"));
}

TEST_CASE("bindings: write and read back: the same bindings") {
    // The defaults, then an edited set.
    const fe::BindingSet d;
    const std::string text = fe::serialise_bindings(d);
    CHECK(text.find("nintendo_buttons = position\n") != std::string::npos);
    CHECK(text.find("[field]\n") != std::string::npos);
    CHECK(text.find("[menu]\n") != std::string::npos);
    CHECK(text.find("attack = button x, trigger righttrigger, key Z, key Left Ctrl, key Right "
                    "Ctrl\n") != std::string::npos);
    CHECK(text.find("ok = button a, key Space, key Z, key Return, key Keypad Enter\n") !=
          std::string::npos);
    fe::BindingSet back;
    back.face = fe::FaceButtons::kLabel;
    CHECK(fe::parse_bindings(text, "d.ini", &back).empty());
    CHECK(back.face == fe::FaceButtons::kPosition);
    CHECK(fe::same_bindings(back.table, d.table));

    fe::BindingSet e;
    e.face = fe::FaceButtons::kLabel;
    fe::set_controls(&e.table, Context::kField, MT_ACTION_ATTACK,
                     {key(SDLK_COMMA), button(SDL_GAMEPAD_BUTTON_LEFT_STICK)});
    fe::set_controls(&e.table, Context::kField, MT_ACTION_SLOT4, {});
    fe::set_controls(&e.table, Context::kMenu, MT_ACTION_REVIVE, {key(SDLK_F5)});
    // An action without a default in that context, bound in the file, is written too.
    fe::set_controls(&e.table, Context::kField, MT_ACTION_BACK, {key(SDLK_B)});
    const std::string etext = fe::serialise_bindings(e);
    CHECK(etext.find("attack = key Comma, button leftstick\n") != std::string::npos);
    CHECK(etext.find("slot4 = none\n") != std::string::npos);
    CHECK(etext.find("back = key B\n") != std::string::npos);
    fe::BindingSet eback;
    const std::vector<std::string> p = fe::parse_bindings(etext, "e.ini", &eback);
    for (const std::string &l : p) {
        MESSAGE(l);
    }
    CHECK(eback.face == fe::FaceButtons::kLabel);
    CHECK(fe::same_bindings(eback.table, e.table));
    CHECK_FALSE(fe::same_bindings(eback.table, d.table));
    // Written again, the same text.
    CHECK(fe::serialise_bindings(eback) == etext);
}

TEST_CASE("bindings: the overrides apply to Actions: the new key presses, the old one does not") {
    g_panel = 0;
    fe::BindingSet s;
    REQUIRE(fe::parse_bindings("[field]\nattack = key K, button leftstick\n", "a.ini", &s).empty());
    fe::Actions a(nullptr);
    a.set_sink(record, nullptr);
    a.set_panel_source(panel, nullptr);
    // The defaults first.
    CHECK(tap(a, fe::InputSource::kKeyboard, key(SDLK_Z)) ==
          std::vector<std::string>{"+attack", "-attack"});
    CHECK(tap(a, fe::InputSource::kKeyboard, key(SDLK_K)).empty());
    a.bindings() = s.table;
    CHECK(tap(a, fe::InputSource::kKeyboard, key(SDLK_K)) ==
          std::vector<std::string>{"+attack", "-attack"});
    CHECK(tap(a, fe::InputSource::kKeyboard, key(SDLK_Z)).empty());
    CHECK(tap(a, fe::InputSource::kGamepad, button(SDL_GAMEPAD_BUTTON_LEFT_STICK)) ==
          std::vector<std::string>{"+attack", "-attack"});
    CHECK(tap(a, fe::InputSource::kGamepad, button(SDL_GAMEPAD_BUTTON_WEST)).empty());
    CHECK(tap(a, fe::InputSource::kGamepad, trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)).empty());
    // In menus Z is still OK.
    g_panel = 1;
    CHECK(tap(a, fe::InputSource::kKeyboard, key(SDLK_Z)) ==
          std::vector<std::string>{"+ok", "-ok"});
    g_panel = 0;
}

TEST_CASE("bindings: the overlay's edit: one control per device, taken from its old action") {
    fe::BindingSet s;
    // K for attack: its keys go, its pad controls stay.
    CHECK(fe::bind_control(&s.table, Context::kField, MT_ACTION_ATTACK, key(SDLK_K)) ==
          MT_ACTION_NONE);
    CHECK(field(s, MT_ACTION_ATTACK) ==
          std::vector<Control>{button(SDL_GAMEPAD_BUTTON_WEST),
                               trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), key(SDLK_K)});
    // X (jump's key) for attack: jump keeps Space and the Alts.
    CHECK(fe::bind_control(&s.table, Context::kField, MT_ACTION_ATTACK, key(SDLK_X)) ==
          MT_ACTION_JUMP);
    CHECK(field(s, MT_ACTION_JUMP) == std::vector<Control>{button(SDL_GAMEPAD_BUTTON_SOUTH),
                                                           key(SDLK_SPACE), key(SDLK_LALT),
                                                           key(SDLK_RALT)});
    // North (slot1's only pad control) for attack: slot1 has no gamepad control.
    CHECK(fe::bind_control(&s.table, Context::kField, MT_ACTION_ATTACK,
                           button(SDL_GAMEPAD_BUTTON_NORTH)) == MT_ACTION_SLOT1);
    CHECK(field(s, MT_ACTION_ATTACK) ==
          std::vector<Control>{button(SDL_GAMEPAD_BUTTON_NORTH), key(SDLK_X)});
    CHECK(has(fe::unbound_warnings(s.table), "slot1 has no gamepad control in the field"));
    // X in menus is untouched (back).
    CHECK(menu(s, MT_ACTION_BACK) == menu(fe::BindingSet{}, MT_ACTION_BACK));
}

TEST_CASE("bindings: load and save in a data directory; a missing file is the defaults") {
    TempDir dir("load");
    const std::string path = fe::bindings_path(dir.path.string(), "");
    CHECK(fs::path(path).filename() == "bindings.ini");
    CHECK(fe::bindings_path(dir.path.string(), "/elsewhere/b.ini") == "/elsewhere/b.ini");
    fe::BindingSet s;
    CHECK(fe::load_bindings(path, &s));
    CHECK(fe::same_bindings(s.table, fe::default_bindings()));
    fe::set_controls(&s.table, Context::kField, MT_ACTION_ATTACK, {key(SDLK_K)});
    s.face = fe::FaceButtons::kLabel;
    REQUIRE(fe::save_bindings(path, s));
    CHECK_FALSE(fs::exists(path + ".tmp"));
    const std::string text = read(path);
    CHECK(text.find("attack = key K\n") != std::string::npos);
    fe::BindingSet back;
    CHECK(fe::load_bindings(path, &back));
    CHECK(back.face == fe::FaceButtons::kLabel);
    CHECK(fe::same_bindings(back.table, s.table));
    // A file in a directory that does not exist yet is created.
    const std::string nested = (dir.path / "sub" / "b.ini").string();
    CHECK(fe::save_bindings(nested, s));
    CHECK(fs::exists(nested));
    // No data directory: nothing written.
    CHECK_FALSE(fe::save_bindings("", s));
}

TEST_CASE("bindings: Nintendo face buttons by position or by label") {
    using fe::FaceButtons;
    const auto ctl = [](SDL_GamepadType t, SDL_GamepadButton b, FaceButtons f) {
        return fe::gamepad_button_control(t, b, f).code;
    };
    const SDL_GamepadType sw = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    // By position: SDL's names.
    for (int b = SDL_GAMEPAD_BUTTON_SOUTH; b <= SDL_GAMEPAD_BUTTON_NORTH; b++) {
        CHECK(ctl(sw, static_cast<SDL_GamepadButton>(b), FaceButtons::kPosition) == b);
    }
    // By label: the bottom button (labelled B) is East, the right one (A) South, the left one
    // (Y) North, the top one (X) West.
    CHECK(ctl(sw, SDL_GAMEPAD_BUTTON_SOUTH, FaceButtons::kLabel) == SDL_GAMEPAD_BUTTON_EAST);
    CHECK(ctl(sw, SDL_GAMEPAD_BUTTON_EAST, FaceButtons::kLabel) == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(ctl(sw, SDL_GAMEPAD_BUTTON_WEST, FaceButtons::kLabel) == SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(ctl(sw, SDL_GAMEPAD_BUTTON_NORTH, FaceButtons::kLabel) == SDL_GAMEPAD_BUTTON_WEST);
    CHECK(ctl(sw, SDL_GAMEPAD_BUTTON_START, FaceButtons::kLabel) == SDL_GAMEPAD_BUTTON_START);
    // Xbox and PlayStation pads do not change.
    for (const SDL_GamepadType t : {SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_PS5}) {
        for (int b = SDL_GAMEPAD_BUTTON_SOUTH; b <= SDL_GAMEPAD_BUTTON_NORTH; b++) {
            CHECK(ctl(t, static_cast<SDL_GamepadButton>(b), FaceButtons::kLabel) == b);
        }
    }
    FaceButtons f{};
    CHECK(fe::parse_face_buttons("LABEL", &f));
    CHECK(f == FaceButtons::kLabel);
    CHECK(fe::parse_face_buttons("position", &f));
    CHECK(f == FaceButtons::kPosition);
    CHECK_FALSE(fe::parse_face_buttons("swap", &f));
}
