// The controller extras (#172, assists.h, ADR 0094) on a fake guest: auto_run's latched walk
// and hold_toggle's toggled walk and fast-forward. What starts a latch (only in the field,
// alive), every way one ends (the press again, the other direction, a menu, a popup, a scene,
// death, a new map, the settings off, a port overlay), the runtime's hold counts it leaves, and
// that with both off nothing is pressed. Actions' press counts and the default controls (L3, R).
// The game's side (the hero walking across map 2.8) is play_real_test's auto_run mode.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "actions.h"
#include "assists.h"
#include "maplethief/input.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct FakeGuest {
    std::map<u32, u32> words;
    void put32(u32 addr, u32 v) { words[addr] = v; }
    fe::GuestRead32 reader() const {
        return [this](u32 addr, u32 *out) {
            const auto it = words.find(addr);
            if (it == words.end()) {
                return false;
            }
            *out = it->second;
            return true;
        };
    }
};

constexpr u32 kCanvas = 0x01000000, kState = 0x01001000, kHero = 0x01002000;

// The hero standing in the field of map 2.8, HP 50 of 50, no items.
FakeGuest field_game() {
    FakeGuest g;
    g.put32(fe::kAutosaveCanvas, kCanvas);
    g.put32(kCanvas + 0x0C, kState);
    g.put32(kState, fe::kAutosaveGameVptr);
    g.put32(kState + 0x30, kHero);
    g.put32(kState + 0x84, 2);
    g.put32(kState + 0x88, 8);
    g.put32(fe::kAutosaveGameMode, 0);
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(kHero + 0x10, 2);
    g.put32(kHero + fe::kHeroHp, 50);
    g.put32(kHero + fe::kHeroMaxHp, 50);
    g.put32(kHero + fe::kHeroItems, 0);
    return g;
}

// The presses the Assists make, and the runtime's hold counts (as mt_input_press counts them),
// the player's own included.
struct Recorder {
    std::vector<std::pair<u32, bool>> presses;
    std::map<u32, u32> holds;
    fe::Assists::Press press() {
        return [this](u32 action, bool down) {
            presses.emplace_back(action, down);
            if (down) {
                holds[action]++;
            } else if (holds[action] > 0) {
                holds[action]--;
            }
        };
    }
};

// The player: what Actions would report (holding, presses by context).
struct Player {
    fe::PlayerHolds h;
    Recorder *rec = nullptr;
    void down(u32 action) {
        switch (action) {
        case MT_ACTION_LEFT:
            h.left = true;
            h.left_presses++;
            break;
        case MT_ACTION_RIGHT:
            h.right = true;
            h.right_presses++;
            break;
        case MT_ACTION_AUTO_RUN:
            h.auto_run_presses++;
            return; // a port action: never the runtime's
        case MT_ACTION_FAST_FORWARD:
            h.ff_presses++;
            return;
        default:
            break;
        }
        h.walk = h.left || h.right;
        rec->holds[action]++;
    }
    void up(u32 action) {
        if (action == MT_ACTION_LEFT) {
            h.left = false;
        } else if (action == MT_ACTION_RIGHT) {
            h.right = false;
        } else {
            return;
        }
        h.walk = h.left || h.right;
        rec->holds[action]--;
    }
};

// One turn of the Assists in the field panel (0) or another.
struct Rig {
    FakeGuest g = field_game();
    Recorder rec;
    fe::Assists a{rec.press()};
    fe::Settings s;
    Player p;
    u64 tick = 100;
    Rig() {
        p.rec = &rec;
        turn(); // the first turn sees the counts so far
    }
    void turn(std::int32_t panel = 0) { a.turn(g.reader(), s, p.h, panel, tick++, true, 0); }
    // A tap: down, a turn, up, a turn.
    void tap(u32 action, std::int32_t panel = 0) {
        p.down(action);
        turn(panel);
        p.up(action);
        turn(panel);
    }
    u32 held(u32 action) { return rec.holds[action]; }
};

} // namespace

