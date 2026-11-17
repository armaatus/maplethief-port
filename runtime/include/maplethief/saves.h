/* The save files on the host (#78, ADR 0074): where the game's Documents/ directory lives, how
 * a file there is replaced so that a crash or a kill never leaves half a file, and the checks a
 * save file's bytes get when the game reads them. Foundation's NSFileManager and NSFileHandle
 * (runtime/src/foundation/files.cpp) and GameKit's gamecenter.json go through here; so should
 * every other writer of the player's data (export, import and backups: #148).
 *
 * The game's own files are docs/formats/save.md: theif0-2, quest0-2, config, cupon, CTerms and
 * the receipt lists, raw bytes with no version or checksum, written at offset 0 without
 * truncating. The port keeps those bytes exactly; only the way they reach the disk changes.
 *
 * Plain C++ like the stubs that call it (no exceptions, no standard containers, no
 * function-local statics), so the recompiler's C test harness links it too. C17 that also
 * compiles as C++20. Not thread-safe: the guest's main thread calls it (ADR 0014). */
#ifndef MAPLETHIEF_SAVES_H
#define MAPLETHIEF_SAVES_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- where the saves are ------------------------------------------------------------------ */

/* The host directory the guest's <home>/Documents maps to: mt_saves_set_documents_dir, else
 * <mt_stubs_data_dir()>/Documents (the runtime's own default, for builds without the frontend).
 * The frontend always sets it (platform/src/data_location.h, ADR 0127):
 *
 *   desktop  <data folder>/saves, the data folder being <game folder>/data by default, or the
 *            per-user folder, or MAPLETHIEF_DATA_DIR; the save_dir setting moves it
 *   iOS      the app's own Documents/ (shared with the Files app, ADR 0074)
 *
 * Never NULL; "" if there is no data directory. The string stays valid until the next call. */
const char *mt_saves_documents_dir(void) MT_NOEXCEPT;

/* Sets the directory Documents/ maps to (copied); NULL or "" goes back to the default above.
 * Then finishes, in that directory, every replacement a stopped process left half done
 * (mt_saves_recover_dir): the frontend sets it at start, before the game reads a save.
 * Returns 0, or -1 if the path is too long. */
int mt_saves_set_documents_dir(const char *dir) MT_NOEXCEPT;

/* --- writing ------------------------------------------------------------------------------ */

/* Replaces the file at `path` (UTF-8) with `size` bytes, atomically: the bytes go to the
 * temporary file "<path>.mt-tmp" in the same directory, which is flushed to the disk
 * (F_FULLFSYNC on Apple, fsync elsewhere, _commit on Windows) and then renamed over `path`
 * (rename(2); MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows).
 *
 * On the Nintendo Switch, where the SD card's rename refuses an existing target, it takes two
 * steps (ADR 0074's Horizon addendum): the flushed temporary file is renamed to
 * "<path>.mt-ready", the old `path` is removed, the ready file is renamed to `path`, and the
 * device is committed (fsdevCommitDevice). A ready file is always whole, so a process stopped
 * between the steps leaves one that mt_saves_recover_file puts in place: this call does so
 * first, and so does mt_saves_set_documents_dir at start.
 *
 * Whenever the process stops, `path` holds (after the recovery) either all of its old bytes or
 * all of the new ones. Creates the parent directories. Returns 0, or -1 with `path` unchanged
 * and the temporary file removed. A "<path>.mt-tmp" left by a killed process is harmless
 * (never read) and is overwritten by the next replacement of `path`. */
int mt_saves_replace_file(const char *path, const void *bytes, size_t size) MT_NOEXCEPT;

/* The suffix of the temporary files: tools that list or copy the saves skip these. */
#define MT_SAVES_TMP_SUFFIX ".mt-tmp"

/* The suffix of a whole new file waiting to replace "<path>" (the two-step replacement). */
#define MT_SAVES_READY_SUFFIX ".mt-ready"

/* Puts "<path>.mt-ready", if there is one, in place of `path`. Returns 1 if it did, 0 if there
 * was none, -1 if it could not (both stay). */
int mt_saves_recover_file(const char *path) MT_NOEXCEPT;

/* mt_saves_recover_file for every "<name>.mt-ready" in directory `dir` (not its
 * subdirectories); returns how many were put in place. */
int mt_saves_recover_dir(const char *dir) MT_NOEXCEPT;

/* --- reading ------------------------------------------------------------------------------- */

