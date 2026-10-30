/* Offline platform services (#52): the stubs that replace every online or Apple-only service
 * the game uses, so that it runs with no network and no Apple account.
 *
 *   GameKit         emulated offline: the local player is always authenticated, scores and
 *                   achievements persist to gamecenter.json in the data directory
 *                   (runtime/src/stubs/gamekit.cpp; host classes GKPlayer, GKLocalPlayer,
 *                   GKScore, GKAchievement, GKLeaderboard, GKLeaderboardViewController).
 *                   The scores are also kept per player (the save slot) for the local
 *                   leaderboard, which a host hook shows (#154, ADR 0084).
 *   StoreKit        by default a store that sells nothing (ADR 0034): products are found,
 *                   every payment ends as cancelled by the user, so the game's own failure path
 *                   closes the cash shop's "Please Wait..." view. With a payment policy (the
 *                   offline cash shop, maplethief/cashshop.h, ADR 0072) a payment it accepts
 *                   completes with a synthetic receipt (runtime/src/stubs/storekit.cpp).
 *   CFSocket        the purchase server connection fails at once instead of after a 10 s
 *                   timeout, unless a receipt list holds a synthetic receipt: then a local fake
 *                   server confirms it (ADR 0072; runtime/src/stubs/network.cpp, HLE imports).
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

/* --- StoreKit's payments (ADR 0034, ADR 0072) --------------------------------------------- */

/* Decides a payment of product `product_id` (com.nexonmobile.maplestory.thief.*), called on the
 * guest thread inside -[SKPaymentQueue addPayment:] (it may call guest functions, e.g. to take
 * the price). 1: the payment completes (SKPaymentTransactionStatePurchased) on the next pump with
 * a synthetic receipt; 0: it is cancelled, as without a policy. */
typedef int (*mt_storekit_policy_fn)(void *user, mt_cpu *cpu, const char *product_id);

/* Installs the policy (NULL: none, every payment is cancelled; the default). Kept across
 * processes. */
void mt_storekit_set_policy(mt_storekit_policy_fn fn, void *user) MT_NOEXCEPT;

/* The synthetic receipt: this prefix and the product id, NUL-terminated text (the game reads
 * receipts with strlen). Only the port writes it; a real App Store receipt never starts so. */
#define MT_STUBS_RECEIPT_PREFIX "maplethief-offline 1 "

/* Writes the receipt of `product_id` to out[0..size) (NUL-terminated). Returns its length
 * without the NUL, or 0 if it does not fit. */
uint32_t mt_stubs_receipt_text(const char *product_id, char *out, uint32_t size) MT_NOEXCEPT;

/* The product id a synthetic receipt names (up to its NUL, a blank or its end), to
 * out[0..size); 1, or 0 for any other bytes (a real receipt). */
int mt_stubs_receipt_product(const uint8_t *bytes, uint32_t size, char *out,
                             uint32_t out_size) MT_NOEXCEPT;

/* --- the fake receipt server (ADR 0072) ------------------------------------------------------ */

typedef struct mt_stubs_server_stats {
    uint32_t connects;  /* CFSocketConnectToAddress calls */
    uint32_t accepted;  /* of those, connected to the fake server */
    uint32_t frames;    /* client frames read */
    uint32_t answers;   /* server frames delivered through the socket's callout */
    uint32_t confirmed; /* op 3 / op 5 answers with status 0 (a product granted) */
    uint32_t rejected;  /* op 3 / op 5 answers with a failure status (a real receipt) */
} mt_stubs_server_stats;

mt_stubs_server_stats mt_stubs_get_server_stats(void) MT_NOEXCEPT;

/* --- the local Game Center (#154, ADR 0084) -------------------------------------------------- */

/* Every reported score is also kept per player: the local player's alias at the report (the
 * frontend sets "Slot N" from the chosen save slot, maplethief/offline.h). gamecenter.json then
 * has "entries": [{"category", "player", "best", "last", "reports"}] next to "scores" (still
 * format version 1: readers ignore keys they don't know). */

/* The alias the next scores are reported under (copied; at most MT_GAMEKIT_NAME_MAX - 1 bytes).
 * NULL or "" goes back to "Player". Kept across processes. */
#define MT_GAMEKIT_NAME_MAX 64u
void mt_gamekit_set_player_alias(const char *alias) MT_NOEXCEPT;
const char *mt_gamekit_player_alias(void) MT_NOEXCEPT;

typedef struct mt_gamekit_entry {
    char category[128];
    char player[MT_GAMEKIT_NAME_MAX];
    int64_t best, last;
    int64_t reports;
} mt_gamekit_entry;

/* The saved entries of `category` (NULL: every category), best score first (then the player's
 * name), read from gamecenter.json: writes up to `max` to `out` and returns how many there are. */
uint32_t mt_gamekit_entries(const char *category, mt_gamekit_entry *out, uint32_t max) MT_NOEXCEPT;

/* The leaderboard screen. When the game presents its GKLeaderboardViewController (-[EAGLView
 * viewGameCenterRank:]), -viewDidAppear: calls the hook with the controller's category, and the
 * controller stays presented until mt_gamekit_leaderboard_close; then its delegate gets
 * -leaderboardViewControllerDidFinish: on the next pump and dismisses it. Without a hook (the
 * default) it finishes at once (ADR 0034). Called on the guest thread. Kept across processes. */
typedef void (*mt_gamekit_leaderboard_fn)(void *user, const char *category);
void mt_gamekit_set_leaderboard_hook(mt_gamekit_leaderboard_fn fn, void *user) MT_NOEXCEPT;
/* 1 while a leaderboard controller waits for mt_gamekit_leaderboard_close. */
int mt_gamekit_leaderboard_is_open(void) MT_NOEXCEPT;
/* Finishes the waiting controller (nothing if none). On the guest thread, between turns. */
void mt_gamekit_leaderboard_close(void) MT_NOEXCEPT;

/* An achievement from the host (the local achievements, maplethief/offline.h): saved as
 * -reportAchievementWithCompletionHandler: saves one (the highest percentage wins, clamped to
 * 0-100). Returns the percentage saved before (0 for a new one), or -1 if it cannot be saved. */
double mt_gamekit_report_achievement(const char *identifier, double percent) MT_NOEXCEPT;
/* The saved percentage of `identifier` (0 if none). Reads the file. */
double mt_gamekit_achievement_percent(const char *identifier) MT_NOEXCEPT;

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
