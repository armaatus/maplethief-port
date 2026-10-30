// Rumble (#72, ADR 0053) with synthetic data only. The runtime's half: mt_observe_entry (what
// the generated code calls at the entry of CHero::TakeDamage and CHero::LevelUp) on a
// synthetic hero turns the entries into game events with the functions' own guards and never
// writes guest memory. The frontend's half: Rumble on SDL3 virtual joysticks whose motors
// record every call SDL makes to them: the game's vibration, a hit and a level-up rumble the
// active pad with the strength scaled, the requests of one turn merge into one rumble, off and
// strength 0 never rumble, no pad, a pad gone and a pad without motors are no crash. And the
// settings keys rumble and rumble_strength.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "audio/audio_internal.h"
#include "gamepad.h"
#include "input.h"
#include "maplethief/audio.h"
#include "maplethief/events.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/calls.h"
#include "rumble.h"
#include "settings.h"

#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using virtual_pad::Kind;
using virtual_pad::RumbleCall;

// --- the runtime: a synthetic hero ------------------------------------------------------------

std::vector<mt_game_event> g_events;
void *g_event_user = nullptr;
void record_event(void *user, const mt_game_event *e) {
    g_events.push_back(*e);
    g_event_user = user;
}

// Guest memory with a CHero on the heap: flags +0x0C, level +0x80, HP +0x94, max HP +0x9C.
struct Hero {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    u32 addr = 0;

    Hero() {
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        addr = mt_heap_calloc(mem, 1, 0x164);
        REQUIRE(addr != 0);
        set(0x80, 7);   // level
        set(0x94, 120); // HP
        set(0x9C, 150); // max HP
        g_events.clear();
        mt_game_events_set_hook(record_event, this);
        mt_game_events_reset_stats();
    }
    ~Hero() {
        mt_game_events_set_hook(nullptr, nullptr);
        mt_mem_destroy(mem);
    }
    Hero(const Hero &) = delete;
    Hero &operator=(const Hero &) = delete;

    void set(u32 off, i32 v) { std::memcpy(cpu.mem + addr + off, &v, 4); }
    std::vector<std::uint8_t> bytes() const { return {cpu.mem + addr, cpu.mem + addr + 0x164}; }
    // The generated code's call at the entry of `fn` with r0 = the hero, r1 = `arg`.
    void enter(u32 fn, u32 hero, u32 arg = 0) {
        cpu.r[0] = hero;
        cpu.r[1] = arg;
        cpu.r[2] = 1;
        mt_observe_entry(&cpu, fn);
    }
};

} // namespace

TEST_CASE("events: CHero::TakeDamage's entry is a damage event, with the function's guard") {
    Hero h;
    const std::vector<std::uint8_t> before = h.bytes();
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 25);
    REQUIRE(g_events.size() == 1);
    CHECK(g_events[0].kind == MT_GAME_EVENT_HERO_DAMAGE);
    CHECK(g_events[0].hero == h.addr);
    CHECK(g_events[0].value == 25);
    CHECK(g_events[0].hp == 120);
    CHECK(g_events[0].max_hp == 150);
    CHECK(g_event_user == &h);
    // A miss (0 or less) is no hit: the function shows "miss" and takes nothing.
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 0);
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, static_cast<u32>(-3));
    CHECK(g_events.size() == 1);
    // A dead hero (flags bit 0x10) takes nothing.
    h.set(0x0C, 0x10 | 0x2);
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 40);
    CHECK(g_events.size() == 1);
    h.set(0x0C, 0x2);
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 40);
    REQUIRE(g_events.size() == 2);
    CHECK(g_events[1].value == 40);
    // The observer reads, never writes.
    h.set(0x0C, 0);
    CHECK(h.bytes() == before);
    const mt_game_events_stats st = mt_game_events_get_stats();
    CHECK(st.entries == 5);
    CHECK(st.hero_damage == 2);
    CHECK(st.ignored == 3);
}

