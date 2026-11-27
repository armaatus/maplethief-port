// Practice states (#409, practice_state.h, ADR 0189): the practice_pick and practice_load actions
// and their keys (F7, F8; none on a pad), a press becoming one request a turn, the state's path
// per category, slot and split, a split arming a state taken at the first safe turn (the last
// split's never), the pick among the splits with a state, and a load with the quick-load's
// refusals. The game's side is a fake (QuickOps); the real save and restore are
// play.practice_state's (real assets). No state file here is the game's.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "input.h"
#include "maplethief/input.h"
#include "practice_state.h"
#include "quick_state.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using Bytes = std::vector<u8>;
using fe::Context;
using fe::PracticeRequest;

fe::Control key(SDL_Keycode k) { return fe::Control{fe::ControlKind::kKey, static_cast<int>(k)}; }

std::int32_t field_panel(void *) { return 0; }
void no_sink(void *, u32, bool) {}

// The fake game: what each op answers, and what was called.
struct Fake {
    std::string unsafe; // "" safe; else mt_snapshot_safe's reason
    int slot = 1;
    std::string bad; // "" the file checks; else check's reason
    std::optional<Bytes> file;
    std::vector<std::string> calls;
    std::string written_path;

    fe::QuickOps ops() {
        fe::QuickOps o;
        o.safe = [this](std::string *why) {
            calls.push_back("safe");
            *why = unsafe;
            return unsafe.empty();
        };
        o.slot = [this] { return slot; };
        o.save = [this](u32 s, Bytes *out, std::string *) {
            calls.push_back("save " + std::to_string(s));
            *out = Bytes{1, 2, 3};
            return true;
        };
        o.check = [this](const Bytes &, u32 s) {
            calls.push_back("check " + std::to_string(s));
            return bad;
        };
        o.restore = [this](const Bytes &, u32 s, std::string *) {
            calls.push_back("restore " + std::to_string(s));
            return true;
        };
        o.write = [this](const std::string &path, const Bytes &) {
            calls.push_back("write " + path);
            written_path = path;
            return true;
        };
        o.read = [this](const std::string &path) {
            calls.push_back("read " + path);
            return file;
        };
        return o;
    }
};

} // namespace

TEST_CASE("practice states: practice_pick is F7 and practice_load F8, in the field and menus") {
    CHECK(std::string(mt_input_action_get_info(MT_ACTION_PRACTICE_PICK)->name) == "practice_pick");
    CHECK(std::string(mt_input_action_get_info(MT_ACTION_PRACTICE_LOAD)->name) == "practice_load");
    CHECK(fe::port_action(MT_ACTION_PRACTICE_PICK));
    CHECK(fe::port_action(MT_ACTION_PRACTICE_LOAD));
    const std::vector<fe::Binding> table = fe::default_bindings();
    for (const Context c : {Context::kField, Context::kMenu}) {
        CHECK(fe::controls_for(table, c, MT_ACTION_PRACTICE_PICK) ==
              std::vector<fe::Control>{key(SDLK_F7)});
        CHECK(fe::controls_for(table, c, MT_ACTION_PRACTICE_LOAD) ==
              std::vector<fe::Control>{key(SDLK_F8)});
    }
    for (const fe::Binding &b : fe::default_gamepad_bindings()) {
        CHECK(b.field != MT_ACTION_PRACTICE_PICK);
        CHECK(b.field != MT_ACTION_PRACTICE_LOAD);
        CHECK(b.menu != MT_ACTION_PRACTICE_PICK);
        CHECK(b.menu != MT_ACTION_PRACTICE_LOAD);
    }
    const std::string text = fe::serialise_bindings(fe::BindingSet{});
    CHECK(text.find("practice_pick = key F7\n") != std::string::npos);
    CHECK(text.find("practice_load = key F8\n") != std::string::npos);
}

TEST_CASE("practice states: a press is one request at the next turn, none with save_states off") {
    fe::Input input;
    fe::Actions actions(&input);
    actions.set_sink(no_sink, nullptr);
    actions.set_panel_source(field_panel, nullptr);
    fe::PracticeStates p;
    CHECK(p.take(actions, true) == PracticeRequest::kNone);
    actions.control(fe::InputSource::kKeyboard, key(SDLK_F7), true);
    actions.control(fe::InputSource::kKeyboard, key(SDLK_F7), false);
    CHECK(p.take(actions, true) == PracticeRequest::kPick);
    CHECK(p.take(actions, true) == PracticeRequest::kNone);
    actions.control(fe::InputSource::kKeyboard, key(SDLK_F8), true);
    actions.control(fe::InputSource::kKeyboard, key(SDLK_F8), false);
    CHECK(p.take(actions, false) == PracticeRequest::kNone); // off: forgotten, never later
    CHECK(p.take(actions, true) == PracticeRequest::kNone);
    CHECK(p.take(1, 1, true) == PracticeRequest::kNone);
    CHECK(p.take(2, 2, true) == PracticeRequest::kNone); // both in one turn: neither
    CHECK(p.take(2, 3, true) == PracticeRequest::kLoad);
}

TEST_CASE("practice states: one state per category, character slot and split") {
    CHECK(fe::practice_state_path("/data", "any", 0, 1) == "/data/states/any/slot0-split1.state");
    CHECK(fe::practice_state_path("/data/", "chapter_1", 2, 12) ==
          "/data/states/chapter_1/slot2-split12.state");
    CHECK(fe::practice_state_path("", "any", 1, 3) == "states/any/slot1-split3.state");
    CHECK(fe::practice_state_path("/data", "any", 3, 1).empty());
    CHECK(fe::practice_state_path("/data", "any", -1, 1).empty());
    CHECK(fe::practice_state_path("/data", "any", 0, 0).empty());
    CHECK(fe::practice_state_path("/data", "../any", 0, 1).empty());
    CHECK(fe::practice_state_path("/data", "", 0, 1).empty());
}

