// The quality-of-life Assists (#90, assists.h, ADR 0092) on a fake guest: the hero, its quick
// slots and items laid out as CHero::KeyAttack reads them. Turbo attack's release-and-press
// pairs (only in the field, only while attack is held, the hold always given back); auto-potion
// (the threshold, the slot found, the cooldown, nothing while dead, in a popup or a menu); the
// rows; MAPLETHIEF_SETTINGS' list. The game's side (the press reaching CHero::KeyAttack) is
// play_real_test's turbo_attack and auto_potion modes (real assets).
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "assists.h"
#include "maplethief/input.h"
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

constexpr u32 kCanvas = 0x01000000, kState = 0x01001000, kHero = 0x01002000, kItems = 0x01003000,
              kItemBase = 0x01004000;

// An item at `index` of the hero's 48: id, type, HP restored, count.
void put_item(FakeGuest &g, u32 index, u32 id, u32 type, u32 hp, u32 count) {
    const u32 item = kItemBase + 0x200 * index;
    g.put32(kItems + 4 * index, item);
    g.put32(item + fe::kItemId, id);
    g.put32(item + fe::kItemType, type);
    g.put32(item + fe::kItemHp, hp);
    g.put32(item + fe::kItemCount, count);
}
void put_slot(FakeGuest &g, u32 slot, u32 kind, u32 id) {
    g.put32(kHero + fe::kHeroQuickSlots + 8 * slot, kind);
    g.put32(kHero + fe::kHeroQuickSlots + 8 * slot + 4, id);
}

// A fresh character in the field: HP 50 of 50, Red Potion (id 3) on slot 1, Blue Potion (id 4,
// MP only) on slot 2, a skill on slot 3.
FakeGuest field_game() {
    FakeGuest g;
    g.put32(fe::kAutosaveCanvas, kCanvas);
    g.put32(kCanvas + 0x0C, kState);
    g.put32(kState, fe::kAutosaveGameVptr);
    g.put32(kState + 0x30, kHero);
    g.put32(fe::kAutosaveGameMode, 0);
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(kHero + 0x10, 2); // standing
    g.put32(kHero + fe::kHeroHp, 50);
    g.put32(kHero + fe::kHeroMaxHp, 50);
    g.put32(kHero + fe::kHeroItems, kItems);
    for (u32 i = 0; i < fe::kHeroItemCount; i++) {
        g.put32(kItems + 4 * i, 0);
    }
    put_item(g, 0, 4, 1, 0, 5);  // Blue Potion: MP only
    put_item(g, 1, 3, 1, 50, 5); // Red Potion
    put_slot(g, 0, 0, 0);
    put_slot(g, 1, 1, 3);
    put_slot(g, 2, 1, 4);
    put_slot(g, 3, 2, 1);
    put_slot(g, 4, 0, 0);
    return g;
}

struct Recorder {
    std::vector<std::pair<u32, bool>> presses;
    fe::Assists::Press press() {
        return [this](u32 action, bool down) { presses.emplace_back(action, down); };
    }
};

} // namespace

TEST_CASE("assists: the view finds the HP potion's quick slot as CHero::KeyAttack would") {
    FakeGuest g = field_game();
    fe::AssistView v = fe::read_assist_view(g.reader());
    CHECK(v.field);
    CHECK(v.alive);
    CHECK(v.hp == 50);
    CHECK(v.max_hp == 50);
    CHECK(v.potion_slot == 1);
    CHECK(v.potion_id == 3);
    CHECK(v.potion_hp == 50);
    CHECK(v.potion_count == 5);
    // Slot 1 emptied: the next slot holds MP only, slot 3 a skill: none.
    put_slot(g, 1, 0, 0);
    CHECK(fe::read_assist_view(g.reader()).potion_slot == 0);
    // The Red Potion on slot 4 instead.
    put_slot(g, 4, 1, 3);
    CHECK(fe::read_assist_view(g.reader()).potion_slot == 4);
    // None left: no slot.
    put_item(g, 1, 3, 1, 50, 0);
    CHECK(fe::read_assist_view(g.reader()).potion_slot == 0);
    // Not a potion (a scroll that adds max HP, type 3).
    put_item(g, 1, 3, 3, 50, 5);
    CHECK(fe::read_assist_view(g.reader()).potion_slot == 0);
    // A popup, the popup menu, another state, a dead hero.
    g = field_game();
    g.put32(fe::kAutosavePopup, 13);
    CHECK_FALSE(fe::read_assist_view(g.reader()).field);
    g = field_game();
    g.put32(fe::kAutosaveGameMode, 1);
    CHECK_FALSE(fe::read_assist_view(g.reader()).field);
    g = field_game();
    g.put32(kState, 0x119C98); // a CStateTitle
    CHECK(fe::read_assist_view(g.reader()).hero == 0);
    CHECK_FALSE(fe::read_assist_view(g.reader()).field);
    g = field_game();
    g.put32(kHero + 0x10, 1);
    CHECK_FALSE(fe::read_assist_view(g.reader()).alive);
    g = field_game();
    g.put32(kHero + fe::kHeroHp, 0);
    CHECK_FALSE(fe::read_assist_view(g.reader()).alive);
    // Unmapped memory: nothing.
    CHECK_FALSE(fe::read_assist_view(FakeGuest{}.reader()).field);
}