TEST_CASE("events: CHero::LevelUp's entry is a level-up to the next level, below 99") {
    Hero h;
    h.enter(MT_EVENTS_FN_LEVEL_UP, h.addr);
    REQUIRE(g_events.size() == 1);
    CHECK(g_events[0].kind == MT_GAME_EVENT_LEVEL_UP);
    CHECK(g_events[0].value == 8);
    h.set(0x80, 98);
    h.enter(MT_EVENTS_FN_LEVEL_UP, h.addr);
    REQUIRE(g_events.size() == 2);
    CHECK(g_events[1].value == 99);
    h.set(0x80, 99); // the function does nothing at 99
    h.enter(MT_EVENTS_FN_LEVEL_UP, h.addr);
    CHECK(g_events.size() == 2);
    CHECK(mt_game_events_get_stats().level_ups == 2);
}

TEST_CASE("events: a bad hero pointer, another function or no hook is no event and no crash") {
    Hero h;
    for (const u32 bad : {0u, 4u, 0x00800000u, 0xFFFFFFF0u, h.addr + 0x10000000u}) {
        h.enter(MT_EVENTS_FN_TAKE_DAMAGE, bad, 10);
        h.enter(MT_EVENTS_FN_LEVEL_UP, bad);
    }
    h.enter(0x225E5u, h.addr, 10); // Rand_Next: not observed
    CHECK(g_events.empty());
    CHECK(mt_game_events_get_stats().ignored == 11);
    mt_game_events_set_hook(nullptr, nullptr);
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 10);
    CHECK(g_events.empty());
    CHECK(mt_game_events_get_stats().hero_damage == 1); // still counted
}

// --- the frontend: Rumble on virtual pads -----------------------------------------------------

namespace {

// SDL's gamepads, Input, Actions (into a sink: no guest) and Gamepads, and a Rumble on them.
struct Rig {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Rumble rumble{&gamepads};

    Rig() {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
        actions.set_sink([](void *, u32, bool) {}, nullptr);
        actions.set_panel_source([](void *) -> i32 { return 0; }, nullptr);
    }
    ~Rig() {
        rumble.uninstall();
        rumble.stop();
        SDL_Quit();
    }
    Rig(const Rig &) = delete;
    Rig &operator=(const Rig &) = delete;

    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input.handle(e);
            gamepads.handle(e);
        }
    }
    // Plugs `pad` in and makes it the active one (a press and a release of A).
    void activate(virtual_pad::Pad &pad) {
        REQUIRE_MESSAGE(pad.ok(), pad.error);
        pump();
        pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
        pump();
        pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
        pump();
        REQUIRE(gamepads.active_id() == pad.id);
    }
};

Uint16 amp(float a, u32 strength) {
    return static_cast<Uint16>(std::lround(a * static_cast<float>(strength) / 100.0f * 65535.0f));
}
bool is(const RumbleCall &c, const fe::RumbleEffect &e, u32 strength) {
    return c.low == amp(e.low, strength) && c.high == amp(e.high, strength);
}

} // namespace

TEST_CASE("rumble: the game's vibration, a hit and a level-up rumble the active pad, scaled") {
    Rig r;
    virtual_pad::Pad pad(Kind::kXbox360, false, true);
    r.activate(pad);
    CHECK(r.rumble.on());
    CHECK(r.rumble.strength() == 100);

    struct Case {
        const char *what;
        fe::RumbleEffect effect;
        mt_game_event event;
    };
    const Case cases[] = {
        {"vibrate", fe::kRumbleVibrate, {}},
        {"hero damage", fe::kRumbleDamage, {MT_GAME_EVENT_HERO_DAMAGE, 0x1000000u, 12, 50, 60}},
        {"level-up", fe::kRumbleLevelUp, {MT_GAME_EVENT_LEVEL_UP, 0x1000000u, 3, 60, 60}},
    };
    for (const u32 strength : {100u, 50u, 1u}) {
        r.rumble.configure(true, strength);
        for (const Case &c : cases) {
            INFO(c.what << " at " << strength << "%");
            pad.rumbles.clear();
            if (c.event.kind == 0) {
                r.rumble.on_vibrate();
            } else {
                r.rumble.on_event(c.event);
            }
            CHECK(pad.rumbles.empty()); // sent at the turn
            r.rumble.turn();
            REQUIRE(pad.rumbles.size() == 1);
            CHECK(is(pad.rumbles[0], c.effect, strength));
            r.rumble.turn(); // nothing new
            CHECK(pad.rumbles.size() == 1);
            r.rumble.stop(); // the motors off: SDL's (0, 0)
            REQUIRE(pad.rumbles.size() == 2);
            CHECK(pad.rumbles[1].low == 0);
            CHECK(pad.rumbles[1].high == 0);
        }
    }
    CHECK(amp(fe::kRumbleVibrate.low, 50) == 22937);
    // The requests of one turn merge: a hit with the game's vibration on is one rumble, the
    // strongest of each motor.
    r.rumble.configure(true, 100);
    pad.rumbles.clear();
    r.rumble.on_event({MT_GAME_EVENT_HERO_DAMAGE, 0x1000000u, 5, 10, 10});
    r.rumble.on_vibrate();
    r.rumble.on_event({MT_GAME_EVENT_LEVEL_UP, 0x1000000u, 4, 10, 10});
    r.rumble.turn();
    REQUIRE(pad.rumbles.size() == 1);
    CHECK(pad.rumbles[0].low == amp(std::fmax(fe::kRumbleVibrate.low, fe::kRumbleDamage.low), 100));
    CHECK(pad.rumbles[0].high == amp(fe::kRumbleLevelUp.high, 100));
    CHECK(r.rumble.stats().failed == 0);
}

