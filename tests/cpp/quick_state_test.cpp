// Quick-save and quick-load (#204, quick_state.h): the save_states Assist and its row, the
// quick_save and quick_load actions and their default keys (F5, F9; none on a pad) through the
// bindings file, a press becoming one request a turn, the state's path and atomic write, every
// refusal reason in the player's words, the header check of a state (another binary, version,
// character, a damaged file) and a save or load refused before anything changes. The game's
// side is a fake (QuickOps); the real save and restore are objc.snapshot's (synthetic) and
// play.quick_state's (real assets). Every state file here is synthetic, made in memory or a
// temporary directory.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "input.h"
#include "keyboard.h"
#include "maplethief/input.h"
#include "maplethief/snapshot.h"
#include "quick_state.h"
#include "settings.h"
#include "snapshot/state_file.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using Bytes = std::vector<u8>;
using fe::Context;
using fe::QuickRequest;

fe::Control key(SDL_Keycode k) { return fe::Control{fe::ControlKind::kKey, static_cast<int>(k)}; }

std::int32_t field_panel(void *) { return 0; }
void no_sink(void *, u32, bool) {}

SDL_Event key_event(SDL_Keycode k, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    return e;
}

// A synthetic state file's bytes with this header (no game: two small sections).
Bytes state_file(const u8 sha_byte, u32 slot, const char *version) {
    mt_snapshot_header h{};
    h.format = MT_SNAPSHOT_FORMAT;
    std::memset(h.binary_sha256, sha_byte, 32);
    std::snprintf(h.port_version, sizeof h.port_version, "%s", version);
    h.slot = slot;
    h.guest_unix_us = 1735689600000000ull;
    std::vector<mt_snap::Section> sections;
    sections.push_back(mt_snap::Section{"alpha", Bytes(300, 0x11)});
    Bytes out;
    std::string why;
    REQUIRE(mt_snap::write_state_file(h, sections, false, &out, &why));
    return out;
}

// The fake game: what each op answers, and what was called.
struct Fake {
    std::string unsafe;  // "" safe; else mt_snapshot_safe's reason
    int slot = 1;        // the slot in play
    std::string no_save; // "" saves; else mt_snapshot_save's reason
    std::string bad;     // "" the file checks; else check's reason
    std::string no_restore;
    bool write_ok = true;
    std::optional<Bytes> file; // what read gives
    std::vector<std::string> calls;
    std::string written_path;
    Bytes written;

    fe::QuickOps ops() {
        fe::QuickOps o;
        o.safe = [this](std::string *why) {
            calls.push_back("safe");
            *why = unsafe;
            return unsafe.empty();
        };
        o.slot = [this] { return slot; };
        o.save = [this](u32 s, Bytes *out, std::string *why) {
            calls.push_back("save " + std::to_string(s));
            *why = no_save;
            *out = Bytes{1, 2, 3, static_cast<u8>(s)};
            return no_save.empty();
        };
        o.check = [this](const Bytes &, u32 s) {
            calls.push_back("check " + std::to_string(s));
            return bad;
        };
        o.restore = [this](const Bytes &, u32 s, std::string *why) {
            calls.push_back("restore " + std::to_string(s));
            *why = no_restore;
            return no_restore.empty();
        };
        o.write = [this](const std::string &path, const Bytes &b) {
            calls.push_back("write");
            if (write_ok) {
                written_path = path;
                written = b;
            }
            return write_ok;
        };
        o.read = [this](const std::string &path) {
            calls.push_back("read " + path);
            return file;
        };
        return o;
    }
};

} // namespace

TEST_CASE("save states: save_states is an Assist, off in every preset, live, with its warning") {
    const fe::SettingInfo *row = fe::find_setting("save_states");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kAssists);
    for (const fe::Preset preset :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        const char *p = row->presets[preset];
        REQUIRE(p != nullptr);
        CHECK(std::string(p) == "off");
    }
    CHECK(std::string(row->flag) == "--save-states");
    CHECK(row->applies == fe::Applies::kAtOnce);
    CHECK(std::string(row->comment).find("Assist:") != std::string::npos);
    // Off without a file; the file's line turns it on.
    fe::Settings s;
    CHECK_FALSE(s.save_states);
    CHECK(fe::parse_settings("save_states = on\n", &s).empty());
    CHECK(s.save_states);
    CHECK(fe::parse_settings("save_states = off\n", &s).empty());
    CHECK_FALSE(s.save_states);
    CHECK_FALSE(fe::parse_settings("save_states = maybe\n", &s).empty());
}