/* Makes the bytes of the save file `name` (a file name in Documents/, e.g. "config") safe for
 * the game to read, in place, before the game sees them. Only values the game cannot handle
 * change; a file the game wrote itself comes through as it is. Returns the number of bytes
 * changed (0 for every other file).
 *
 *   config  byte 1 (game speed) and byte 5 above 2 become 2: the game indexes 3-entry tables
 *           with them unchecked (ApplyGameSpeed 0x37C41: table 0x108C58, the tick length;
 *           0x37B85 and CStateGame::LoadGame 0x86C1D: table 0x108C4C), so 3 gave a tick
 *           length of -1 and larger values read whatever follows the table. */
uint32_t mt_saves_sanitise(const char *name, uint8_t *bytes, size_t size) MT_NOEXCEPT;

/* --- checking (#148, ADR 0078) -------------------------------------------------------------- */

/* Whether `name` is the name of a save file in Documents/: one the game keeps (theif0-2,
 * quest0-2, config, cupon, CTerms, crestore, riprestore1, riprestore2 or totalrestore; case
 * matters, the misspelling too) or the port's maplethief-cashshop0-2 (the cash shop's counters
 * per slot, #156, ADR 0072). Export, import and backups copy only these. */
int mt_saves_is_game_file(const char *name) MT_NOEXCEPT;

/* The port's counters file of a slot: MT_SAVES_COUNTERS_PREFIX "<slot>" (maplethief/cashshop.h
 * writes it). */
#define MT_SAVES_COUNTERS_PREFIX "maplethief-cashshop"

/* What mt_saves_check found: the first rule a file breaks (the checks go through the file in
 * byte order). The same codes and order as tools/mtport/savecheck.py (CTest
 * saves.check_parity runs both on the same fixtures). */
typedef enum mt_saves_verdict {
    MT_SAVES_OK = 0,
    MT_SAVES_BAD_NAME = 1,      /* not a file the game uses */
    MT_SAVES_BAD_SIZE = 2,      /* truncated or oversized */
    MT_SAVES_BAD_JOB = 3,       /* theif: branch above 2 or job tier above 4 */
    MT_SAVES_BAD_LEVEL = 4,     /* theif: level outside 1-100 (hero.dat's rows) */
    MT_SAVES_BAD_ITEM = 5,      /* an item id past item_equip's or item_etc's rows */
    MT_SAVES_BAD_SKILL = 6,     /* theif: a skill level above the highest max_level */
    MT_SAVES_BAD_MAP = 7,       /* theif: an area or map with no shipped .map file */
    MT_SAVES_BAD_QUEST = 8,     /* quest: a key that is not text, a requirement or reward kind */
    MT_SAVES_BAD_CONFIG = 9,    /* config: a value past the game's tables */
    MT_SAVES_BAD_RECEIPTS = 10, /* a receipt list whose entries run past its end */
    MT_SAVES_BAD_COUNTERS = 11, /* maplethief-cashshop<n>: not "key = number" text */
    /* Not a verdict: widens the enum to every int, so that mt_saves_verdict_name may read any
     * value a C caller passes (UBSan's invalid-enum-load in C++, #321). */
    MT_SAVES_VERDICT_RANGE_ = 0x7FFFFFFF
} mt_saves_verdict;

/* The limits the checks use: facts of the shipped 1.5 data (docs/formats/data-tables.md,
 * map.md; tests/test_savecheck_real.py checks them against the bundle). */
#define MT_SAVES_EQUIP_IDS 74u       /* item_equip.dat rows: ids 0-73 (0xFF = empty) */
#define MT_SAVES_ETC_IDS 138u        /* item_etc.dat rows: ids 0-137 */
#define MT_SAVES_MAX_LEVEL 100u      /* hero.dat rows: levels 1-100 */
#define MT_SAVES_MAX_BRANCH 2u       /* skill.dat branch: 0 Rogue, 1 Assassin, 2 Bandit */
#define MT_SAVES_MAX_JOB_TIER 4u     /* skill.dat tier */
#define MT_SAVES_MAX_SKILL_LEVEL 20u /* the highest skill.dat max_level */
#define MT_SAVES_AREAS 11u           /* map/0.mapprj ... map/10.mapprj */
#define MT_SAVES_MAX_RECEIPTS_BYTES (1024u * 1024u) /* a receipt list: a few KB per entry */
#define MT_SAVES_MAX_COUNTERS_BYTES 1024u /* the cash shop writes them from a 1 KB buffer */