TEST_CASE("rumble: off, or at strength 0, never rumbles") {
    Rig r;
    virtual_pad::Pad pad(Kind::kPs4, false, true);
    r.activate(pad);
    for (const bool on : {false, true}) {
        r.rumble.configure(on, on ? 0u : 100u);
        for (int i = 0; i < 3; i++) {
            r.rumble.on_vibrate();
            r.rumble.on_event({MT_GAME_EVENT_HERO_DAMAGE, 0x1000000u, 5, 10, 10});
            r.rumble.on_event({MT_GAME_EVENT_LEVEL_UP, 0x1000000u, 2, 10, 10});
            r.rumble.turn();
        }
    }
    CHECK(pad.rumbles.empty());
    CHECK(r.rumble.stats().ignored == 18);
    CHECK(r.rumble.stats().sent == 0);
    // Pending requests are dropped when it is switched off before the turn.
    r.rumble.configure(true, 100);
    r.rumble.on_vibrate();
    r.rumble.configure(false, 100);
    r.rumble.configure(true, 100);
    r.rumble.turn();
    CHECK(pad.rumbles.empty());
}

TEST_CASE("rumble: through the runtime's hooks: AudioServicesPlaySystemSound and the events") {
    Rig r;
    virtual_pad::Pad pad(Kind::kXbox360, false, true);
    r.activate(pad);
    r.rumble.install();
    Hero h; // installs its own event hook: put Rumble's back
    r.rumble.install();
    // kSystemSoundID_Vibrate (0xFFF) through the HLE import, as CMSoundPlayer::Vibrate plays it.
    h.cpu.r[0] = 0xFFF;
    mt_hle_AudioServicesPlaySystemSound(&h.cpu, 0);
    r.rumble.turn();
    REQUIRE(pad.rumbles.size() == 1);
    CHECK(is(pad.rumbles[0], fe::kRumbleVibrate, 100));
    r.rumble.stop();
    pad.rumbles.clear();
    // Another system sound id is no vibration.
    h.cpu.r[0] = 1000;
    mt_hle_AudioServicesPlaySystemSound(&h.cpu, 0);
    r.rumble.turn();
    CHECK(pad.rumbles.empty());
    // The generated code's observer at CHero::TakeDamage and CHero::LevelUp.
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 30);
    r.rumble.turn();
    REQUIRE(pad.rumbles.size() == 1);
    CHECK(is(pad.rumbles[0], fe::kRumbleDamage, 100));
    r.rumble.stop();
    pad.rumbles.clear();
    h.enter(MT_EVENTS_FN_LEVEL_UP, h.addr);
    r.rumble.turn();
    REQUIRE(pad.rumbles.size() == 1);
    CHECK(is(pad.rumbles[0], fe::kRumbleLevelUp, 100));
    r.rumble.stop();
    pad.rumbles.clear();
    // A miss is no rumble; off is none either; uninstalled, nothing reaches Rumble.
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 0);
    r.rumble.turn();
    r.rumble.configure(false, 100);
    h.enter(MT_EVENTS_FN_TAKE_DAMAGE, h.addr, 30);
    h.cpu.r[0] = 0xFFF;
    mt_hle_AudioServicesPlaySystemSound(&h.cpu, 0);
    r.rumble.turn();
    r.rumble.configure(true, 100);
    r.rumble.uninstall();
    h.enter(MT_EVENTS_FN_LEVEL_UP, h.addr);
    h.cpu.r[0] = 0xFFF;
    mt_hle_AudioServicesPlaySystemSound(&h.cpu, 0);
    r.rumble.turn();
    CHECK(pad.rumbles.empty());
}