TEST_CASE("save states: quick_save is F5 and quick_load F9, in the field and menus, none on pads") {
    CHECK(std::string(mt_input_action_get_info(MT_ACTION_QUICK_SAVE)->name) == "quick_save");
    CHECK(std::string(mt_input_action_get_info(MT_ACTION_QUICK_LOAD)->name) == "quick_load");
    CHECK(fe::port_action(MT_ACTION_QUICK_SAVE));
    CHECK(fe::port_action(MT_ACTION_QUICK_LOAD));
    const std::vector<fe::Binding> table = fe::default_bindings();
    for (const Context c : {Context::kField, Context::kMenu}) {
        CHECK(fe::controls_for(table, c, MT_ACTION_QUICK_SAVE) ==
              std::vector<fe::Control>{key(SDLK_F5)});
        CHECK(fe::controls_for(table, c, MT_ACTION_QUICK_LOAD) ==
              std::vector<fe::Control>{key(SDLK_F9)});
    }
    for (const fe::Binding &b : fe::default_gamepad_bindings()) {
        CHECK(b.field != MT_ACTION_QUICK_SAVE);
        CHECK(b.field != MT_ACTION_QUICK_LOAD);
        CHECK(b.menu != MT_ACTION_QUICK_SAVE);
        CHECK(b.menu != MT_ACTION_QUICK_LOAD);
    }
    // F5 and F9 are bound to nothing else (F1 the menu, F3 the frame-time overlay, F11 the
    // fullscreen and F12 the screenshot are the port's own, never in the table).
    for (const fe::Binding &b : table) {
        if (b.control == key(SDLK_F5)) {
            CHECK(b.field == MT_ACTION_QUICK_SAVE);
        }
        if (b.control == key(SDLK_F9)) {
            CHECK(b.field == MT_ACTION_QUICK_LOAD);
        }
        for (const SDL_Keycode own : {SDLK_F1, SDLK_F3, SDLK_F11, SDLK_F12}) {
            CHECK_FALSE(b.control == key(own));
        }
    }
    // bindings.ini lists them; a rebind to a pad button round-trips; none is no warning.
    fe::BindingSet set;
    const std::string text = fe::serialise_bindings(set);
    CHECK(text.find("quick_save = key F5\n") != std::string::npos);
    CHECK(text.find("quick_load = key F9\n") != std::string::npos);
    fe::BindingSet edited;
    const std::vector<std::string> problems = fe::parse_bindings(
        "[field]\nquick_save = button paddle1, key F6\nquick_load = none\n", "b.ini", &edited);
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
    CHECK(fe::controls_for(edited.table, Context::kField, MT_ACTION_QUICK_SAVE).size() == 2);
    CHECK(fe::controls_for(edited.table, Context::kField, MT_ACTION_QUICK_LOAD).empty());
    fe::BindingSet back;
    CHECK(fe::parse_bindings(fe::serialise_bindings(edited), "b.ini", &back).empty());
    CHECK(fe::same_bindings(back.table, edited.table));
    // The remap overlay's rows: both, in both contexts.
    int slots = 0;
    for (const fe::ActionSlot &s : fe::action_slots(table)) {
        slots += s.action == MT_ACTION_QUICK_SAVE || s.action == MT_ACTION_QUICK_LOAD;
    }
    CHECK(slots == 4);
}

TEST_CASE("save states: a press is one request at the next turn; never the runtime's") {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Keyboard keyboard{&actions};
    actions.set_sink(no_sink, nullptr);
    actions.set_panel_source(field_panel, nullptr);
    fe::QuickStates q;
    CHECK(q.take(actions, true) == QuickRequest::kNone);
    keyboard.handle(key_event(SDLK_F5, true));
    keyboard.handle(key_event(SDLK_F5, false));
    CHECK(q.take(actions, true) == QuickRequest::kSave);
    CHECK(q.take(actions, true) == QuickRequest::kNone); // once
    keyboard.handle(key_event(SDLK_F9, true));
    CHECK(q.take(actions, true) == QuickRequest::kLoad);
    keyboard.handle(key_event(SDLK_F9, false));
    CHECK(q.take(actions, true) == QuickRequest::kNone);
    // Both in one turn: neither.
    CHECK(q.take(5, 7, true) == QuickRequest::kNone);
    CHECK(q.take(6, 7, true) == QuickRequest::kSave);
    CHECK(q.take(7, 8, true) == QuickRequest::kNone);
    // save_states off: a press does nothing, now or once it is turned on.
    CHECK(q.take(8, 8, false) == QuickRequest::kNone);
    CHECK(q.take(8, 9, false) == QuickRequest::kNone);
    CHECK(q.take(8, 9, true) == QuickRequest::kNone);
    CHECK(q.take(9, 9, true) == QuickRequest::kSave);
    CHECK(std::string(fe::quick_request_name(QuickRequest::kLoad)) == "quick_load");
}

