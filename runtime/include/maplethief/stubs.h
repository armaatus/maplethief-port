/* Offline platform services (#52): the stubs that replace every online or Apple-only service
 * the game uses, so that it runs with no network and no Apple account.
 *
 *   GameKit         emulated offline: the local player is always authenticated, scores and
 *                   achievements persist to gamecenter.json in the data directory
 *                   (runtime/src/stubs/gamekit.cpp; host classes GKPlayer, GKLocalPlayer,
 *                   GKScore, GKAchievement, GKLeaderboard, GKLeaderboardViewController).
 *   StoreKit        a store that sells nothing (ADR 0034): products are found, every payment
 *                   ends as cancelled by the user, so the game's own failure path closes the
 *                   cash shop's "Please Wait..." view (runtime/src/stubs/storekit.cpp).
 *   CFSocket        the purchase server connection fails at once instead of after a 10 s
 *                   timeout (runtime/src/stubs/network.cpp, HLE imports).
 *   Reachability    SCNetworkReachability* answer "not reachable" (Chartboost only, never
 *                   reached while +[Chartboost sharedChartboost] returns nil).
 *   Chartboost      the host override +[Chartboost sharedChartboost] -> nil (classes.cpp).
 *
 * Completion handlers and delegate callbacks are never called inside the request: like the
 * real frameworks, they are delivered later, by mt_stubs_pump, which the run loop calls once
 * per turn (docs/re/timing-threading.md). The game itself then marshals GameKit results to
 * the main queue with dispatch_async.
 *
 * C17 that also compiles as C++20. */
#ifndef MAPLETHIEF_STUBS_H
#define MAPLETHIEF_STUBS_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- deferred deliveries ------------------------------------------------------------------ */

/* Delivers the completion handlers and delegate callbacks that were posted before this call,
 * oldest first, on `cpu` (the guest's main thread), each inside its own autorelease pool.
 * Deliveries posted while it runs wait for the next call, like sources of the next run-loop
 * turn. Returns the number delivered. Call it once per run-loop turn. */
uint32_t mt_stubs_pump(mt_cpu *cpu) MT_NOEXCEPT;

/* The number of deliveries waiting for mt_stubs_pump. */
uint32_t mt_stubs_pending(void) MT_NOEXCEPT;

/* Drops every waiting delivery without running it, and the per-runtime state (the local
 * player, the payment queue). A new current ObjC runtime (mt_objc_set_current) does this by
 * itself on first use. */
void mt_stubs_reset(void) MT_NOEXCEPT;

/* --- where the stubs keep their files ------------------------------------------------------ */

/* The directory the stubs write their files to (gamecenter.json). Chosen in this order:
 * mt_stubs_set_data_dir, the MAPLETHIEF_DATA_DIR environment variable, then the per-user
 * application data directory of the OS:
 *
 *   macOS, iOS  $HOME/Library/Application Support/MapleThief
 *   Windows     %APPDATA%\MapleThief
 *   Linux       $XDG_DATA_HOME/maplethief, else $HOME/.local/share/maplethief
 *
 * The platform layer may pass SDL_GetPrefPath's answer instead. Created (with its parents)
 * when a file is first written. Never NULL; "" if no directory can be determined. */
const char *mt_stubs_data_dir(void) MT_NOEXCEPT;

/* Sets the data directory (copied); NULL or "" goes back to the default above. Returns 0, or
 * -1 if the path is too long. */
int mt_stubs_set_data_dir(const char *dir) MT_NOEXCEPT;

/* The file GameKit persists scores and achievements to: <data dir>/gamecenter.json. Writes at
 * most `size` bytes (NUL included) to `out`; returns 0, or -1 if it does not fit. */
int mt_stubs_gamecenter_path(char *out, uint32_t size) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_STUBS_H */