TEST_CASE("practice states: a split arms a state, taken at the first safe turn after it") {
    fe::PracticeStates p;
    Fake f;
    // Nothing armed: nothing asked of the game.
    CHECK_FALSE(p.take_armed(f.ops(), "/data", "any").done);
    CHECK(f.calls.empty());
    // The last split's state is never armed: nothing follows it.
    p.reached(3, 3);
    CHECK_FALSE(p.armed().has_value());
    p.reached(0, 3);
    CHECK_FALSE(p.armed().has_value());

    p.reached(1, 3);
    REQUIRE(p.armed() == std::optional<std::size_t>(1));
    // Not safe yet (a walking hero): nothing, and it stays armed.
    f.unsafe = "a key is held";
    fe::QuickResult r = p.take_armed(f.ops(), "/data", "any");
    CHECK_FALSE(r.done);
    CHECK(r.notice.empty());
    CHECK(f.calls == std::vector<std::string>{"safe"});
    CHECK(p.armed() == std::optional<std::size_t>(1));
    // A later split before a safe turn replaces it.
    p.reached(2, 3);
    f.unsafe.clear();
    f.calls.clear();
    r = p.take_armed(f.ops(), "/data", "any");
    CHECK(r.done);
    CHECK(f.written_path == "/data/states/any/slot1-split2.state");
    CHECK(f.calls == std::vector<std::string>{"safe", "safe", "save 1",
                                              "write /data/states/any/slot1-split2.state"});
    CHECK_FALSE(p.armed().has_value());
    CHECK(p.stats().saves == 1);
    // forget drops an armed split.
    p.reached(1, 3);
    p.forget();
    CHECK_FALSE(p.armed().has_value());
}

TEST_CASE("practice states: the pick goes through the splits that have a state, wrapping") {
    fe::PracticeStates p;
    Fake f;
    std::set<std::string> files = {"/data/states/any/slot1-split2.state",
                                   "/data/states/any/slot1-split4.state",
                                   "/data/states/any/slot0-split1.state",
                                   "/data/states/any/slot1-split5.state"}; // the last: never one
    const auto exists = [&files](const std::string &path) { return files.count(path) > 0; };
    const fe::QuickOps ops = f.ops();
    CHECK(p.pick(std::nullopt, 5, ops, "/data", "any", exists) == std::optional<std::size_t>(2));
    CHECK(p.pick(2, 5, ops, "/data", "any", exists) == std::optional<std::size_t>(4));
    CHECK(p.pick(4, 5, ops, "/data", "any", exists) == std::optional<std::size_t>(2));
    CHECK(p.pick(3, 5, ops, "/data", "any", exists) == std::optional<std::size_t>(4));
    CHECK_FALSE(p.pick(std::nullopt, 5, ops, "/data", "chapter_1", exists).has_value());
    CHECK_FALSE(p.pick(std::nullopt, 1, ops, "/data", "any", exists).has_value());
    Fake other;
    other.slot = 0;
    CHECK(p.pick(std::nullopt, 5, other.ops(), "/data", "any", exists) ==
          std::optional<std::size_t>(1));

    // practice_state_exists on a real folder.
    const fs::path dir = fs::temp_directory_path() / "mt_practice_state_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "states" / "any");
    std::ofstream(dir / "states" / "any" / "slot1-split2.state") << "x";
    const std::u8string d8 = dir.u8string();
    const std::string d(d8.begin(), d8.end());
    CHECK(fe::practice_state_exists(fe::practice_state_path(d, "any", 1, 2)));
    CHECK_FALSE(fe::practice_state_exists(fe::practice_state_path(d, "any", 1, 3)));
    CHECK_FALSE(fe::practice_state_exists(""));
    CHECK(p.pick(std::nullopt, 3, ops, d, "any", fe::practice_state_exists) ==
          std::optional<std::size_t>(2));
    fs::remove_all(dir, ec);
}

TEST_CASE("practice states: a load has the quick-load's checks and reasons") {
    fe::PracticeStates p;
    Fake f;
    f.file = Bytes{9};
    fe::QuickResult r = p.load(f.ops(), "/data", "any", 2);
    CHECK(r.done);
    CHECK(r.notice == "State loaded");
    CHECK(f.calls == std::vector<std::string>{"safe", "read /data/states/any/slot1-split2.state",
                                              "check 1", "restore 1"});
    Fake none;
    r = p.load(none.ops(), "/data", "any", 2);
    CHECK_FALSE(r.done);
    CHECK(r.notice == "Not loaded: no practice state for this split yet");
    Fake popup;
    popup.file = Bytes{9};
    popup.unsafe = "a popup is open";
    r = p.load(popup.ops(), "/data", "any", 2);
    CHECK(r.notice == "Not loaded: not while a popup is open");
    CHECK(popup.calls == std::vector<std::string>{"safe"});
    Fake held; // held input is no refusal: the restore lets go of it
    held.file = Bytes{9};
    held.unsafe = "a key is held";
    CHECK(p.load(held.ops(), "/data", "any", 2).done);
    Fake other;
    other.file = Bytes{9};
    other.bad = "the state is of another character";
    r = p.load(other.ops(), "/data", "any", 2);
    CHECK(r.notice == "Not loaded: the state is of another character");
    CHECK(other.calls.back() == "check 1");
    Fake nobody;
    nobody.slot = -1;
    CHECK(p.load(nobody.ops(), "/data", "any", 2).notice == "Not loaded: no character is in play");
    CHECK(p.stats().loads == 2);
    CHECK(p.stats().refused == 4);
}