TEST_CASE("auto_run: off by default, and with both off nothing is pressed") {
    Rig r;
    CHECK_FALSE(r.s.auto_run);
    CHECK(r.s.hold_toggle == fe::HoldToggle::kOff);
    r.p.down(MT_ACTION_RIGHT);
    r.turn();
    r.tap(MT_ACTION_AUTO_RUN);
    r.p.up(MT_ACTION_RIGHT);
    r.turn();
    r.tap(MT_ACTION_LEFT);
    r.tap(MT_ACTION_FAST_FORWARD);
    CHECK(r.rec.presses.empty());
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    CHECK_FALSE(r.a.fast_forward_latched());
    CHECK(r.held(MT_ACTION_RIGHT) == 0);
    CHECK(r.held(MT_ACTION_LEFT) == 0);
    CHECK(r.a.stats().latches == 0);
}

TEST_CASE("auto_run: latches the held direction, else the last one; a second press ends it") {
    Rig r;
    r.s.auto_run = true;
    // Nothing walked yet: nothing to keep going.
    r.tap(MT_ACTION_AUTO_RUN);
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    CHECK(r.rec.presses.empty());
    // Right held, auto_run: right latched; let go, the runtime still holds it once.
    r.p.down(MT_ACTION_RIGHT);
    r.turn();
    r.tap(MT_ACTION_AUTO_RUN);
    CHECK(r.a.walk_latch() == MT_ACTION_RIGHT);
    CHECK(r.held(MT_ACTION_RIGHT) == 2);
    r.p.up(MT_ACTION_RIGHT);
    r.turn();
    CHECK(r.held(MT_ACTION_RIGHT) == 1);
    for (int i = 0; i < 50; i++) {
        r.turn();
    }
    CHECK(r.a.walk_latch() == MT_ACTION_RIGHT);
    // Pressing the latched direction itself changes nothing (auto_run's latch).
    r.tap(MT_ACTION_RIGHT);
    CHECK(r.a.walk_latch() == MT_ACTION_RIGHT);
    CHECK(r.held(MT_ACTION_RIGHT) == 1);
    // A second auto_run: let go.
    r.tap(MT_ACTION_AUTO_RUN);
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    CHECK(r.held(MT_ACTION_RIGHT) == 0);
    CHECK(r.a.stats().last_end == fe::LatchEnd::kAgain);
    // Nothing held: the last direction pressed (right).
    r.tap(MT_ACTION_AUTO_RUN);
    CHECK(r.a.walk_latch() == MT_ACTION_RIGHT);
    r.tap(MT_ACTION_AUTO_RUN);
    // Both held: the one pressed last.
    r.p.down(MT_ACTION_RIGHT);
    r.turn();
    r.p.down(MT_ACTION_LEFT);
    r.turn();
    r.tap(MT_ACTION_AUTO_RUN);
    CHECK(r.a.walk_latch() == MT_ACTION_LEFT);
    r.p.up(MT_ACTION_LEFT);
    r.p.up(MT_ACTION_RIGHT);
    r.turn();
    CHECK(r.held(MT_ACTION_LEFT) == 1);
    CHECK(r.held(MT_ACTION_RIGHT) == 0);
    CHECK(r.a.stats().latches == 3);
    // A press and its release between two turns still counts (Actions' counts).
    r.tap(MT_ACTION_AUTO_RUN);
    r.p.down(MT_ACTION_AUTO_RUN);
    r.turn();
    CHECK(r.a.walk_latch() == MT_ACTION_LEFT);
}