TEST_CASE("rumble: no pad, a pad not yet active, a pad gone and a pad without motors") {
    Rig r;
    // No pad at all.
    r.rumble.on_vibrate();
    r.rumble.turn();
    r.rumble.stop();
    CHECK(r.rumble.stats().no_pad == 1);
    // A pad plugged in but never pressed is not the active one: it does not rumble.
    virtual_pad::Pad idle(Kind::kXbox360, false, true);
    REQUIRE(idle.ok());
    r.pump();
    r.rumble.on_vibrate();
    r.rumble.turn();
    CHECK(idle.rumbles.empty());
    CHECK(r.rumble.stats().no_pad == 2);
    // Active, rumbling, then unplugged: the next requests and the stop find no pad.
    r.activate(idle);
    r.rumble.on_vibrate();
    r.rumble.turn();
    CHECK(idle.rumbles.size() == 1);
    idle.detach();
    r.pump();
    CHECK(r.gamepads.active() == nullptr);
    r.rumble.on_event({MT_GAME_EVENT_HERO_DAMAGE, 0x1000000u, 5, 10, 10});
    r.rumble.turn();
    r.rumble.stop();
    CHECK(r.rumble.stats().no_pad == 3);
    // A pad without motors: SDL_RumbleGamepad fails, counted, nothing else.
    virtual_pad::Pad plain(Kind::kPs5);
    r.activate(plain);
    r.rumble.on_vibrate();
    r.rumble.turn();
    r.rumble.on_vibrate();
    r.rumble.turn();
    CHECK(r.rumble.stats().failed == 2);
    CHECK(r.rumble.stats().sent == 1);
}

TEST_CASE("settings: rumble is on or off, rumble_strength 0 to 100 (#72)") {
    const fe::Settings defaults;
    CHECK(defaults.rumble);
    CHECK(defaults.rumble_strength == 100);
    fe::Settings s;
    CHECK(fe::parse_settings("rumble = Off\nRUMBLE_STRENGTH=35\n", &s).empty());
    CHECK_FALSE(s.rumble);
    CHECK(s.rumble_strength == 35);
    CHECK(fe::parse_settings("rumble = yes\nrumble_strength = 0\n", &s).empty());
    CHECK(s.rumble);
    CHECK(s.rumble_strength == 0);
    // Bad values: reported, the setting kept.
    s.rumble_strength = 60;
    for (const char *bad : {"101", "-1", "50%", "0.5", "", "strong"}) {
        INFO(bad);
        std::string why;
        CHECK_FALSE(fe::apply_setting(&s, "rumble_strength", bad, &why));
        CHECK(why.find("rumble_strength: '") == 0);
        CHECK(s.rumble_strength == 60);
    }
    std::string why;
    CHECK_FALSE(fe::apply_setting(&s, "rumble", "maybe", &why));
    CHECK(why == "rumble: 'maybe' is not on or off");
    CHECK(s.rumble);
    // Written with their comments, and read back.
    s.rumble = false;
    s.rumble_strength = 42;
    const std::string text = fe::serialise_settings(s);
    CHECK(text.find("\nrumble = off\n") != std::string::npos);
    CHECK(text.find("# how strongly, 0 to 100 (percent)\nrumble_strength = 42\n") !=
          std::string::npos);
    fe::Settings back;
    CHECK(fe::parse_settings(text, &back).empty());
    CHECK(back == s);
    CHECK(fe::serialise_settings(defaults).find("\nrumble = on\nrumble_strength = 100\n") ==
          std::string::npos); // each after its comment line
    CHECK(fe::serialise_settings(defaults).find("\nrumble = on\n") != std::string::npos);
}
