/* Offline polish (#154, ADR 0084): nothing on the title menu points at a service that no longer
 * exists, and Game Center's features work on this computer.
 *
 *   the dead online UI   two host overrides on MainMenuView (ADR 0026) hide the promo banner
 *                        (MainMenuAD.png, -clickADButton:) and its close button, More Games
 *                        (its MainMenuMoreGame.png image view and the -clickMoreGame: button)
 *                        and About's four invisible link buttons (-clickLinkURL1:-4:), all of
 *                        which open dead Nexon or App Store pages (docs/re/network.md#urls).
 *                        With MT_OFFLINE_HIDE_GAME_CENTER the Game Center button and its image
 *                        go too: without the local leaderboard it closes as soon as it opens.
 *                        A hidden view is not drawn, not hit and not focused (UIKit, #69).
 *   achievements         a table of local achievements, unlocked from the game events of the
 *                        observed functions (maplethief/events.h, ADR 0053) and kept in
 *                        gamecenter.json's achievements (maplethief/stubs.h), with a queue of
 *                        unlock notices for the frontend's toast. The local player's alias (the
 *                        leaderboard's rows) follows the chosen save slot.
 *
 * The local leaderboard itself is GameKit's (maplethief/stubs.h: mt_gamekit_*).
 *
 * Off (the defaults, the Original preset): no override, no listener, as the original offline.
 * Single guest thread. C17 that also compiles as C++20; the implementation is
 * runtime/src/offline/. */
#ifndef MAPLETHIEF_OFFLINE_H
#define MAPLETHIEF_OFFLINE_H

#include <stdint.h>

#include "maplethief/objc/objc.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- the dead online UI --------------------------------------------------------------------- */

enum mt_offline_ui_flags {
    /* The banner, its close button, More Games and About's four links. */
    MT_OFFLINE_HIDE_LINKS = 1u,
    /* The Game Center button and its image (when the local leaderboard is off). */
    MT_OFFLINE_HIDE_GAME_CENTER = 2u,
};

/* What the next boot hides (mt_offline_ui_flags; 0, the default: nothing). Kept across
 * processes; read when the boot installs the host overrides. */
void mt_offline_ui_configure(uint32_t flags) MT_NOEXCEPT;
uint32_t mt_offline_ui_flags(void) MT_NOEXCEPT;

/* Installs the overrides on `rt`'s MainMenuView when the flags ask for any (the boot calls it
 * after mt_objc_install_overrides): -initWithFrame: and
 * -addButtonWithImageName:selectedImageName:withFrame:withEdgeInsets:actionForTouchDown:
 * actionForTouchUpInside: each run the game's own method first. Returns the number installed:
 * 0 with no flags, or when the image has no MainMenuView (logged). */
uint32_t mt_offline_ui_install(mt_objc *rt) MT_NOEXCEPT;

typedef struct mt_offline_ui_stats {
    uint32_t installed;      /* overrides installed by the last mt_offline_ui_install */
    uint32_t buttons_hidden; /* buttons hidden since the start of the host process */
    uint32_t views_hidden;   /* image views hidden */
} mt_offline_ui_stats;
mt_offline_ui_stats mt_offline_ui_get_stats(void) MT_NOEXCEPT;

/* 1 if a MainMenuView button whose touch-up action is `selector` is hidden with `flags`. */
int mt_offline_ui_hides_action(uint32_t flags, const char *selector) MT_NOEXCEPT;

/* --- local achievements ----------------------------------------------------------------------- */

typedef struct mt_achievement {
    const char *identifier;  /* gamecenter.json's identifier, "maplethief.local.*" */
    const char *title;       /* "Rookie" */
    const char *description; /* "Reach level 10." */
} mt_achievement;

/* The table, in the order the list shows it. */
uint32_t mt_achievements_count(void) MT_NOEXCEPT;
/* Entry i, or NULL past the end. */
const mt_achievement *mt_achievement_at(uint32_t i) MT_NOEXCEPT;
/* 1 if achievement i is unlocked (gamecenter.json says 100 %). Reads the file. */
int mt_achievement_unlocked(uint32_t i) MT_NOEXCEPT;

/* Listens to the game events (on) or stops (off). On: a level-up to 10, 30, 50 or 70, a new
 * character, the death popup and a revive unlock their achievements, and a load or a new
 * character sets the local player's alias to "Slot N" (mt_gamekit_set_player_alias). Kept across
 * processes. */
void mt_achievements_install(int enabled) MT_NOEXCEPT;
int mt_achievements_installed(void) MT_NOEXCEPT;

/* Unlocks achievement i now (the listener's path; tests). Returns 1 if it was not unlocked
 * before (a notice is queued), 0 if it was, -1 if i is out of range or it cannot be saved. */
int mt_achievement_unlock(uint32_t i) MT_NOEXCEPT;

/* The oldest unlock notice not taken yet: writes its achievement's index and returns 1, or
 * returns 0. At most MT_ACHIEVEMENT_NOTICES wait; older ones are dropped. On the guest thread
 * (the frontend's turn hook). */
#define MT_ACHIEVEMENT_NOTICES 8u
int mt_achievement_next_notice(uint32_t *index) MT_NOEXCEPT;
/* Drops the waiting notices (tests). */
void mt_achievement_clear_notices(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_OFFLINE_H */