TEST_CASE("auto_run: a latched walk ends on every cancel") {
    struct Cancel {
        const char *what;
        fe::LatchEnd why;
        void (*apply)(Rig &);
    };
    const Cancel cancels[] = {
        {"the other direction", fe::LatchEnd::kOther,
         [](Rig &r) { r.tap(MT_ACTION_LEFT); }},
        {"a menu panel (the key pad's panel leaves 0)", fe::LatchEnd::kField,
         [](Rig &r) { r.turn(1); }},
        {"no key pad (a UIKit screen)", fe::LatchEnd::kField, [](Rig &r) { r.turn(-1); }},
        {"a popup", fe::LatchEnd::kField,
         [](Rig &r) {
             r.g.put32(fe::kAutosavePopup, 0x68);
             r.turn();
         }},
        {"the popup menu (game mode 1)", fe::LatchEnd::kField,
         [](Rig &r) {
             r.g.put32(fe::kAutosaveGameMode, 1);
             r.turn();
         }},
        {"a scene (game mode 9)", fe::LatchEnd::kField,
         [](Rig &r) {
             r.g.put32(fe::kAutosaveGameMode, 9);
             r.turn();
         }},
        {"a map loading (game mode 7)", fe::LatchEnd::kField,
         [](Rig &r) {
             r.g.put32(fe::kAutosaveGameMode, 7);
             r.turn();
         }},
        {"another state (the title)", fe::LatchEnd::kField,
         [](Rig &r) {
             r.g.put32(kState, 0x119C98);
             r.turn();
         }},
        {"the hero dying", fe::LatchEnd::kDeath,
         [](Rig &r) {
             r.g.put32(kHero + 0x10, 0);
             r.turn();
         }},
        {"the hero dead", fe::LatchEnd::kDeath,
         [](Rig &r) {
             r.g.put32(kHero + 0x10, 1);
             r.turn();
         }},
        {"HP 0", fe::LatchEnd::kDeath,
         [](Rig &r) {
             r.g.put32(kHero + fe::kHeroHp, 0);
             r.turn();
         }},
        {"a new map", fe::LatchEnd::kMap,
         [](Rig &r) {
             r.g.put32(kState + 0x88, 9);
             r.turn();
         }},
        {"a new area", fe::LatchEnd::kMap,
         [](Rig &r) {
             r.g.put32(kState + 0x84, 3);
             r.turn();
         }},
        {"a new hero", fe::LatchEnd::kMap,
         [](Rig &r) {
             r.g.put32(kState + 0x30, kHero + 0x1000);
             r.g.put32(kHero + 0x1000 + 0x10, 2);
             r.g.put32(kHero + 0x1000 + fe::kHeroHp, 50);
             r.g.put32(kHero + 0x1000 + fe::kHeroMaxHp, 50);
             r.turn();
         }},
        {"auto_run turned off", fe::LatchEnd::kOff,
         [](Rig &r) {
             r.s.auto_run = false;
             r.turn();
         }},
        {"a port overlay", fe::LatchEnd::kOverlay, [](Rig &r) { r.a.drop_latches(0); }},
        {"auto_run again", fe::LatchEnd::kAgain, [](Rig &r) { r.tap(MT_ACTION_AUTO_RUN); }},
    };
    for (const Cancel &c : cancels) {
        CAPTURE(c.what);
        Rig r;
        r.s.auto_run = true;
        r.p.down(MT_ACTION_RIGHT);
        r.turn();
        r.tap(MT_ACTION_AUTO_RUN);
        r.p.up(MT_ACTION_RIGHT);
        r.turn();
        REQUIRE(r.a.walk_latch() == MT_ACTION_RIGHT);
        REQUIRE(r.held(MT_ACTION_RIGHT) == 1);
        c.apply(r);
        CHECK(r.a.walk_latch() == MT_ACTION_NONE);
        CHECK(r.held(MT_ACTION_RIGHT) == 0);
        CHECK(r.a.stats().latch_ends == 1);
        CHECK(r.a.stats().last_end == c.why);
        CHECK(std::string(fe::latch_end_name(c.why)) != "?");
        // Nothing else was pressed or left held (the other direction's own tap is the
        // player's and is let go).
        CHECK(r.held(MT_ACTION_LEFT) == 0);
        CHECK(r.rec.presses.size() == 2);
        // Nothing is latched again by itself.
        r.turn();
        CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    }
}

