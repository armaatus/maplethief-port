/* Game events (#72, ADR 0053): what happens to the hero, as the host wants to hear of it (a
 * gamepad's rumble), seen at the entry of the engine's own functions. It knows nothing of SDL.
 *
 *   hero damage   CHero::TakeDamage(hero, damage, from_left) 0x65B55, the one function that
 *                 takes a hit off the hero's HP (+0x94): the mob-contact check 0x65F05 and the
 *                 attack-hit check 0x65DC5 call it. An event when damage > 0 and the hero is
 *                 not dead (flags +0x0C bit 0x10 clear), the function's own guard (a damage of
 *                 0 or less is a "miss"). `value` is the damage asked for (a guard skill may
 *                 halve what is taken off).
 *   level-up      CHero::LevelUp(hero) 0x65089, from CHero::setExp 0x6654D (once per level
 *                 gained) and an item effect 0x66695. An event when the level (+0x80) is below
 *                 99, the function's own guard. `value` is the new level.
 *
 * The recompiler reports the entry of these functions (recomp/observe.py, MT_FN_OBSERVE in
 * maplethief/recomp/calls.h) to mt_observe_entry, which only reads the registers and guest
 * memory: the guest runs exactly as without it. The game's own vibration (CMSoundPlayer::Vibrate
 * at the start of every hit, while its vibration option is on) stays the audio module's vibrate
 * hook (maplethief/audio.h); the area hazard of 0x65F05 that takes 300 HP directly (areas 10
 * and 11) is no event.
 *
 * Single guest thread: the hook is called on the guest thread, inside the guest's call. It must
 * not call back into the guest. Trace: MAPLETHIEF_TRACE=events. C17 that also compiles as
 * C++20; the implementation is runtime/src/events/. */
#ifndef MAPLETHIEF_EVENTS_H
#define MAPLETHIEF_EVENTS_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

typedef enum mt_game_event_kind {
    MT_GAME_EVENT_HERO_DAMAGE = 1, /* value: the damage */
    MT_GAME_EVENT_LEVEL_UP = 2,    /* value: the new level */
} mt_game_event_kind;

typedef struct mt_game_event {
    uint32_t kind;  /* mt_game_event_kind */
    uint32_t hero;  /* the CHero (guest address) */
    int32_t value;  /* see the kind */
    int32_t hp;     /* the hero's HP (+0x94) at the entry */
    int32_t max_hp; /* the hero's max HP (+0x9C) at the entry */
} mt_game_event;

typedef void (*mt_game_event_fn)(void *user, const mt_game_event *event);

/* Installs the host's hook (NULL: none, the default: events are only counted). Kept across
 * processes. */
void mt_game_events_set_hook(mt_game_event_fn hook, void *user) MT_NOEXCEPT;

typedef struct mt_game_events_stats {
    uint64_t entries;     /* observed entries (mt_observe_entry calls) */
    uint64_t hero_damage; /* damage events */
    uint64_t level_ups;   /* level-up events */
    uint64_t ignored;     /* entries the function's guard turns away (a miss, a dead hero, 99) */
} mt_game_events_stats;

/* The numbers since the start of the host process (or the last reset). */
mt_game_events_stats mt_game_events_get_stats(void) MT_NOEXCEPT;
void mt_game_events_reset_stats(void) MT_NOEXCEPT;

/* The observed functions' pointers (recomp/observe.py lists the same). */
#define MT_EVENTS_FN_LEVEL_UP 0x65089u
#define MT_EVENTS_FN_TAKE_DAMAGE 0x65B55u

/* mt_observe_entry(cpu, fn) is declared in maplethief/recomp/calls.h: the generated code's ABI.
 * Tests call it as the generated code does. */

MT_EXTERN_C_END

#endif /* MAPLETHIEF_EVENTS_H */
