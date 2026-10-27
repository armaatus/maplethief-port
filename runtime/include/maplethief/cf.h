/* CoreFoundation (#48, ADR 0032): the CF imports of the game, its main bundle, and the run
 * loop the platform layer drives.
 *
 * The 24 CoreFoundation functions and 12 data imports the game uses are HLE functions in
 * runtime/src/cf/, listed in the HLE registry. CF objects are guest Objective-C objects on
 * the runtime of ADR 0029 (CFStringRef and NSString * are the same object); their classes and
 * storage are in runtime/src/cf/cf_objects.h, which Foundation (#49) builds on.
 *
 * Configuration, once at start-up (before the guest runs), like the libc's:
 *
 *     mt_cf_config cfg = mt_cf_config_default();
 *     mt_cf_config_from_env(&cfg);          // MAPLETHIEF_BUNDLE_DIR, MAPLETHIEF_LANGUAGE
 *     cfg.bundle_dir = ".../work/ipa/Payload/MapleThief.app";
 *     mt_cf_configure(&cfg);
 *
 * The boot (ADR 0027) calls mt_cf_start after the ObjC runtime has its host classes; it
 * creates the CF constants (kCFBooleanTrue, kCFRunLoopCommonModes, ...) the data imports
 * point at.
 *
 * The run loop. iOS's main run loop is the app's frame loop: the game's only clock is one
 * CFRunLoopTimer on it (docs/re/program-flow.md). Here the platform layer (#54, #117) owns
 * the loop and runs one turn at a time:
 *
 *     for (;;) {
 *         ... platform events ...
 *         double next = mt_runloop_run_once(cpu, mt_cf_absolute_time());
 *         ... wait until `next` or the next event ...
 *     }
 *
 * A turn pushes an autorelease pool (ADR 0029), runs the per-turn sources, fires every due
 * timer once in fire-date order, drains GCD's main queue (mt_gcd_drain_main, ADR 0033), and
 * pops the pool. It returns when the loop next has work (a timer or a GCD deadline).
 *
 * C17 that also compiles as C++20; the implementation is C++. */
#ifndef MAPLETHIEF_CF_H
#define MAPLETHIEF_CF_H

#include <stdint.h>

#include "maplethief/objc/objc.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- configuration ------------------------------------------------------------------------ */

/* The environment variables mt_cf_config_from_env reads. */
#define MT_CF_BUNDLE_DIR_ENV "MAPLETHIEF_BUNDLE_DIR" /* host directory of MapleThief.app */
#define MT_CF_LANGUAGE_ENV "MAPLETHIEF_LANGUAGE"     /* "en" or "ko" */

/* The bundle's path as the guest sees it (NSBundle -bundlePath): the directory of
 * MT_BOOT_DEFAULT_EXECUTABLE. */
#define MT_CF_DEFAULT_BUNDLE_PATH                                                                  \
    "/var/mobile/Applications/00000000-0000-0000-0000-000000000000/MapleThief.app"

typedef struct mt_cf_config {
    /* Host directory of the app bundle, e.g. work/ipa/Payload/MapleThief.app; NULL: none
     * (every resource lookup fails, logged). */
    const char *bundle_dir;
    /* The bundle's guest path; NULL: MT_CF_DEFAULT_BUNDLE_PATH. */
    const char *bundle_path;
    /* The user's language, an ISO 639-1 code the bundle has an .lproj for: "en" (default)
     * or "ko". First in the preferred localizations (and in [NSLocale preferredLanguages],
     * #49). */
    char language[8];
} mt_cf_config;

/* No bundle, the default guest bundle path, language "en". */
mt_cf_config mt_cf_config_default(void) MT_NOEXCEPT;

/* Applies MAPLETHIEF_BUNDLE_DIR and MAPLETHIEF_LANGUAGE if set (the bundle directory string
 * is the environment's). Returns 0, or -1 (leaving the language unchanged) if the language is
 * not "en" or "ko". */
int mt_cf_config_from_env(mt_cf_config *cfg) MT_NOEXCEPT;

