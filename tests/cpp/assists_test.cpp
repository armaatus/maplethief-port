// The quality-of-life Assists (#90, assists.h, ADR 0092) on a fake guest: the hero, its quick
// slots and items laid out as CHero::KeyAttack reads them. Turbo attack's release-and-press
// pairs (only in the field, only while attack is held, the hold always given back); auto-potion
// (the threshold, the slot found, the cooldown, nothing while dead, in a popup or a menu); the
// rows; MAPLETHIEF_SETTINGS' list. The game's side (the press reaching CHero::KeyAttack) is
// play_real_test's turbo_attack and auto_potion modes (real assets).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "assists.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/recomp/calls.h"
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

// The presses, and the runtime's hold counts they make (as mt_input_press / release count).
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
    fe::Assists::Held held() {
        return [this](u32 action) { return holds[action]; };
    }
};

const fe::PlayerHolds kNone{};
const fe::PlayerHolds kAttack{.attack = true};

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

TEST_CASE("assists: auto_potion taps the slot under the threshold, then waits its cooldown") {
    FakeGuest g = field_game();
    Recorder rec;
    fe::Assists a(rec.press(), rec.held());
    fe::Settings s;
    std::uint64_t tick = 100;
    const auto slot = [](u32 action, bool down) { return std::pair<u32, bool>{action, down}; };
    // Off: nothing, whatever the HP.
    g.put32(kHero + fe::kHeroHp, 5);
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    CHECK(rec.presses.empty());
    // 50 %: HP 30 of 50 is above.
    s.auto_potion = fe::AutoPotion::k50;
    g.put32(kHero + fe::kHeroHp, 30);
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    CHECK(rec.presses.empty());
    // HP 20: slot 1 pressed, released kPotionHoldTicks later (a tap).
    g.put32(kHero + fe::kHeroHp, 20);
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    REQUIRE(rec.presses.size() == 1);
    CHECK(rec.presses[0] == slot(MT_ACTION_SLOT1, true));
    a.turn(g.reader(), s, kNone, 0, tick + fe::kPotionHoldTicks - 1, true, 0);
    CHECK(rec.presses.size() == 1);
    tick += fe::kPotionHoldTicks;
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    REQUIRE(rec.presses.size() == 2);
    CHECK(rec.presses[1] == slot(MT_ACTION_SLOT1, false));
    CHECK(a.stats().potions == 1);
    CHECK(a.stats().last_slot == 1);
    // Within the cooldown: nothing, though HP is still low.
    a.turn(g.reader(), s, kNone, 0, tick + fe::kPotionCooldownTicks - 1, true, 0);
    CHECK(rec.presses.size() == 2);
    // After it: again.
    tick += fe::kPotionCooldownTicks;
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    a.turn(g.reader(), s, kNone, 0, tick + fe::kPotionHoldTicks, true, 0);
    CHECK(rec.presses.size() == 4);
    tick += 10 * fe::kPotionCooldownTicks;
    // Not the field panel (a menu), a popup, dead, no potion, something queued, the player
    // walking or holding the slot: nothing.
    a.turn(g.reader(), s, kNone, 7, tick, true, 0);
    a.turn(g.reader(), s, kNone, 0, tick, false, 0);
    a.turn(g.reader(), s, fe::PlayerHolds{.walk = true}, 0, tick, true, 0);
    fe::PlayerHolds slot1;
    slot1.slot[1] = true;
    a.turn(g.reader(), s, slot1, 0, tick, true, 0);
    g.put32(fe::kAutosavePopup, 2);
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(kHero + 0x10, 0); // dying
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    g.put32(kHero + 0x10, 2);
    put_slot(g, 1, 0, 0);
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    CHECK(rec.presses.size() == 4);
    // The potion on slot 3: that slot's action.
    put_slot(g, 3, 1, 3);
    a.turn(g.reader(), s, kNone, 0, tick, true, 0);
    a.turn(g.reader(), s, kNone, 0, tick + fe::kPotionHoldTicks, true, 0);
    REQUIRE(rec.presses.size() == 6);
    CHECK(rec.presses[4] == slot(MT_ACTION_SLOT3, true));
    CHECK(rec.presses[5] == slot(MT_ACTION_SLOT3, false));
}