TEST_CASE("save states: a state file over kMaxStateBytes is refused before it is read (#413)") {
    // A sparse file one byte over 1 GiB: refused from its size, so nothing is allocated for it.
    const fs::path dir = fs::temp_directory_path() / "maplethief_quick_state_cap_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path big = dir / "slot0.state";
    {
        std::ofstream(big, std::ios::binary).put('\x01');
    }
    fs::resize_file(big, fe::kMaxStateBytes + 1, ec);
    REQUIRE_FALSE(ec);
    CHECK(fe::kMaxStateBytes == std::uint64_t(1) << 30);
    CHECK_FALSE(fe::read_quick_state(big.string()).has_value());
    // A small one reads.
    fs::resize_file(big, 3, ec);
    REQUIRE_FALSE(ec);
    CHECK(fe::read_quick_state(big.string()) == Bytes{1, 0, 0});
    fs::remove_all(dir, ec);
}

TEST_CASE("save states: one state per character slot, written atomically") {
    CHECK(fe::quick_state_path("/data", 0) == "/data/states/slot0.state");
    CHECK(fe::quick_state_path("/data/", 2) == "/data/states/slot2.state");
    CHECK(fe::quick_state_path("C:\\data\\", 1) == "C:\\data\\states/slot1.state");
    CHECK(fe::quick_state_path("/data", -1).empty());
    CHECK(fe::quick_state_path("/data", 3).empty());

    const fs::path dir = fs::temp_directory_path() / "maplethief_quick_state_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string path = fe::quick_state_path(dir.string(), 1);
    CHECK_FALSE(fe::read_quick_state(path).has_value());
    const Bytes a(100000, 0xA5), b{1, 2, 3};
    REQUIRE(fe::write_quick_state(path, a)); // makes states/
    CHECK(fe::read_quick_state(path) == a);
    REQUIRE(fe::write_quick_state(path, b)); // replaced whole
    CHECK(fe::read_quick_state(path) == b);
    // No temporary file is left beside it.
    std::size_t files = 0;
    for (const auto &e : fs::directory_iterator(dir / "states")) {
        CHECK(e.path().filename() == "slot1.state");
        files++;
    }
    CHECK(files == 1);
    fs::remove_all(dir, ec);
}

TEST_CASE("save states: every refusal of the safe point in the player's words") {
    // runtime/src/snapshot/snapshot.cpp's reasons, as mt_snapshot_safe gives them.
    const struct {
        const char *why;
        const char *says;
        bool held;
    } cases[] = {
        {"no game is running", "no character is in play", false},
        {"inside a run-loop turn", "the game is busy, try again", false},
        {"during a scene", "not during a scene", false},
        {"not in the field (game mode 4)", "not in a menu", false},
        {"not in the field (the key pad shows another panel)", "not in a menu", false},
        {"a popup is open", "not while a popup is open", false},
        {"a touch is down", "let go of the keys first", true},
        {"a key is held", "let go of the keys first", true},
        {"input is waiting", "let go of the keys first", true},
        {"an animation is running", "not while the screen moves", false},
        {"background work is running (GCD)", "the game is busy, try again", false},
        {"a file is open", "the game is busy, try again", false},
        {"autoreleased objects are waiting", "the game is busy, try again", false},
    };
    for (const auto &c : cases) {
        CAPTURE(c.why);
        CHECK(fe::quick_state_reason(c.why) == c.says);
        CHECK(fe::held_input_reason(c.why) == c.held);
    }
    // One it does not know is the runtime's own.
    CHECK(fe::quick_state_reason("host state type X has no serializer in this port") ==
          "host state type X has no serializer in this port");
}

TEST_CASE("save states: a state of another binary, version or character, or damaged, is refused") {
    u8 sha[32];
    std::memset(sha, 0x5A, 32);
    const Bytes good = state_file(0x5A, 1, "1.2.3");
    CHECK(fe::check_quick_state(good, sha, 1, "1.2.3").empty());
    CHECK(fe::check_quick_state(state_file(0x77, 1, "1.2.3"), sha, 1, "1.2.3") ==
          "the state is of another copy of the game");
    CHECK(fe::check_quick_state(good, sha, 1, "1.2.4") ==
          "the state is of another version (1.2.3)");
    CHECK(fe::check_quick_state(good, sha, 2, "1.2.3") == "the state is of another character");
    Bytes damaged = good;
    damaged[damaged.size() - 3] ^= 0xFF; // the section's bytes: its CRC fails
    CHECK(fe::check_quick_state(damaged, sha, 1, "1.2.3") == "the state file is damaged");
    CHECK(fe::check_quick_state(Bytes(good.begin(), good.begin() + 50), sha, 1, "1.2.3") ==
          "the state file is damaged");
    CHECK(fe::check_quick_state(Bytes{}, sha, 1, "1.2.3") == "the state file is damaged");
    // Another format version (the header's CRC made right again).
    Bytes other = good;
    other[8] = static_cast<u8>(MT_SNAPSHOT_FORMAT + 1);
    const u32 crc = mt_crc32(0, other.data(), 100);
    for (int i = 0; i < 4; i++) {
        other[100 + i] = static_cast<u8>(crc >> (8 * i));
    }
    CHECK(fe::check_quick_state(other, sha, 1, "1.2.3") ==
          "the state is of another version (format " + std::to_string(MT_SNAPSHOT_FORMAT + 1) +
              ")");
}