TEST_CASE("auto_run: never starts in a menu, a popup, a scene or dead") {
    struct Where {
        const char *what;
        std::int32_t panel;
        u32 addr, value;
    };
    const Where wheres[] = {
        {"a menu panel", 1, 0, 0},
        {"no key pad", -1, 0, 0},
        {"a popup", 0, fe::kAutosavePopup, 0x68},
        {"the popup menu", 0, fe::kAutosaveGameMode, 1},
        {"a scene", 0, fe::kAutosaveGameMode, 9},
        {"dying", 0, kHero + 0x10, 0},
        {"dead", 0, kHero + 0x10, 1},
    };
    for (const Where &w : wheres) {
        CAPTURE(w.what);
        Rig r;
        r.s.auto_run = true;
        r.s.hold_toggle = fe::HoldToggle::kAll;
        if (w.addr != 0) {
            r.g.put32(w.addr, w.value);
        }
        r.p.down(MT_ACTION_RIGHT);
        r.turn(w.panel);
        r.tap(MT_ACTION_AUTO_RUN, w.panel);
        r.p.up(MT_ACTION_RIGHT);
        r.turn(w.panel);
        r.tap(MT_ACTION_LEFT, w.panel);
        CHECK(r.a.walk_latch() == MT_ACTION_NONE);
        CHECK(r.rec.presses.empty());
    }
}

TEST_CASE("hold_toggle: walk toggles a direction, the other direction takes over") {
    Rig r;
    r.s.hold_toggle = fe::HoldToggle::kWalk;
    // A tap of left: walking left with nothing held.
    r.tap(MT_ACTION_LEFT);
    CHECK(r.a.walk_latch() == MT_ACTION_LEFT);
    CHECK(r.held(MT_ACTION_LEFT) == 1);
    // A tap of right: left ends, right latched.
    r.tap(MT_ACTION_RIGHT);
    CHECK(r.a.walk_latch() == MT_ACTION_RIGHT);
    CHECK(r.held(MT_ACTION_LEFT) == 0);
    CHECK(r.held(MT_ACTION_RIGHT) == 1);
    CHECK(r.a.stats().last_end == fe::LatchEnd::kOther);
    // A tap of right again: let go.
    r.tap(MT_ACTION_RIGHT);
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    CHECK(r.held(MT_ACTION_RIGHT) == 0);
    CHECK(r.a.stats().last_end == fe::LatchEnd::kAgain);
    // Held for a while and let go: still latched (a press toggles, the hold does not matter).
    r.p.down(MT_ACTION_LEFT);
    for (int i = 0; i < 10; i++) {
        r.turn();
    }
    r.p.up(MT_ACTION_LEFT);
    r.turn();
    CHECK(r.held(MT_ACTION_LEFT) == 1);
    // auto_run off: its press does nothing here.
    r.tap(MT_ACTION_AUTO_RUN);
    CHECK(r.a.walk_latch() == MT_ACTION_LEFT);
    // hold_toggle = fast_forward only: the walk latch ends (the setting off for walking).
    r.s.hold_toggle = fe::HoldToggle::kFastForward;
    r.turn();
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    CHECK(r.a.stats().last_end == fe::LatchEnd::kOff);
    r.tap(MT_ACTION_LEFT);
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    CHECK(r.held(MT_ACTION_LEFT) == 0);
    // With auto_run on too, auto_run's press ends a toggled latch and the same direction's
    // press ends an auto-run latch.
    r.s.hold_toggle = fe::HoldToggle::kAll;
    r.s.auto_run = true;
    r.tap(MT_ACTION_RIGHT);
    CHECK(r.a.walk_latch() == MT_ACTION_RIGHT);
    r.tap(MT_ACTION_AUTO_RUN);
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    r.tap(MT_ACTION_AUTO_RUN); // the last direction: right
    CHECK(r.a.walk_latch() == MT_ACTION_RIGHT);
    r.tap(MT_ACTION_RIGHT);
    CHECK(r.a.walk_latch() == MT_ACTION_NONE);
    CHECK(r.held(MT_ACTION_RIGHT) == 0);
}

