/* Save states (#202, ADR 0105, docs/re/save-states.md): an in-process snapshot of the running
 * game at a safe point, as a state file, and its restore inside the same process.
 *
 * A state holds what the next ticks depend on: guest memory (the writable image, the heap up to
 * its committed end, the stacks), the heap's host bookkeeping, the main cpu's registers, the ObjC
 * runtime's mutable tables (retain counts, @synchronized depths, autorelease pools, +initialize
 * flags, the selector table) and every live host state through a per-type serializer
 * (runtime/src/snapshot/snapshot_internal.h: mt_snap::StateOps). Not saved: what is the same in
 * every process of one binary (class and method tables, checked by the binary's hash), and the
 * player's settings, window and audio device.
 *
 *   uint8_t *bytes; size_t n; char why[160];
 *   mt_snapshot_meta meta = mt_snapshot_meta_default();
 *   mt_process_image_sha256(p, meta.binary_sha256);
 *   meta.slot = slot;
 *   if (mt_snapshot_save(cpu, &meta, &bytes, &n, why, sizeof why) == 0) { write; free(bytes); }
 *   ...
 *   mt_snapshot_expect ex = mt_snapshot_expect_default();  // the binary's hash, the slot
 *   ex.rebase_clock = rebase;                               // the guest time goes on from it
 *   if (mt_snapshot_restore(cpu, bytes, n, &ex, &header, why, sizeof why) != 0) toast(why);
 *
 * Both are only valid in the platform's turn hook (between two run-loop turns, maplethief/uikit.h)
 * and only at a safe point (mt_snapshot_safe); anywhere else they refuse with the reason. A
 * restore is all or nothing: the whole file is read and checked, every host state loaded, before
 * anything live changes; a refused state changes nothing.
 *
 * The file is untrusted input: every length is checked against the file and a size limit before
 * it is read or inflated, every section's CRC-32 before it is used. C17 that also compiles as
 * C++20; the functions are in runtime/src/snapshot/. */
#ifndef MAPLETHIEF_SNAPSHOT_H
#define MAPLETHIEF_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/objc/objc.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* The state file's format version; a reader refuses any other. */
#define MT_SNAPSHOT_FORMAT 1u
/* No character slot (a state taken outside a game, or a restore that accepts any). */
#define MT_SNAPSHOT_ANY_SLOT 0xFFFFFFFFu

/* --- the safe point ------------------------------------------------------------------------- */

/* Where the safe-point check reads the game. The defaults are the game's
 * (docs/re/save-states.md); a synthetic test points them at its own words or turns them off. */
typedef struct mt_snapshot_config {
    uint32_t game_mode; /* guest int32 that must be 0 (the field): 0x11AA00; 0: not checked */
    uint32_t popup;     /* guest int32 that must be -1 (no popup): 0x108C64; 0: not checked */
    int check_panel;    /* 1: the key pad's panel (mt_input_panel) must be 0 */
} mt_snapshot_config;

mt_snapshot_config mt_snapshot_config_default(void) MT_NOEXCEPT;
/* NULL: the default. */
void mt_snapshot_configure(const mt_snapshot_config *config) MT_NOEXCEPT;

/* 1 if a state may be taken or restored now; else 0 with the reason ("not in the field", "a
 * popup is open", "a touch is down", "GCD is busy", ...) in `why` (may be NULL). The turn hook is
 * the caller's to ensure; this refuses inside a run-loop turn. */
int mt_snapshot_safe(mt_cpu *cpu, char *why, size_t why_size) MT_NOEXCEPT;

/* --- saving ----------------------------------------------------------------------------------- */

typedef struct mt_snapshot_meta {
    uint8_t binary_sha256[32]; /* the armv7 slice's SHA-256 (mt_process_image_sha256) */
    uint32_t slot;             /* the character slot, MT_SNAPSHOT_ANY_SLOT for none */
    uint32_t flags;            /* MT_SNAPSHOT_* below */
} mt_snapshot_meta;

/* Sections stored, not deflated (faster, larger). */
#define MT_SNAPSHOT_STORE 1u
/* Save even though some live host state types have no serializer: their states are left out.
 * Only for measurements and the census test; such a file is refused by a restore. */
#define MT_SNAPSHOT_PARTIAL 2u

mt_snapshot_meta mt_snapshot_meta_default(void) MT_NOEXCEPT;

/* The state file of the running game: *out (malloc'd, the caller frees it with free) and *size.
 * Returns 0, or -1 with the reason in `why` (not at a safe point, a live host state type without
 * a serializer, out of memory). Changes nothing in the game. */
int mt_snapshot_save(mt_cpu *cpu, const mt_snapshot_meta *meta, uint8_t **out, size_t *size,
                     char *why, size_t why_size) MT_NOEXCEPT;

/* --- reading -------------------------------------------------------------------------------- */

typedef struct mt_snapshot_header {
    uint32_t format;
    uint8_t binary_sha256[32];
    char port_version[32]; /* NUL-terminated: MT_VERSION of the port that wrote it */
    uint32_t slot;
    uint32_t flags;         /* the meta's flags it was saved with */
    uint64_t guest_unix_us; /* the guest's clock (Unix microseconds) at the save */
    uint32_t sections;
} mt_snapshot_header;

typedef struct mt_snapshot_expect {
    const uint8_t *binary_sha256; /* NULL: any binary (tests) */
    uint32_t slot;                /* MT_SNAPSHOT_ANY_SLOT: any */
    /* Called after a restore with the saved guest time: the platform re-bases the clock behind
     * the guest (the virtual clock, or the launch clock in real time) so that the guest's time
     * goes on from it. NULL: nothing. */
    void (*rebase_clock)(void *user, uint64_t guest_unix_us);
    void *user;
} mt_snapshot_expect;

mt_snapshot_expect mt_snapshot_expect_default(void) MT_NOEXCEPT;

/* Checks the file (header, every section's length and CRC) without restoring it. Returns 0 with
 * *header, or -1 with the reason (a wrong magic, format version, binary hash or slot, a bad CRC,
 * a truncated or oversized section). `expect` may be NULL. */
int mt_snapshot_read_header(const uint8_t *bytes, size_t size, const mt_snapshot_expect *expect,
                            mt_snapshot_header *header, char *why, size_t why_size) MT_NOEXCEPT;

/* Restores the state file: at a safe point, the file checked and loaded first (all or nothing).
 * Returns 0 (with *header, which may be NULL), or -1 with the reason, having changed nothing. */
int mt_snapshot_restore(mt_cpu *cpu, const uint8_t *bytes, size_t size,
                        const mt_snapshot_expect *expect, mt_snapshot_header *header, char *why,
                        size_t why_size) MT_NOEXCEPT;

/* --- the census --------------------------------------------------------------------------- */

/* The live host state types of `rt` without a serializer, by name (distinct, in first-seen
 * order): up to `max` names into `names` (static strings). Returns how many there are. */
uint32_t mt_snapshot_missing(const mt_objc *rt, const char **names, uint32_t max) MT_NOEXCEPT;
/* 1 if the state type named `name` has a serializer. */
int mt_snapshot_has_serializer(const char *name) MT_NOEXCEPT;

/* --- helpers -------------------------------------------------------------------------------- */

/* SHA-256 of `n` bytes (the binary's hash in the header). */
void mt_sha256(const uint8_t *data, size_t n, uint8_t out[32]) MT_NOEXCEPT;
/* CRC-32 (IEEE 802.3) of `n` bytes, continuing from `crc` (0 to start). */
uint32_t mt_crc32(uint32_t crc, const uint8_t *data, size_t n) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_SNAPSHOT_H */