TEST_CASE("assists: auto_potion lets a held attack go for its tap, turbo gives it back") {
    FakeGuest g = field_game();
    g.put32(kHero + fe::kHeroHp, 5);
    const auto ev = [](u32 action, bool down) { return std::pair<u32, bool>{action, down}; };
    for (const bool turbo : {false, true}) {
        CAPTURE(turbo);
        Recorder rec;
        fe::Assists a(rec.press(), rec.held());
        fe::Settings s;
        s.auto_potion = fe::AutoPotion::k25;
        s.turbo_attack = turbo;
        rec.holds[MT_ACTION_ATTACK] = 1; // the player's
        a.turn(g.reader(), s, kAttack, 0, 100, true, 0);
        REQUIRE(rec.presses.size() == 2);
        CHECK(rec.presses[0] == ev(MT_ACTION_ATTACK, false));
        CHECK(rec.presses[1] == ev(MT_ACTION_SLOT1, true));
        // No turbo pulse during the tap.
        a.turn(g.reader(), s, kAttack, 0, 100 + fe::kPotionHoldTicks - 1, true, 0);
        CHECK(rec.presses.size() == 2);
        a.turn(g.reader(), s, kAttack, 0, 100 + fe::kPotionHoldTicks, true, 0);
        CHECK(rec.presses[2] == ev(MT_ACTION_SLOT1, false));
        if (turbo) {
            REQUIRE(rec.presses.size() == 4);
            CHECK(rec.presses[3] == ev(MT_ACTION_ATTACK, true));
            CHECK(rec.holds[MT_ACTION_ATTACK] == 1);
        } else {
            CHECK(rec.presses.size() == 3); // a held attack does nothing more: left let go
            CHECK(rec.holds[MT_ACTION_ATTACK] == 0);
        }
    }
    // The player pressed attack again during the tap: not pressed a second time.
    Recorder rec;
    fe::Assists a(rec.press(), rec.held());
    fe::Settings s;
    s.auto_potion = fe::AutoPotion::k25;
    s.turbo_attack = true;
    rec.holds[MT_ACTION_ATTACK] = 1;
    a.turn(g.reader(), s, kAttack, 0, 100, true, 0);
    rec.holds[MT_ACTION_ATTACK] = 1; // Actions' new press
    a.turn(g.reader(), s, kAttack, 0, 100 + fe::kPotionHoldTicks, true, 0);
    CHECK(rec.holds[MT_ACTION_ATTACK] == 1);
}