TEST_CASE("hold_toggle: fast_forward toggles in any context, a port overlay ends it") {
    Rig r;
    r.tap(MT_ACTION_FAST_FORWARD);
    CHECK_FALSE(r.a.fast_forward_latched()); // off
    r.s.hold_toggle = fe::HoldToggle::kFastForward;
    r.tap(MT_ACTION_FAST_FORWARD);
    CHECK(r.a.fast_forward_latched());
    r.tap(MT_ACTION_FAST_FORWARD, 3); // in a menu: the port's clock, as the held key
    CHECK_FALSE(r.a.fast_forward_latched());
    r.tap(MT_ACTION_FAST_FORWARD, -1);
    CHECK(r.a.fast_forward_latched());
    r.a.drop_latches(0);
    CHECK_FALSE(r.a.fast_forward_latched());
    r.tap(MT_ACTION_FAST_FORWARD);
    CHECK(r.a.fast_forward_latched());
    r.s.hold_toggle = fe::HoldToggle::kWalk; // the setting off for fast-forward
    r.turn();
    CHECK_FALSE(r.a.fast_forward_latched());
    CHECK(r.rec.presses.empty()); // nothing reaches the game
}

TEST_CASE("auto_run: auto_potion waits while a walk is latched") {
    Rig r;
    // A Red Potion on quick slot 1 (as assists_test lays it out), HP low.
    constexpr u32 kItems = 0x01003000, kItem = 0x01004000;
    r.g.put32(kHero + fe::kHeroItems, kItems);
    for (u32 i = 0; i < fe::kHeroItemCount; i++) {
        r.g.put32(kItems + 4 * i, i == 0 ? kItem : 0);
    }
    r.g.put32(kItem + fe::kItemId, 3);
    r.g.put32(kItem + fe::kItemType, 1);
    r.g.put32(kItem + fe::kItemHp, 50);
    r.g.put32(kItem + fe::kItemCount, 5);
    r.g.put32(kHero + fe::kHeroQuickSlots + 8, 1);
    r.g.put32(kHero + fe::kHeroQuickSlots + 12, 3);
    r.g.put32(kHero + fe::kHeroHp, 5);
    r.s.auto_potion = fe::AutoPotion::k50;
    r.s.hold_toggle = fe::HoldToggle::kWalk;
    r.tap(MT_ACTION_RIGHT);
    REQUIRE(r.a.walk_latch() == MT_ACTION_RIGHT);
    for (int i = 0; i < 40; i++) {
        r.turn();
    }
    CHECK(r.a.stats().potions == 0);
    r.tap(MT_ACTION_RIGHT); // let go: the potion next
    CHECK(r.a.stats().potions == 1);
}

TEST_CASE("auto_run: the default sink leaves the runtime's hold to the player") {
    mt_input_configure(nullptr);
    FakeGuest g = field_game();
    fe::Assists a; // mt_input_press / mt_input_release
    fe::Settings s;
    s.auto_run = true;
    fe::PlayerHolds h;
    a.turn(g.reader(), s, h, 0, 0, true, 0);
    mt_input_press(MT_ACTION_RIGHT); // the player's
    h.right = h.walk = true;
    h.right_presses = 1;
    h.auto_run_presses = 1;
    a.turn(g.reader(), s, h, 0, 1, true, 0);
    CHECK(mt_input_held(MT_ACTION_RIGHT) == 2);
    mt_input_release(MT_ACTION_RIGHT); // let go: still walking
    CHECK(mt_input_held(MT_ACTION_RIGHT) == 1);
    a.drop_latches(0);
    CHECK(mt_input_held(MT_ACTION_RIGHT) == 0);
    mt_input_configure(nullptr);
}

namespace {

std::int32_t g_panel = 0;
std::vector<std::pair<u32, bool>> g_sent;

} // namespace