TEST_CASE("assists: hp_under compares with the percentage of max HP") {
    CHECK(fe::hp_under(12, 50, 25));
    CHECK_FALSE(fe::hp_under(13, 50, 25)); // 26 %
    CHECK(fe::hp_under(24, 50, 50));
    CHECK_FALSE(fe::hp_under(25, 50, 50));
    CHECK_FALSE(fe::hp_under(1, 50, 0));  // off
    CHECK_FALSE(fe::hp_under(0, 50, 75)); // dead
    CHECK_FALSE(fe::hp_under(10, 0, 75));
    CHECK(fe::hp_under(2000000000, 2147483647, 99)); // no overflow
}

TEST_CASE("assists: auto_potion presses the slot under the threshold, then waits its cooldown") {
    FakeGuest g = field_game();
    Recorder rec;
    fe::Assists a(rec.press());
    fe::Settings s;
    std::uint64_t tick = 100;
    // Off: nothing, whatever the HP.
    g.put32(kHero + fe::kHeroHp, 5);
    a.turn(g.reader(), s, false, 0, tick, true, 0);
    CHECK(rec.presses.empty());
    // 50 %: HP 30 of 50 is above.
    s.auto_potion = fe::AutoPotion::k50;
    g.put32(kHero + fe::kHeroHp, 30);
    a.turn(g.reader(), s, false, 0, tick, true, 0);
    CHECK(rec.presses.empty());
    // HP 20: slot 1 pressed and released once.
    g.put32(kHero + fe::kHeroHp, 20);
    a.turn(g.reader(), s, false, 0, tick, true, 0);
    REQUIRE(rec.presses.size() == 2);
    CHECK(rec.presses[0] == std::pair<u32, bool>{MT_ACTION_SLOT1, true});
    CHECK(rec.presses[1] == std::pair<u32, bool>{MT_ACTION_SLOT1, false});
    CHECK(a.stats().potions == 1);
    CHECK(a.stats().last_slot == 1);
    // Within the cooldown: nothing, though HP is still low.
    a.turn(g.reader(), s, false, 0, tick + fe::kPotionCooldownTicks - 1, true, 0);
    CHECK(rec.presses.size() == 2);
    // After it: again.
    a.turn(g.reader(), s, false, 0, tick + fe::kPotionCooldownTicks, true, 0);
    CHECK(rec.presses.size() == 4);
    tick += 10 * fe::kPotionCooldownTicks;
    // Not the field panel (a menu), a popup, dead, no potion: nothing.
    a.turn(g.reader(), s, false, 7, tick, true, 0);
    g.put32(fe::kAutosavePopup, 2);
    a.turn(g.reader(), s, false, 0, tick, true, 0);
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(kHero + 0x10, 0); // dying
    a.turn(g.reader(), s, false, 0, tick, true, 0);
    g.put32(kHero + 0x10, 2);
    put_slot(g, 1, 0, 0);
    a.turn(g.reader(), s, false, 0, tick, true, 0);
    CHECK(rec.presses.size() == 4);
    // The potion on slot 3: that slot's action.
    put_slot(g, 3, 1, 3);
    a.turn(g.reader(), s, false, 0, tick, true, 0);
    REQUIRE(rec.presses.size() == 6);
    CHECK(rec.presses[4] == std::pair<u32, bool>{MT_ACTION_SLOT3, true});
    CHECK(rec.presses[5] == std::pair<u32, bool>{MT_ACTION_SLOT3, false});
}

