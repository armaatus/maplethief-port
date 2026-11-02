// The scene skip (#174, scene_skip.h, ADR 0096) on a fake guest: a CStateGame in game mode 9
// with its speech box flag (+0x1E9) and its scene list's step. The rate while a scene plays, the
// taps of OK (only on the cut-scene panel, only while nothing is queued, released after
// kSceneTapTicks), nothing outside a scene or with the setting off, the row. The game's side
// (the scene's state the same skipped as watched) is play_real_test's skip_intros modes.
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "maplethief/input.h"
#include "scene_skip.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;

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

constexpr u32 kCanvas = 0x01000000, kState = 0x01001000, kList = 0x01002000;

// The engine in a scene (game mode 9) at `step`, a speech box up or not. +0x1E8's word: the
// byte at +0x1E8 (cmd 52's flag) set too, so only +0x1E9 is read as the box.
FakeGuest scene(bool box, u32 step = 12) {
    FakeGuest g;
    g.put32(fe::kAutosaveCanvas, kCanvas);
    g.put32(kCanvas + 0x0C, kState);
    g.put32(kState, fe::kAutosaveGameVptr);
    g.put32(fe::kAutosaveGameMode, 9);
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(kState + fe::kSceneBoxWord, 0x01u | (box ? 0x100u : 0u));
    g.put32(kState + fe::kSceneList, kList);
    g.put32(kList + fe::kSceneListStep, step);
    return g;
}

struct Recorder {
    std::vector<std::pair<u32, bool>> presses;
    fe::SceneSkip::Press press() {
        return [this](u32 action, bool down) { presses.emplace_back(action, down); };
    }
};

constexpr std::int32_t kPanel = fe::kScenePanel;

} // namespace

TEST_CASE("scene_skip: the view of a scene: game mode 9 in CStateGame, the box, the step") {
    const FakeGuest up = scene(true, 135);
    fe::SceneView v = fe::read_scene_view(up.reader());
    CHECK(v.scene);
    CHECK(v.box);
    CHECK(v.step == 135);
    v = fe::read_scene_view(scene(false).reader());
    CHECK(v.scene);
    CHECK_FALSE(v.box); // +0x1E8's own byte is not the box
    // Not a scene: the field, loading, a queued scene, a popup, another state, unmapped.
    for (u32 mode : {0u, 8u, 10u}) {
        FakeGuest g = scene(true);
        g.put32(fe::kAutosaveGameMode, mode);
        CHECK_FALSE(fe::read_scene_view(g.reader()).scene);
    }
    FakeGuest popup = scene(true);
    popup.put32(fe::kAutosavePopup, 8);
    CHECK_FALSE(fe::read_scene_view(popup.reader()).scene);
    FakeGuest other = scene(true);
    other.put32(kState, 0x119000);
    CHECK_FALSE(fe::read_scene_view(other.reader()).scene);
    CHECK_FALSE(fe::read_scene_view(FakeGuest{}.reader()).scene);
    // No list: still a scene, the step unknown.
    FakeGuest nolist = scene(false);
    nolist.put32(kState + fe::kSceneList, 0);
    v = fe::read_scene_view(nolist.reader());
    CHECK(v.scene);
    CHECK(v.step == -1);
}

TEST_CASE("scene_skip: the rate is the skip's only while a scene plays with the setting on") {
    Recorder rec;
    fe::SceneSkip skip(rec.press());
    CHECK(skip.rate() == 1);
    FakeGuest g = scene(false);
    skip.turn(g.reader(), false, kPanel, 10, true, 0.5);
    CHECK(skip.rate() == 1); // off
    skip.turn(g.reader(), true, kPanel, 11, true, 0.55);
    CHECK(skip.rate() == fe::kSceneSkipRate);
    CHECK(skip.skipping());
    CHECK(skip.stats().scenes == 1);
    // Turned off mid-scene: back to 1 at once.
    skip.turn(g.reader(), false, kPanel, 12, true, 0.6);
    CHECK(skip.rate() == 1);
    skip.turn(g.reader(), true, kPanel, 13, true, 0.65);
    CHECK(skip.stats().scenes == 2);
    // The scene ends (game mode 8, a map loading): 1; its ticks counted.
    g.put32(fe::kAutosaveGameMode, 8);
    skip.turn(g.reader(), true, kPanel, 40, true, 2.0);
    CHECK(skip.rate() == 1);
    CHECK(skip.stats().scene_ticks == (12 - 11) + (40 - 13));
    CHECK(rec.presses.empty()); // no box: no tap
}