TEST_CASE("auto_run: Actions counts presses by context; L3 and R are auto_run in the field") {
    fe::Actions actions(nullptr);
    actions.set_panel_source([](void *) { return g_panel; }, nullptr);
    actions.set_sink([](void *, u32 action, bool down) { g_sent.emplace_back(action, down); },
                     nullptr);
    g_sent.clear();
    const fe::Control l3{fe::ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_LEFT_STICK};
    const fe::Control r_key{fe::ControlKind::kKey, static_cast<int>(SDLK_R)};
    const fe::Control left{fe::ControlKind::kDirection, static_cast<int>(fe::Direction::kLeft)};
    CHECK(fe::port_action(MT_ACTION_AUTO_RUN));
    CHECK(actions.action_for(l3, fe::Context::kField) == MT_ACTION_AUTO_RUN);
    CHECK(actions.action_for(r_key, fe::Context::kField) == MT_ACTION_AUTO_RUN);
    CHECK(actions.action_for(l3, fe::Context::kMenu) == MT_ACTION_NONE);
    CHECK(actions.action_for(r_key, fe::Context::kMenu) == MT_ACTION_REVIVE);
    const mt_input_action_info *info = mt_input_action_get_info(MT_ACTION_AUTO_RUN);
    REQUIRE(info != nullptr);
    CHECK(std::string(info->name) == "auto_run");
    CHECK(mt_input_action_named("auto_run") == MT_ACTION_AUTO_RUN);
    g_panel = 0;
    CHECK(actions.control(fe::InputSource::kGamepad, l3, true) == MT_ACTION_AUTO_RUN);
    CHECK(actions.control(fe::InputSource::kGamepad, l3, false) == MT_ACTION_AUTO_RUN);
    CHECK(actions.control(fe::InputSource::kKeyboard, r_key, true) == MT_ACTION_AUTO_RUN);
    CHECK(actions.control(fe::InputSource::kKeyboard, r_key, false) == MT_ACTION_AUTO_RUN);
    CHECK(g_sent.empty()); // the port's: never sent to the game
    actions.control(fe::InputSource::kGamepad, left, true);
    actions.control(fe::InputSource::kGamepad, left, false);
    g_panel = 3;
    actions.control(fe::InputSource::kGamepad, left, true);
    actions.control(fe::InputSource::kGamepad, left, false);
    actions.control(fe::InputSource::kKeyboard, r_key, true); // revive in a menu
    actions.control(fe::InputSource::kKeyboard, r_key, false);
    CHECK(actions.presses(MT_ACTION_AUTO_RUN, fe::Context::kField) == 2);
    CHECK(actions.presses(MT_ACTION_AUTO_RUN, fe::Context::kMenu) == 0);
    CHECK(actions.presses(MT_ACTION_LEFT, fe::Context::kField) == 1);
    CHECK(actions.presses(MT_ACTION_LEFT, fe::Context::kMenu) == 1);
    CHECK(actions.presses(MT_ACTION_REVIVE, fe::Context::kMenu) == 1);
    CHECK(actions.presses(MT_ACTION_COUNT, fe::Context::kField) == 0);
    g_panel = 0;
}

TEST_CASE("auto_run: the rows") {
    for (const char *key : {"auto_run", "hold_toggle"}) {
        CAPTURE(key);
        const fe::SettingInfo *row = fe::find_setting(key);
        REQUIRE(row != nullptr);
        CHECK(row->applies == fe::Applies::kAtOnce);
        CHECK(row->group == fe::SettingGroup::kAssists);
    }
    fe::Settings s;
    std::string why;
    REQUIRE(fe::apply_setting_list(&s, "auto_run = on; hold_toggle = all", &why));
    CHECK(s.auto_run);
    CHECK(s.hold_toggle == fe::HoldToggle::kAll);
    CHECK(fe::hold_toggle_walk(fe::HoldToggle::kAll));
    CHECK(fe::hold_toggle_fast_forward(fe::HoldToggle::kAll));
    CHECK(fe::hold_toggle_walk(fe::HoldToggle::kWalk));
    CHECK_FALSE(fe::hold_toggle_fast_forward(fe::HoldToggle::kWalk));
    CHECK_FALSE(fe::hold_toggle_walk(fe::HoldToggle::kFastForward));
    CHECK_FALSE(fe::hold_toggle_walk(fe::HoldToggle::kOff));
    CHECK(std::string(fe::hold_toggle_name(fe::HoldToggle::kFastForward)) == "fast_forward");
    CHECK_FALSE(fe::apply_setting_list(&s, "hold_toggle = jump", &why));
}
