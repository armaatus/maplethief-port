/* The runtime side of the import tables (ADR 0019): HLE registry, the default handler for
 * unimplemented imports, data objects and binds.
 *
 * Start-up, after the image is mapped (M4, #43), with the generated tables of
 * maplethief/recomp/imports.h:
 *
 *     mt_import_config cfg = mt_import_config_default();
 *     mt_import_config_from_env(&cfg);                       // MAPLETHIEF_UNIMPLEMENTED_IMPORT
 *     uint32_t n_hle;
 *     const mt_hle_entry *hle = mt_hle_registry(&n_hle);
 *     mt_imports_link(mt_import_descs, mt_import_count, mt_import_table, hle, n_hle, &cfg);
 *     mt_imports_place(cpu, mt_import_descs, mt_import_count, hle, n_hle, alloc, heap, addrs);
 *     mt_imports_apply_binds(cpu, mt_import_binds, mt_import_bind_count, addrs, mt_import_count);
 *
 * mt_imports_link gives every slot of the table a host function: the HLE implementation
 * registered under the import's name, or mt_import_unimplemented. mt_imports_place gives
 * every import a guest address (a function's from its descriptor; a data import's from a
 * zeroed object the runtime's allocator provides, then filled by its registered init
 * hook), and mt_imports_apply_binds writes those addresses into the guest's pointers.
 *
 * The implementation is C++ (runtime/src/hle/imports.cpp); this header is C17 too. */
#ifndef MAPLETHIEF_HLE_IMPORTS_H
#define MAPLETHIEF_HLE_IMPORTS_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- configuration -------------------------------------------------------------------- */

/* What the default handler does when the guest calls an import without an implementation.
 * It always logs the import's name and the guest caller first. */
enum mt_unimplemented_policy {
    MT_UNIMPLEMENTED_STOP = 0,     /* stop the guest: mt_trap(MT_TRAP_IMPORT, lr, id) */
    MT_UNIMPLEMENTED_CONTINUE = 1, /* return 0 in r0 and r1; log only the first call of each */
    MT_UNIMPLEMENTED_TRACE = 2,    /* like CONTINUE, but log every call */
};

/* The environment variable mt_import_config_from_env reads: "stop" (the default),
 * "continue" or "trace". */
#define MT_UNIMPLEMENTED_IMPORT_ENV "MAPLETHIEF_UNIMPLEMENTED_IMPORT"

/* Receives one log line (no trailing newline). */
typedef void (*mt_log_fn)(void *user, const char *line);

typedef struct mt_import_config {
    uint32_t unimplemented; /* enum mt_unimplemented_policy */
    mt_log_fn log;          /* NULL: write the line to stderr */
    void *log_user;
} mt_import_config;

/* Stop on an unimplemented import, log to stderr. */
mt_import_config mt_import_config_default(void) MT_NOEXCEPT;

/* Parses a policy name ("stop", "continue", "trace"); returns 0, or -1 if `text` is none. */
int mt_unimplemented_policy_parse(const char *text, uint32_t *policy) MT_NOEXCEPT;

/* Applies MAPLETHIEF_UNIMPLEMENTED_IMPORT if it is set. Returns 0, or -1 (and logs, and
 * leaves cfg unchanged) if its value is not a policy name. */
int mt_import_config_from_env(mt_import_config *cfg) MT_NOEXCEPT;

/* --- the HLE registry ----------------------------------------------------------------- */

/* Fills the zeroed guest object of data import `id` at guest address `addr` (e.g. stores a
 * CFBooleanRef into _kCFBooleanTrue). Runs after every import has its address. */
typedef void (*mt_data_init_fn)(mt_cpu *cpu, uint32_t id, uint32_t addr);

/* One HLE implementation, matched to imports by symbol name. A function import uses `fn`,
 * a data import `init` (either may be NULL). */
typedef struct mt_hle_entry {
    const char *name;
    mt_import_fn fn;
    mt_data_init_fn init;
} mt_hle_entry;

/* Every implementation the runtime has (runtime/src/hle/registry.cpp). */
const mt_hle_entry *mt_hle_registry(uint32_t *count) MT_NOEXCEPT;

/* --- linking, placing, binding -------------------------------------------------------- */

/* Fills table[0..count) from `hle` by name. Function imports without an implementation, and
 * the (never called) slots of data imports, get mt_import_unimplemented, which uses `descs`
 * and a copy of `cfg` from now on (NULL: mt_import_config_default()). Returns the number of
 * function imports left to the default handler. Not thread-safe; call once at start-up. */
uint32_t mt_imports_link(const mt_import_desc *descs, uint32_t count, mt_import_fn *table,
                         const mt_hle_entry *hle, uint32_t hle_count,
                         const mt_import_config *cfg) MT_NOEXCEPT;

/* The default handler: logs "unimplemented import <name> (id <id>, <library>) called from
 * 0x<lr>", then stops or returns per the configured policy. The caller is the return
 * address in lr (for a guest tail call, the return address of the tail-calling function). */
void mt_import_unimplemented(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;

/* How often the default handler has run for import `id` since mt_imports_link. */
uint32_t mt_import_unimplemented_calls(uint32_t id) MT_NOEXCEPT;

/* Allocates `size` bytes of guest memory aligned to `align` for a load-time object; returns
 * its guest address, or 0 if there is no room. Provided by the memory model (#42). */
typedef uint32_t (*mt_guest_alloc_fn)(void *user, uint32_t size, uint32_t align);

/* Sets addrs[id] for every import: a function's descriptor address; for a data import, a
 * new zeroed object of its size and alignment from `alloc`. Then runs the init hook of every
 * data import that `hle` has one for, in id order. Returns 0, or -1 if an allocation fails
 * or lies outside guest memory. */
int mt_imports_place(mt_cpu *cpu, const mt_import_desc *descs, uint32_t count,
                     const mt_hle_entry *hle, uint32_t hle_count, mt_guest_alloc_fn alloc,
                     void *alloc_user, uint32_t *addrs) MT_NOEXCEPT;

/* Writes addrs[id] + addend to each bind's slot. Returns 0, or -1 if a slot is outside guest
 * memory or an id is not below `count` (the other binds are still written). */
int mt_imports_apply_binds(mt_cpu *cpu, const mt_import_bind *binds, uint32_t bind_count,
                           const uint32_t *addrs, uint32_t count) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_HLE_IMPORTS_H */