TEST_CASE("scene_skip: a speech box gets a tap of OK, released kSceneTapTicks later") {
    Recorder rec;
    fe::SceneSkip skip(rec.press());
    FakeGuest g = scene(true);
    skip.turn(g.reader(), true, kPanel, 100, true, 1.0);
    REQUIRE(rec.presses.size() == 1);
    CHECK(rec.presses[0] == std::pair<u32, bool>{MT_ACTION_OK, true});
    // Held: no second press, no release before its ticks.
    skip.turn(g.reader(), true, kPanel, 100 + fe::kSceneTapTicks - 1, true, 1.05);
    CHECK(rec.presses.size() == 1);
    skip.turn(g.reader(), true, kPanel, 100 + fe::kSceneTapTicks, true, 1.1);
    REQUIRE(rec.presses.size() == 2);
    CHECK(rec.presses[1] == std::pair<u32, bool>{MT_ACTION_OK, false});
    // The release is still queued: no new tap until the queue is empty.
    skip.turn(g.reader(), true, kPanel, 103, false, 1.15);
    CHECK(rec.presses.size() == 2);
    // The box still up (its next two lines): another tap.
    skip.turn(g.reader(), true, kPanel, 104, true, 1.2);
    REQUIRE(rec.presses.size() == 3);
    CHECK(rec.presses[2] == std::pair<u32, bool>{MT_ACTION_OK, true});
    CHECK(skip.stats().taps == 2);
}

TEST_CASE("scene_skip: no tap off the cut-scene panel, while queued, without a box or off") {
    Recorder rec;
    fe::SceneSkip skip(rec.press());
    FakeGuest g = scene(true);
    skip.turn(g.reader(), true, 0, 1, true, 0.1);      // the game pad, not panel 3
    skip.turn(g.reader(), true, -1, 2, true, 0.2);     // no panel yet
    skip.turn(g.reader(), true, kPanel, 3, false, 0.3); // something queued
    CHECK(rec.presses.empty());
    FakeGuest none = scene(false);
    skip.turn(none.reader(), true, kPanel, 4, true, 0.4);
    CHECK(rec.presses.empty());
    fe::SceneSkip off(rec.press());
    off.turn(g.reader(), false, kPanel, 5, true, 0.5);
    CHECK(rec.presses.empty());
    // A field with a box flag left set (never in the game; the guard is game mode 9).
    FakeGuest field = scene(true);
    field.put32(fe::kAutosaveGameMode, 0);
    skip.turn(field.reader(), true, kPanel, 6, true, 0.6);
    CHECK(rec.presses.empty());
}

TEST_CASE("scene_skip: a tap is released even when the scene ends or the setting goes off") {
    Recorder rec;
    fe::SceneSkip skip(rec.press());
    FakeGuest g = scene(true);
    skip.turn(g.reader(), true, kPanel, 50, true, 1.0);
    REQUIRE(rec.presses.size() == 1);
    g.put32(fe::kAutosaveGameMode, 8);
    skip.turn(g.reader(), false, kPanel, 51, true, 1.05); // off and gone: still held
    CHECK(rec.presses.size() == 1);
    skip.turn(g.reader(), false, kPanel, 52, true, 1.1);
    REQUIRE(rec.presses.size() == 2);
    CHECK(rec.presses[1] == std::pair<u32, bool>{MT_ACTION_OK, false});
    skip.turn(g.reader(), false, kPanel, 60, true, 1.5);
    CHECK(rec.presses.size() == 2);
    CHECK(skip.rate() == 1);
}

TEST_CASE("scene_skip: the skip_intros row: Gameplay, off in every preset, at once") {
    const fe::SettingInfo *row = fe::find_setting("skip_intros");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kGameplay);
    CHECK(row->applies == fe::Applies::kAtOnce);
    CHECK(std::string(row->flag) == "--skip-intros");
    fe::Settings s;
    CHECK_FALSE(s.skip_intros);
    for (const char *v :
         {row->presets.original, row->presets.original_controller, row->presets.enhanced}) {
        REQUIRE(v != nullptr);
        CHECK(std::string(v) == "off");
    }
}