TEST_CASE("assists: turbo_attack releases and presses a held attack every few ticks") {
    FakeGuest g = field_game();
    Recorder rec;
    fe::Assists a(rec.press());
    fe::Settings s;
    // Off: a held attack is left alone.
    for (std::uint64_t t = 0; t < 20; t++) {
        a.turn(g.reader(), s, kAttack, 0, t, true, 0);
    }
    CHECK(rec.presses.empty());
    s.turbo_attack = true;
    // The player's press starts it at tick 100; every kTurboTicks a release then a press.
    for (std::uint64_t t = 100; t <= 100 + 3 * fe::kTurboTicks; t++) {
        a.turn(g.reader(), s, kAttack, 0, t, true, 0);
    }
    REQUIRE(rec.presses.size() == 6);
    for (std::size_t i = 0; i < rec.presses.size(); i += 2) {
        CHECK(rec.presses[i] == std::pair<u32, bool>{MT_ACTION_ATTACK, false});
        CHECK(rec.presses[i + 1] == std::pair<u32, bool>{MT_ACTION_ATTACK, true});
    }
    CHECK(a.stats().turbo_presses == 3);
    // Something waiting to be delivered: no new pair until it has gone.
    a.turn(g.reader(), s, kAttack, 0, 200, false, 0);
    a.turn(g.reader(), s, kAttack, 0, 300, false, 0);
    CHECK(rec.presses.size() == 6);
    a.turn(g.reader(), s, kAttack, 0, 301, true, 0);
    CHECK(rec.presses.size() == 8);
    // Let go: nothing; pressed again: its own press first, the repeat kTurboTicks later.
    a.turn(g.reader(), s, kNone, 0, 400, true, 0);
    a.turn(g.reader(), s, kAttack, 0, 401, true, 0);
    a.turn(g.reader(), s, kAttack, 0, 401 + fe::kTurboTicks - 1, true, 0);
    CHECK(rec.presses.size() == 8);
    a.turn(g.reader(), s, kAttack, 0, 401 + fe::kTurboTicks, true, 0);
    CHECK(rec.presses.size() == 10);
    // In a menu panel, a popup, the popup menu, or dead: never (attack is OK there).
    const std::size_t before = rec.presses.size();
    for (std::uint64_t t = 500; t < 540; t++) {
        a.turn(g.reader(), s, kAttack, 1, t, true, 0);
    }
    g.put32(fe::kAutosavePopup, 0x68);
    for (std::uint64_t t = 540; t < 580; t++) {
        a.turn(g.reader(), s, kAttack, 0, t, true, 0);
    }
    g.put32(fe::kAutosavePopup, 0xFFFFFFFFu);
    g.put32(fe::kAutosaveGameMode, 1);
    for (std::uint64_t t = 580; t < 620; t++) {
        a.turn(g.reader(), s, kAttack, 0, t, true, 0);
    }
    g.put32(fe::kAutosaveGameMode, 0);
    g.put32(kHero + 0x10, 1);
    for (std::uint64_t t = 620; t < 660; t++) {
        a.turn(g.reader(), s, kAttack, 0, t, true, 0);
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
    a.turn(g.reader(), s, kAttack, 0, 0, true, 0);
    a.turn(g.reader(), s, kAttack, 0, fe::kTurboTicks, true, 0);
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

// --- auto_loot (#175) ----------------------------------------------------------------------------

namespace {

// A guest memory with the field, the hero and a stack, for the hook on 0x6CF01.
struct LootGuest {
    static constexpr u32 kSize = 0x200000;
    static constexpr u32 kCanvas = 0x180000, kState = 0x181000, kHero = 0x182000, kSp = 0x1F0000;
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(kSize, 0);
    mt_cpu cpu{};

    LootGuest() {
        cpu.mem = mem.data();
        cpu.mem_size = kSize;
        put(fe::kAutosaveCanvas, kCanvas);
        put(kCanvas + 0x0C, kState);
        put(kState, fe::kAutosaveGameVptr);
        put(kState + 0x30, kHero);
        put(fe::kAutosaveGameMode, 0);
        put(fe::kAutosavePopup, 0xFFFFFFFFu);
        put(kHero + 0x10, 2); // standing
        put(kHero + fe::kHeroHp, 50);
        put(kHero + fe::kHeroMaxHp, 50);
    }
    void put(u32 addr, u32 v) { std::memcpy(mem.data() + addr, &v, 4); }
    u32 get(u32 addr) const {
        u32 v = 0;
        std::memcpy(&v, mem.data() + addr, 4);
        return v;
    }
    // A call of 0x6CF01 from `lr` with the drop's rect; the result.
    int call(u32 lr, const fe::LootBox &drop, int *handled = nullptr);
};

fe::GuestRead32 bounded_reader(const mt_cpu *cpu) {
    return [cpu](u32 addr, u32 *out) {
        if (addr == 0 || std::uint64_t{addr} + 4 > cpu->mem_size) {
            return false;
        }
        std::memcpy(out, cpu->mem + addr, 4);
        return true;
    };
}

std::int32_t g_panel = 0;
std::int32_t test_panel() { return g_panel; }

// The hero's box, 40 x 60 standing at (200, 300), as 0x6CE5D computes it.
constexpr fe::LootBox kHeroBox = {180, 241, 220, 301};
fe::LootBox g_seen;  // the rect the original was asked about
u32 g_originals = 0; // its calls

// The original 0x6CF01: computes the hero's box (+0x40) and tests the rect against it.
void fake_touches(mt_cpu *MT_RESTRICT cpu) {
    g_originals++;
    u32 bottom = 0;
    std::memcpy(&bottom, cpu->mem + cpu->r[13], 4);
    g_seen = {static_cast<std::int32_t>(cpu->r[1]), static_cast<std::int32_t>(cpu->r[2]),
              static_cast<std::int32_t>(cpu->r[3]), static_cast<std::int32_t>(bottom)};
    const std::int32_t box[4] = {kHeroBox.left, kHeroBox.top, kHeroBox.right, kHeroBox.bottom};
    std::memcpy(cpu->mem + cpu->r[0] + fe::kObjectBox, box, sizeof box);
    cpu->r[0] = fe::loot_boxes_meet(kHeroBox, g_seen) ? 1 : 0;
}

int LootGuest::call(u32 lr, const fe::LootBox &drop, int *handled) {
    cpu.r[0] = kHero;
    cpu.r[1] = static_cast<u32>(drop.left);
    cpu.r[2] = static_cast<u32>(drop.top);
    cpu.r[3] = static_cast<u32>(drop.right);
    cpu.r[13] = kSp;
    cpu.r[14] = lr;
    put(kSp, static_cast<u32>(drop.bottom));
    const int h = mt_hook_entry(&cpu, MT_HOOK_FN_OBJECT_TOUCHES, fake_touches);
    if (h == 0) {
        fake_touches(&cpu); // MT_FN_HOOK's tail call
    }
    if (handled != nullptr) {
        *handled = h;
    }
    CHECK(get(kSp) == static_cast<u32>(drop.bottom)); // the caller's argument, as it was
    return static_cast<int>(cpu.r[0]);
}

// A 16 x 16 drop lying with its foot at y 300, its centre at x.
fe::LootBox drop_at(std::int32_t x) { return {x - 8, 285, x + 8, 301}; }

} // namespace

TEST_CASE("assists: auto_loot's reach and the contact test's arithmetic") {
    const fe::LootBox d = drop_at(400);
    const fe::LootBox w = fe::auto_loot_reach(d);
    CHECK(w.left == d.left - fe::kAutoLootReachX);
    CHECK(w.right == d.right + fe::kAutoLootReachX);
    CHECK(w.top == d.top - fe::kAutoLootReachY);
    CHECK(w.bottom == d.bottom + fe::kAutoLootReachY);
    // 0x6CF01's test: half-open boxes; touching edges do not meet.
    CHECK(fe::loot_boxes_meet(kHeroBox, drop_at(200)));
    CHECK(fe::loot_boxes_meet(kHeroBox, {219, 300, 230, 310}));
    CHECK_FALSE(fe::loot_boxes_meet(kHeroBox, {220, 241, 230, 301})); // right of the hero
    CHECK_FALSE(fe::loot_boxes_meet(kHeroBox, {170, 241, 180, 301})); // left
    CHECK_FALSE(fe::loot_boxes_meet(kHeroBox, {180, 301, 220, 320})); // under
    CHECK_FALSE(fe::loot_boxes_meet(kHeroBox, {180, 200, 220, 241})); // over
}

TEST_CASE("assists: auto_loot widens the drop update's contact test, only there") {
    mt_hook_reset();
    g_panel = 0;
    LootGuest g;
    fe::install_auto_loot(true, bounded_reader, test_panel);
    REQUIRE(fe::auto_loot_installed());
    CHECK(mt_hook_enabled(MT_HOOK_FN_OBJECT_TOUCHES) == 1);
    const fe::AutoLootStats before = fe::auto_loot_stats();

    // A drop 100 points right of the hero: the game's own test says no, the wider one yes.
    int handled = -1;
    CHECK(g.call(fe::kDropContactReturn, drop_at(300), &handled) == 1);
    CHECK(handled == 1);
    CHECK(g_seen.left == drop_at(300).left - fe::kAutoLootReachX);
    CHECK(g_seen.bottom == drop_at(300).bottom + fe::kAutoLootReachY);
    CHECK(fe::auto_loot_stats().reached == before.reached + 1);
    // One under the hero: yes both ways, not counted as reached.
    CHECK(g.call(fe::kDropContactReturn, drop_at(200)) == 1);
    CHECK(fe::auto_loot_stats().reached == before.reached + 1);
    // Beyond the reach: no.
    CHECK(g.call(fe::kDropContactReturn, drop_at(220 + 8 + fe::kAutoLootReachX)) == 0);
    CHECK(g.call(fe::kDropContactReturn, drop_at(219 + 8 + fe::kAutoLootReachX)) == 1);
    CHECK(fe::auto_loot_stats().widened == before.widened + 4);

    // Every other caller of 0x6CF01 (the hero's, the mobs' touches) gets the original.
    for (const u32 lr : {0x65EABu, 0x660D7u, 0x763EBu, 0x767F5u, 0x76B8Du, 0x5BCF1u}) {
        CHECK(g.call(lr, drop_at(300), &handled) == 0);
        CHECK(handled == 0);
        CHECK(g_seen.left == drop_at(300).left);
    }
    CHECK(fe::auto_loot_stats().tests == before.tests + 4);
    fe::install_auto_loot(false);
    mt_hook_reset();
}

TEST_CASE("assists: auto_loot stays out of menus, popups and a dead hero's way") {
    mt_hook_reset();
    g_panel = 0;
    fe::install_auto_loot(true, bounded_reader, test_panel);
    const auto unwidened = [](LootGuest &g) {
        int handled = -1;
        const int hit = g.call(fe::kDropContactReturn, drop_at(300), &handled);
        return hit == 0 && handled == 0 && g_seen.left == drop_at(300).left;
    };
    {
        LootGuest g;
        g.put(fe::kAutosavePopup, 0x68); // the death popup
        CHECK(unwidened(g));
    }
    {
        LootGuest g;
        g.put(fe::kAutosaveGameMode, 1); // the popup menu
        CHECK(unwidened(g));
    }
    {
        LootGuest g;
        g.put(LootGuest::kHero + 0x10, 1); // dead
        CHECK(unwidened(g));
    }
    {
        LootGuest g;
        g.put(LootGuest::kState, 0x119C98); // another state (CStateTitle)
        CHECK(unwidened(g));
    }
    {
        LootGuest g;
        g_panel = 1; // the key pad shows a menu's panel
        CHECK(unwidened(g));
        g_panel = 0;
        CHECK_FALSE(unwidened(g)); // back in the field
    }
    fe::install_auto_loot(false);
    mt_hook_reset();
}

TEST_CASE("assists: auto_loot off is the original contact test") {
    mt_hook_reset();
    g_panel = 0;
    fe::install_auto_loot(true, bounded_reader, test_panel);
    fe::install_auto_loot(false, bounded_reader, test_panel);
    CHECK_FALSE(fe::auto_loot_installed());
    CHECK(mt_hook_enabled(MT_HOOK_FN_OBJECT_TOUCHES) == 0);
    LootGuest g;
    const u32 calls = g_originals;
    int handled = -1;
    CHECK(g.call(fe::kDropContactReturn, drop_at(300), &handled) == 0);
    CHECK(handled == 0);
    CHECK(g_originals == calls + 1);
    CHECK(g.call(fe::kDropContactReturn, drop_at(200)) == 1);

    fe::Settings s;
    CHECK_FALSE(s.auto_loot);
    std::string why;
    REQUIRE(fe::apply_setting_list(&s, "auto_loot = on", &why));
    CHECK(s.auto_loot);
    const fe::SettingInfo *row = fe::find_setting("auto_loot");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kAssists);
    CHECK(row->applies == fe::Applies::kAtOnce);
    mt_hook_reset();
}