/* Makes a copy of `cfg` the configuration (NULL: the defaults). Not thread-safe; call at
 * start-up or between tests. */
void mt_cf_configure(const mt_cf_config *cfg) MT_NOEXCEPT;

/* The configured language ("en" or "ko"). */
const char *mt_cf_language(void) MT_NOEXCEPT;

/* --- start-up ----------------------------------------------------------------------------- */

/* Binds CF to the runtime `rt` (which has its host classes) and creates the objects of the
 * CF data imports placed since the last call (their init hooks only record them). Returns
 * the number of data imports filled. The CF functions bind to the current runtime by
 * themselves if this was not called (unit tests). */
uint32_t mt_cf_start(mt_cpu *cpu, mt_objc *rt) MT_NOEXCEPT;

/* --- time --------------------------------------------------------------------------------- */

/* Seconds from 2001-01-01 00:00:00 UTC (CFAbsoluteTime) to the Unix epoch. */
#define MT_CF_ABSOLUTE_TIME_1970 978307200.0
/* A fire date that never comes: CF's distant future (year 4001). */
#define MT_CF_DISTANT_FUTURE 63113904000.0

/* CFAbsoluteTimeGetCurrent: the libc clock (maplethief/hle/libc.h, the same as the guest's
 * gettimeofday) as seconds since 2001-01-01. */
double mt_cf_absolute_time(void) MT_NOEXCEPT;

/* --- the run loop ------------------------------------------------------------------------- */

/* A per-turn source: called once at the start of every turn, before the timers, inside the
 * turn's autorelease pool, on the guest thread `cpu` (it may run guest code). For per-frame
 * work of the platform layer; GCD's main queue is drained by the turn itself, after the
 * timers. */
typedef void (*mt_runloop_source_fn)(mt_cpu *cpu, void *user);

/* Registers a per-turn source (sources run in registration order); returns 0, or -1 if
 * (fn, user) is already registered. Kept across mt_cf_start. */
int mt_runloop_add_turn_source(mt_runloop_source_fn fn, void *user) MT_NOEXCEPT;
/* Unregisters it; returns 0, or -1 if it was not registered. */
int mt_runloop_remove_turn_source(mt_runloop_source_fn fn, void *user) MT_NOEXCEPT;

/* One turn of the main run loop at absolute time `now` (CFAbsoluteTime, normally
 * mt_cf_absolute_time()): pool push, per-turn sources, every timer of the default or common
 * modes whose fire date is <= now (once each, earliest first), GCD's main queue
 * (mt_gcd_drain_main: due dispatch_after blocks, worker threads, the main-queue blocks queued
 * before it started), pool pop. A repeating timer's next fire date stays on its interval
 * grid (missed fires are skipped) unless the callout set a new one; a one-shot timer is
 * invalidated after it fires. Returns mt_runloop_next_wake(now). A call from inside a turn
 * (a nested run) only logs. */
double mt_runloop_run_once(mt_cpu *cpu, double now) MT_NOEXCEPT;

/* When the loop next has work, in CFAbsoluteTime: the earlier of the next timer fire date and
 * GCD's next deadline (mt_gcd_next_deadline, converted from GCD's monotonic clock by the
 * distance between mt_gcd_now() and `now`); `now` if GCD work is waiting;
 * MT_CF_DISTANT_FUTURE if nothing is scheduled. The platform may sleep until then. */
double mt_runloop_next_wake(double now) MT_NOEXCEPT;

/* The earliest fire date of a scheduled timer, MT_CF_DISTANT_FUTURE if none (timers only). */
double mt_runloop_next_fire_date(void) MT_NOEXCEPT;

/* The main run loop's guest object (CFRunLoopGetMain), created on first use; 0 without a
 * runtime. */
uint32_t mt_runloop_main(mt_cpu *cpu) MT_NOEXCEPT;

/* The number of timers scheduled on the main run loop. */
uint32_t mt_runloop_timer_count(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_CF_H */