TEST_CASE("save states: a quick-save writes the slot's state; a refusal writes nothing") {
    fe::QuickStates q;
    Fake f;
    fe::QuickResult r = q.run(QuickRequest::kSave, f.ops(), "/data");
    CHECK(r.done);
    CHECK(r.notice == "State saved");
    CHECK(f.written_path == "/data/states/slot1.state");
    CHECK(f.written == Bytes{1, 2, 3, 1});
    CHECK(q.stats().saves == 1);

    const struct {
        const char *unsafe;
        const char *notice;
    } unsafe[] = {
        {"during a scene", "Not saved: not during a scene"},
        {"not in the field (game mode 3)", "Not saved: not in a menu"},
        {"a popup is open", "Not saved: not while a popup is open"},
        {"a key is held", "Not saved: let go of the keys first"},
    };
    for (const auto &c : unsafe) {
        Fake g;
        g.unsafe = c.unsafe;
        r = q.run(QuickRequest::kSave, g.ops(), "/data");
        CHECK_FALSE(r.done);
        CHECK(r.notice == c.notice);
        CHECK(g.calls == std::vector<std::string>{"safe"});
    }
    Fake none;
    none.slot = -1;
    r = q.run(QuickRequest::kSave, none.ops(), "/data");
    CHECK(r.notice == "Not saved: no character is in play");
    CHECK(none.written.empty());
    Fake failed;
    failed.no_save = "host state type UIFoo has no serializer in this port";
    r = q.run(QuickRequest::kSave, failed.ops(), "/data");
    CHECK(r.notice == "Not saved: host state type UIFoo has no serializer in this port");
    CHECK(failed.written.empty());
    Fake unwritten;
    unwritten.write_ok = false;
    r = q.run(QuickRequest::kSave, unwritten.ops(), "/data");
    CHECK(r.notice == "Not saved: the state could not be written");
    CHECK(q.stats().saves == 1);
    CHECK(q.stats().refused == 7);
    // kNone does nothing.
    Fake idle;
    r = q.run(QuickRequest::kNone, idle.ops(), "/data");
    CHECK(r.notice.empty());
    CHECK(idle.calls.empty());
}

TEST_CASE("save states: a quick-load checks everything before the restore changes anything") {
    fe::QuickStates q;
    Fake f;
    f.file = Bytes{9, 9};
    fe::QuickResult r = q.run(QuickRequest::kLoad, f.ops(), "/data");
    CHECK(r.done);
    CHECK(r.notice == "State loaded");
    CHECK(f.calls == std::vector<std::string>{"safe", "read /data/states/slot1.state", "check 1",
                                              "restore 1"});
    // Held input is no refusal: the restore lets go of it.
    Fake held;
    held.file = Bytes{9};
    held.unsafe = "a key is held";
    CHECK(q.run(QuickRequest::kLoad, held.ops(), "/data").done);
    // Refusals, each before the restore.
    Fake popup;
    popup.file = Bytes{9};
    popup.unsafe = "a popup is open";
    r = q.run(QuickRequest::kLoad, popup.ops(), "/data");
    CHECK(r.notice == "Not loaded: not while a popup is open");
    CHECK(popup.calls == std::vector<std::string>{"safe"});
    Fake scene;
    scene.unsafe = "during a scene";
    CHECK(q.run(QuickRequest::kLoad, scene.ops(), "/data").notice ==
          "Not loaded: not during a scene");
    Fake missing;
    r = q.run(QuickRequest::kLoad, missing.ops(), "/data");
    CHECK(r.notice == "Not loaded: no state for this character yet");
    CHECK(missing.calls.back() == "read /data/states/slot1.state");
    Fake other;
    other.slot = 0;
    other.file = Bytes{9};
    other.bad = "the state is of another character";
    r = q.run(QuickRequest::kLoad, other.ops(), "/data");
    CHECK(r.notice == "Not loaded: the state is of another character");
    CHECK(other.calls.back() == "check 0");
    Fake refused;
    refused.file = Bytes{9};
    refused.no_restore = "an animation is running";
    r = q.run(QuickRequest::kLoad, refused.ops(), "/data");
    CHECK(r.notice == "Not loaded: not while the screen moves");
    CHECK_FALSE(r.done);
    CHECK(q.stats().loads == 2);
}