/* The number of maps area `area` has (map/<area><map>.map for map 0 .. n-1); 0 past the last. */
uint32_t mt_saves_maps_in_area(uint32_t area) MT_NOEXCEPT;

/* Checks the bytes of the save file `name` (a file name in Documents/) against everything the
 * game trusts when it loads them (docs/formats/save.md, "Checking a save"): the size, the job,
 * the level, every item id, the skill levels, the map, the quest records' text keys and kinds,
 * and config's table indexes. A file the game wrote itself always passes. It never changes the
 * bytes: a file that breaks a rule is refused, not fixed. Returns MT_SAVES_OK, or the first
 * rule broken with its reason in plain words in why[0..why_size) (NUL-terminated, cut to
 * fit; `why` may be NULL), e.g. "theif0 has 1000 bytes; a character file has exactly 1124". */
mt_saves_verdict mt_saves_check(const char *name, const uint8_t *bytes, size_t size, char *why,
                                size_t why_size) MT_NOEXCEPT;

/* The verdict's name: "ok", "bad_name", "bad_size", "bad_job", "bad_level", "bad_item",
 * "bad_skill", "bad_map", "bad_quest", "bad_config", "bad_receipts", "bad_counters" ("?" for
 * anything else). */
const char *mt_saves_verdict_name(mt_saves_verdict verdict) MT_NOEXCEPT;

/* --- refusing a save the game would read (#413, ADR 0154 (a)) ------------------------------ */

/* A theif<N> or quest<N> that the guest finds directly in Documents/ (by fileExistsAtPath: or
 * an NSFileHandle) is checked with mt_saves_check first: a file copied in by hand, by a sync or
 * through the iOS Files app reaches the game without the import's checks. One that fails is
 * moved to MT_SAVES_REFUSED_DIR/<time>/ under the data directory (mt_stubs_data_dir), with
 * MT_SAVES_REFUSED_REASON beside it ("<name>: <verdict>: <why>"), logged, and the guest sees no
 * file (moved, not hidden: the game writes a new save over a missing slot). The game's own
 * writes always pass. foundation/files.cpp does this; the hook below hears of each refusal. */
#define MT_SAVES_REFUSED_DIR "backups/refused"
#define MT_SAVES_REFUSED_REASON "reason.txt"

/* Called once per refused file, on the guest's thread: the file's name, why (mt_saves_check's
 * words) and the folder it was moved to ("" if it could not be moved: the guest still sees no
 * file this time). */
typedef void (*mt_saves_refused_fn)(const char *name, const char *why, const char *moved_to,
                                    void *user);

/* Sets the hook (NULL: none, the default). The frontend shows each refusal once. */
void mt_saves_set_refused_hook(mt_saves_refused_fn fn, void *user) MT_NOEXCEPT;

/* --- tests ---------------------------------------------------------------------------------- */

/* Where mt_saves_replace_file stops when a fault is set, as a process killed at that moment
 * would: nothing more is written, nothing is cleaned up, and it returns -1. */
typedef enum mt_saves_fault {
    MT_SAVES_FAULT_NONE = 0,
    MT_SAVES_FAULT_OPENED = 1,       /* the temporary file was created, nothing written */
    MT_SAVES_FAULT_HALF_WRITTEN = 2, /* half the bytes are in the temporary file */
    MT_SAVES_FAULT_WRITTEN = 3,      /* all of them, flushed, but not yet renamed */
    MT_SAVES_FAULT_READY = 4,  /* two steps: renamed to the ready name, old file still there */
    MT_SAVES_FAULT_REMOVED = 5 /* two steps: the old file removed, the ready one not renamed */
} mt_saves_fault;

/* Sets the fault every later mt_saves_replace_file meets until it is set back to NONE. A test
 * seam: nothing else sets it. */
void mt_saves_set_fault(mt_saves_fault fault) MT_NOEXCEPT;

/* How mt_saves_replace_file puts the new file in place: AUTO (the platform's way: two steps
 * on the Nintendo Switch, one rename elsewhere), or one of the two forced, so that the desktop
 * tests run the Switch's way too. A test seam: nothing else sets it. */
typedef enum mt_saves_replace_mode {
    MT_SAVES_REPLACE_AUTO = 0,
    MT_SAVES_REPLACE_RENAME = 1,
    MT_SAVES_REPLACE_TWO_STEP = 2
} mt_saves_replace_mode;
void mt_saves_set_replace_mode(mt_saves_replace_mode mode) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_SAVES_H */