TEST_CASE("assists: turbo_attack releases and presses a held attack every few ticks") {
    FakeGuest g = field_game();
    Recorder rec;
    fe::Assists a(rec.press());
    fe::Settings s;
    // Off: a held attack is left alone.
    for (std::uint64_t t = 0; t < 20; t++) {
        a.turn(g.reader(), s, true, 0, t, true, 0);
    }
    CHECK(rec.presses.empty());
    s.turbo_attack = true;
    // The player's press starts it at tick 100; every kTurboTicks a release then a press.
    for (std::uint64_t t = 100; t <= 100 + 3 * fe::kTurboTicks; t++) {
        a.turn(g.reader(), s, true, 0, t, true, 0);
    }
    REQUIRE(rec.presses.size() == 6);
    for (std::size_t i = 0; i < rec.presses.size(); i += 2) {
        CHECK(rec.presses[i] == std::pair<u32, bool>{MT_ACTION_ATTACK, false});
        CHECK(rec.presses[i + 1] == std::pair<u32, bool>{MT_ACTION_ATTACK, true});
    }
    CHECK(a.stats().turbo_presses == 3);
    // Something waiting to be delivered: no new pair until it has gone.
    a.turn(g.reader(), s, true, 0, 200, false, 0);
    a.turn(g.reader(), s, true, 0, 300, false, 0);
    CHECK(rec.presses.size() == 6);
    a.turn(g.reader(), s, true, 0, 301, true, 0);
    CHECK(rec.presses.size() == 8);
    // Let go: nothing; pressed again: its own press first, the repeat kTurboTicks later.
    a.turn(g.reader(), s, false, 0, 400, true, 0);
    a.turn(g.reader(), s, true, 0, 401, true, 0);
    a.turn(g.reader(), s, true, 0, 401 + fe::kTurboTicks - 1, true, 0);
    CHECK(rec.presses.size() == 8);
    a.turn(g.reader(), s, true, 0, 401 + fe::kTurboTicks, true, 0);
    CHECK(rec.presses.size() == 10);
    // In a menu panel, a popup, the popup menu, or dead: never (attack is OK there).
    const std::size_t before = rec.presses.size();
    for (std::uint64_t t = 500; t < 540; t++) {
        a.turn(g.reader(), s, true, 1, t, true, 0);
    }
    g.put32(fe::kAutosavePopup, 0x68);
    for (std::uint64_t t = 540; t < 580; t++) {
        a.turn(g.reader(), s, true, 0, t, true, 0);
    }
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(fe::kAutosaveGameMode, 1);
    for (std::uint64_t t = 580; t < 620; t++) {
        a.turn(g.reader(), s, true, 0, t, true, 0);
    }
    g.put32(fe::kAutosaveGameMode, 0);
    g.put32(kHero + 0x10, 1);
    for (std::uint64_t t = 620; t < 660; t++) {
        a.turn(g.reader(), s, true, 0, t, true, 0);
    }
    CHECK(rec.presses.size() == before);
}

TEST_CASE("assists: the default sink keeps the runtime's hold through a turbo pair") {
    mt_input_configure(nullptr);
    FakeGuest g = field_game();
    fe::Assists a; // mt_input_press / mt_input_release
    fe::Settings s;
    s.turbo_attack = true;
    mt_input_press(MT_ACTION_ATTACK); // the player's
    a.turn(g.reader(), s, true, 0, 0, true, 0);
    a.turn(g.reader(), s, true, 0, fe::kTurboTicks, true, 0);
    CHECK(a.stats().turbo_presses == 1);
    CHECK(mt_input_held(MT_ACTION_ATTACK) == 1);
    // The press, then the pair's release and press, wait for the guest.
    CHECK(mt_input_get_stats().queued == 3);
    mt_input_release(MT_ACTION_ATTACK);
    CHECK(mt_input_held(MT_ACTION_ATTACK) == 0);
    mt_input_configure(nullptr);
}

TEST_CASE("assists: the rows and MAPLETHIEF_SETTINGS' list") {
    fe::Settings s;
    CHECK(s.ui_animations == fe::UiAnimations::kNormal);
    CHECK_FALSE(s.turbo_attack);
    CHECK(s.auto_potion == fe::AutoPotion::kOff);
    CHECK(fe::ui_animation_scale(fe::UiAnimations::kNormal) == 1.0);
    CHECK(fe::ui_animation_scale(fe::UiAnimations::kFast) == 0.25);
    CHECK(fe::ui_animation_scale(fe::UiAnimations::kInstant) == 0.0);
    CHECK(fe::auto_potion_percent(fe::AutoPotion::k75) == 75);
    std::string why;
    REQUIRE(fe::apply_setting_list(&s,
                                   " turbo_attack = on ;; auto_potion=50; ui_animations = "
                                   "instant;",
                                   &why));
    CHECK(s.turbo_attack);
    CHECK(s.auto_potion == fe::AutoPotion::k50);
    CHECK(s.ui_animations == fe::UiAnimations::kInstant);
    CHECK(fe::apply_setting_list(&s, "", &why));
    CHECK_FALSE(fe::apply_setting_list(&s, "turbo_attack = off; auto_potion = 10", &why));
    CHECK(why.find("'auto_potion = 10'") != std::string::npos);
    CHECK_FALSE(s.turbo_attack); // the part before it applied
    CHECK_FALSE(fe::apply_setting_list(&s, "turbo_attack", &why));
    CHECK(why.find("no '='") != std::string::npos);
    for (const char *key : {"ui_animations", "turbo_attack", "auto_potion"}) {
        const fe::SettingInfo *row = fe::find_setting(key);
        REQUIRE(row != nullptr);
        CHECK(row->applies == fe::Applies::kAtOnce);
    }
    CHECK(fe::find_setting("turbo_attack")->group == fe::SettingGroup::kAssists);
    CHECK(fe::find_setting("auto_potion")->group == fe::SettingGroup::kAssists);
    CHECK(fe::find_setting("ui_animations")->group == fe::SettingGroup::kVideo);
}
