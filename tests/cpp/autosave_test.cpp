// Autosave (#150, autosave.h, docs/re/autosave.md) with synthetic data only: every guard on a
// fake guest memory (a CStateGame, its hero and the data manager laid out as the game has them),
// one value changed at a time, in the order the guards are checked; the 16 maps where the game
// refuses to save; the setting's row; Lifecycle::leaving, which says when the turn hook saves;
// and the Autosaver's decisions (off, nothing to leave, once per turn, a skip when there is no
// game). The save itself runs the game's code: play_real_test's autosave modes (real assets).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "autosave.h"
#include "lifecycle.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using fe::AutosaveGuard;
using u32 = std::uint32_t;

// A fake guest: bytes at addresses; unwritten bytes are not mapped.
struct FakeGuest {
    std::map<u32, std::uint8_t> bytes;
    void put32(u32 addr, u32 v) {
        for (u32 i = 0; i < 4; i++) {
            bytes[addr + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    void put8(u32 addr, std::uint8_t v) { bytes[addr] = v; }
    fe::GuestRead32 reader() const {
        return [this](u32 addr, u32 *out) {
            u32 v = 0;
            for (u32 i = 0; i < 4; i++) {
                const auto it = bytes.find(addr + i);
                if (it == bytes.end()) {
                    return false;
                }
                v |= u32{it->second} << (8 * i);
            }
            *out = v;
            return true;
        };
    }
};

constexpr u32 kCanvas = 0x01000000, kState = 0x01001000, kHero = 0x01002000, kMgr = 0x01003000;

// The field on Henesys (2.0), the hero standing, slot 1: every guard passes.
FakeGuest field_game() {
    FakeGuest g;
    g.put32(fe::kAutosaveCanvas, kCanvas);
    g.put32(kCanvas + 0x0C, kState);
    g.put32(kState, fe::kAutosaveGameVptr);
    g.put32(kState + 0x30, kHero);
    g.put32(kState + 0x84, 2);
    g.put32(kState + 0x88, 0);
    g.put32(kState + 0xB4, 0); // the warp byte and its neighbours
    g.put32(fe::kAutosaveDataMgr, kMgr);
    g.put32(fe::kAutosaveGameMode, 0);
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(kHero + 0x0C, 0);  // flags
    g.put32(kHero + 0x10, 2);  // standing
    g.put32(kHero + 0x119, 0); // knock-back
    g.put32(kMgr + 0x108, 0);  // a scroll
    g.put32(kMgr + 0x4E4, 1);  // the slot
    return g;
}

AutosaveGuard guard_of(const FakeGuest &g) { return fe::check_autosave(g.reader()).guard; }

SDL_Event event(std::uint32_t type) {
    SDL_Event e{};
    e.type = type;
    return e;
}

} // namespace

TEST_CASE("autosave: in the field on a plain map every guard passes") {
    const FakeGuest g = field_game();
    const fe::AutosaveCheck c = fe::check_autosave(g.reader());
    CHECK(c.ok());
    CHECK(c.state == kState);
    CHECK(c.hero == kHero);
    CHECK(c.data_mgr == kMgr);
    CHECK(c.slot == 1);
    CHECK(c.area == 2);
    CHECK(c.map == 0);
    CHECK(c.describe() == "slot 1, map 2.0");
    // The popup menu (mode 1) is where the game's own Save is: allowed. Walking and climbing too.
    FakeGuest menu = field_game();
    menu.put32(fe::kAutosaveGameMode, 1);
    CHECK(guard_of(menu) == AutosaveGuard::kNone);
    for (const u32 action : {3u, 4u}) {
        FakeGuest h = field_game();
        h.put32(kHero + 0x10, action);
        if (action == 4) {
            h.put32(kHero + 0x0C, 0x4); // on a ladder
        }
        CHECK(guard_of(h) == AutosaveGuard::kNone);
    }
}

TEST_CASE("autosave: outside the game there is nothing to save") {
    CHECK(guard_of(FakeGuest{}) == AutosaveGuard::kNotInGame);
    FakeGuest title = field_game();
    title.put32(kState, 0x119C98); // a CStateTitle
    CHECK(guard_of(title) == AutosaveGuard::kNotInGame);
    FakeGuest no_hero = field_game();
    no_hero.put32(kState + 0x30, 0);
    CHECK(guard_of(no_hero) == AutosaveGuard::kNotInGame);
    FakeGuest no_mgr = field_game();
    no_mgr.put32(fe::kAutosaveDataMgr, 0);
    CHECK(guard_of(no_mgr) == AutosaveGuard::kNotInGame);
    FakeGuest bad_slot = field_game();
    bad_slot.put32(kMgr + 0x4E4, 3);
    CHECK(guard_of(bad_slot) == AutosaveGuard::kNotInGame);
    // A bare cpu (no guest memory): the same, without reading anything.
    const mt_cpu cpu{};
    CHECK(fe::check_autosave(&cpu).guard == AutosaveGuard::kNotInGame);
}

TEST_CASE("autosave: each of the game's guards refuses on its own, with its value") {
    struct Case {
        const char *what;
        void (*change)(FakeGuest &);
        AutosaveGuard want;
        const char *text;
    };
    const Case cases[] = {
        {"loading (6)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 6); },
         AutosaveGuard::kLoading, "loading (game mode 6)"},
        {"loading (7)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 7); },
         AutosaveGuard::kLoading, "loading (game mode 7)"},
        {"loading (8)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 8); },
         AutosaveGuard::kLoading, "loading (game mode 8)"},
        {"a scene (9)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 9); },
         AutosaveGuard::kScene, "a scene is playing (game mode 9)"},
        {"a scene (10)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 10); },
         AutosaveGuard::kScene, "a scene is playing (game mode 10)"},
        {"the shop (2)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 2); },
         AutosaveGuard::kNotField, "not in the field or the popup menu (game mode 2)"},
        {"NPC chat (4)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 4); },
         AutosaveGuard::kNotField, "not in the field or the popup menu (game mode 4)"},
        {"the cash shop (11)", [](FakeGuest &g) { g.put32(fe::kAutosaveGameMode, 11); },
         AutosaveGuard::kNotField, "not in the field or the popup menu (game mode 11)"},
        {"a popup", [](FakeGuest &g) { g.put32(fe::kAutosavePopup, 13); }, AutosaveGuard::kPopup,
         "a popup is open (popup 13)"},
        {"a warp", [](FakeGuest &g) { g.put8(kState + 0xB4, 1); }, AutosaveGuard::kWarp,
         "a map change is pending"},
        {"knock-back", [](FakeGuest &g) { g.put8(kHero + 0x119, 1); }, AutosaveGuard::kKnockBack,
         "the hero is thrown back by a hit"},
        {"a scroll", [](FakeGuest &g) { g.put8(kMgr + 0x108, 1); }, AutosaveGuard::kScroll,
         "a scroll is being applied"},
        {"dying", [](FakeGuest &g) { g.put32(kHero + 0x10, 0); }, AutosaveGuard::kDying,
         "the hero is dying (action 0)"},
        {"dead", [](FakeGuest &g) { g.put32(kHero + 0x10, 1); }, AutosaveGuard::kDying,
         "the hero is dead (action 1)"},
        {"a boss map", [](FakeGuest &g) {
             g.put32(kState + 0x84, 3);
             g.put32(kState + 0x88, 8);
         },
         AutosaveGuard::kNoSaveMap, "map 3.8, a boss map where the game cannot save"},
        {"jumping", [](FakeGuest &g) {
             g.put32(kHero + 0x10, 5);
             g.put32(kHero + 0x0C, 0x2);
         },
         AutosaveGuard::kHeroBusy, "the hero is busy: not standing, walking or climbing (action 5, flags 0x2)"},
        {"attacking", [](FakeGuest &g) { g.put32(kHero + 0x0C, 0x8); }, AutosaveGuard::kHeroBusy,
         "the hero is busy: not standing, walking or climbing (action 2, flags 0x8)"},
        {"attacking (6)", [](FakeGuest &g) { g.put32(kHero + 0x10, 6); },
         AutosaveGuard::kHeroBusy,
         "the hero is busy: not standing, walking or climbing (action 6, flags 0x0)"},
        {"attacking a target (7)", [](FakeGuest &g) { g.put32(kHero + 0x10, 7); },
         AutosaveGuard::kHeroBusy,
         "the hero is busy: not standing, walking or climbing (action 7, flags 0x0)"},
        {"a skill (11)", [](FakeGuest &g) { g.put32(kHero + 0x10, 11); },
         AutosaveGuard::kHeroBusy,
         "the hero is busy: not standing, walking or climbing (action 11, flags 0x0)"},
        {"in the air while walking", [](FakeGuest &g) {
             g.put32(kHero + 0x10, 3);
             g.put32(kHero + 0x0C, 0x2);
         },
         AutosaveGuard::kHeroBusy,
         "the hero is busy: not standing, walking or climbing (action 3, flags 0x2)"},
    };
    for (const Case &k : cases) {
        INFO(k.what);
        FakeGuest g = field_game();
        k.change(g);
        const fe::AutosaveCheck c = fe::check_autosave(g.reader());
        CHECK(c.guard == k.want);
        CHECK(c.describe() == k.text);
        CHECK_FALSE(c.ok());
    }
}

TEST_CASE("autosave: the guards are checked in the game's order") {
    // A scene on a boss map with a popup and the hero dying: the mode comes first.
    FakeGuest g = field_game();
    g.put32(fe::kAutosaveGameMode, 9);
    g.put32(fe::kAutosavePopup, 0x68);
    g.put32(kHero + 0x10, 0);
    g.put32(kState + 0x84, 10);
    g.put32(kState + 0x88, 5);
    CHECK(guard_of(g) == AutosaveGuard::kScene);
    g.put32(fe::kAutosaveGameMode, 0);
    CHECK(guard_of(g) == AutosaveGuard::kPopup); // the death popup
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    CHECK(guard_of(g) == AutosaveGuard::kDying);
    g.put32(kHero + 0x10, 2);
    CHECK(guard_of(g) == AutosaveGuard::kNoSaveMap);
    g.put32(kState + 0x88, 4);
    CHECK(guard_of(g) == AutosaveGuard::kNone);
}

TEST_CASE("autosave: the 16 maps where the game cannot save, and no others") {
    const std::set<std::pair<int, int>> want = {
        {2, 8}, {2, 9}, {3, 8}, {4, 17}, {7, 4}, {9, 5},  {10, 5}, {2, 6},
        {2, 7}, {6, 1}, {6, 2}, {9, 9},  {9, 10}, {0, 5}, {4, 18}, {6, 3}};
    std::set<std::pair<int, int>> got;
    std::map<std::string, int> kinds;
    for (const fe::NoSaveMap &m : fe::no_save_maps()) {
        got.insert({m.area, m.map});
        kinds[m.kind]++;
    }
    CHECK(fe::no_save_maps().size() == 16);
    CHECK(got == want);
    CHECK(kinds == std::map<std::string, int>{
                       {"tutorial", 2}, {"boss", 5}, {"climb", 6}, {"Monster Carnival", 3}});
    // Every map of every area: refused exactly on those.
    for (int area = 0; area <= 10; area++) {
        for (int map = 0; map < 20; map++) {
            INFO(area << "." << map);
            FakeGuest g = field_game();
            g.put32(kState + 0x84, static_cast<u32>(area));
            g.put32(kState + 0x88, static_cast<u32>(map));
            const bool refused = want.count({area, map}) != 0;
            CHECK((guard_of(g) == AutosaveGuard::kNoSaveMap) == refused);
            CHECK((fe::find_no_save_map(area, map) != nullptr) == refused);
        }
    }
}

TEST_CASE("autosave: the setting is off by default, on_quit in the Enhanced preset") {
    const fe::Settings defaults;
    CHECK(defaults.autosave == fe::Autosave::kOff);
    const fe::SettingInfo *info = fe::find_setting("autosave");
    REQUIRE(info != nullptr);
    CHECK(info->group == fe::SettingGroup::kGameplay);
    CHECK(std::string(info->flag) == "--autosave");
    fe::Settings s;
    std::string why;
    CHECK(fe::apply_setting(&s, "autosave", "on_quit", &why));
    CHECK(s.autosave == fe::Autosave::kOnQuit);
    CHECK(fe::setting_text(s, *info) == "on_quit");
    CHECK(fe::apply_setting(&s, "autosave", "OFF", &why));
    CHECK(s.autosave == fe::Autosave::kOff);
    CHECK_FALSE(fe::apply_setting(&s, "autosave", "anywhere", &why));
    CHECK(s.autosave == fe::Autosave::kOff);
}

TEST_CASE("autosave: Lifecycle::leaving is the first queued resign or quit") {
    fe::Lifecycle l(true);
    CHECK_FALSE(l.leaving().has_value());
    l.handle(event(SDL_EVENT_WINDOW_MINIMIZED), true);
    REQUIRE(l.leaving().has_value());
    CHECK(l.leaving()->message == fe::AppMessage::kWillResignActive);
    CHECK(l.leaving()->cause == SDL_EVENT_WINDOW_MINIMIZED);
    CHECK(l.leaving()->scripted);
    l.handle(event(SDL_EVENT_WINDOW_RESTORED), true);
    l.handle(event(SDL_EVENT_QUIT), true);
    CHECK(l.leaving()->message == fe::AppMessage::kWillResignActive); // still the first
    fe::Lifecycle q(true);
    q.handle(event(SDL_EVENT_WINDOW_CLOSE_REQUESTED));
    REQUIRE(q.leaving().has_value());
    CHECK(q.leaving()->message == fe::AppMessage::kWillTerminate);
    CHECK_FALSE(q.leaving()->scripted);
    // pause_unfocused's focus loss resigns too.
    fe::Lifecycle f(true);
    f.set_pause_unfocused(true);
    f.handle(event(SDL_EVENT_WINDOW_FOCUS_LOST), true);
    REQUIRE(f.leaving().has_value());
    CHECK(f.leaving()->cause == SDL_EVENT_WINDOW_FOCUS_LOST);
    // A focus loss without it is no leave.
    fe::Lifecycle n(true);
    n.handle(event(SDL_EVENT_WINDOW_FOCUS_LOST), true);
    CHECK_FALSE(n.leaving().has_value());
}

TEST_CASE("autosave: the Autosaver checks once per leave and turn, and only when on") {
    mt_cpu cpu{}; // no game: every check is "not in the game"
    fe::Lifecycle l(true);
    fe::Autosaver a;
    // Nothing to leave.
    CHECK_FALSE(a.before_deliver(&cpu, fe::Autosave::kOnQuit, l, 1, 0));
    CHECK(a.stats().skips == 0);
    l.handle(event(SDL_EVENT_QUIT), true);
    // Off: nothing is checked.
    CHECK_FALSE(a.before_deliver(&cpu, fe::Autosave::kOff, l, 2, 0));
    CHECK(a.stats().skips == 0);
    // On: checked (and refused: no game), once for the turn.
    CHECK_FALSE(a.before_deliver(&cpu, fe::Autosave::kOnQuit, l, 3, 0));
    CHECK(a.stats().skips == 1);
    CHECK(a.stats().last == AutosaveGuard::kNotInGame);
    CHECK_FALSE(a.before_deliver(&cpu, fe::Autosave::kOnQuit, l, 3, 0));
    CHECK(a.stats().skips == 1);
    CHECK(a.stats().saves == 0);
}
