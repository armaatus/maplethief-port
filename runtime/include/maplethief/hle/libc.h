/* The libc subset of the HLE layer (ADR 0028, #44): what the platform layer configures.
 *
 * The guest's libSystem imports (malloc, mem and str functions, the printf family, floor,
 * pow, trunc, gettimeofday, sysctl, the socket stubs, abort and exit) are HLE functions in
 * runtime/src/hle/libc*.cpp, listed in the registry by their import names. They need a few
 * things from the host, set here once at start-up, before the guest runs:
 *
 *     mt_libc_config cfg = mt_libc_config_default();
 *     mt_libc_config_from_env(&cfg);   // MAPLETHIEF_HW_MACHINE
 *     cfg.write = my_console;          // optional: where guest stdout/stderr go
 *     mt_libc_configure(&cfg);
 *
 * Without a call to mt_libc_configure the defaults apply.
 *
 * Guest exit() and abort() never end the host process from inside an HLE function: they stop
 * the guest through mt_trap with MT_TRAP_EXIT (detail = status) or MT_TRAP_ABORT, and the
 * runtime's mt_trap decides what happens (run the guest's atexit handlers and exit, or show
 * a crash report).
 *
 * C17 that also compiles as C++20; the implementation is C++. */
#ifndef MAPLETHIEF_HLE_LIBC_H
#define MAPLETHIEF_HLE_LIBC_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/hle/imports.h" /* mt_log_fn */
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* Receives guest output: fd 1 (stdout: printf, puts) or 2 (stderr: vfprintf(stderr, ...),
 * __assert_rtn's message). `data` is not NUL-terminated. */
typedef void (*mt_libc_write_fn)(void *user, int fd, const char *data, size_t len);

/* The wall clock in microseconds since the Unix epoch (gettimeofday). */
typedef uint64_t (*mt_libc_clock_fn)(void *user);

/* The environment variable mt_libc_config_from_env reads: the hw.machine string. */
#define MT_LIBC_HW_MACHINE_ENV "MAPLETHIEF_HW_MACHINE"

/* The device the guest sees by default: an iPhone 4 (the game's armv7 slice targets iOS 4). */
#define MT_LIBC_DEFAULT_HW_MACHINE "iPhone3,1"

typedef struct mt_libc_config {
    mt_libc_write_fn write; /* NULL: the host's stdout and stderr */
    void *write_user;
    mt_libc_clock_fn clock; /* NULL: the host's wall clock */
    void *clock_user;
    /* The runtime's own diagnostics (a bad free, an unknown sysctl), one line each: */
    mt_log_fn log; /* NULL: stderr */
    void *log_user;
    /* sysctl hw.machine, e.g. "iPhone3,1" or "iPod4,1"; NUL-terminated. hw.model follows it
     * (N90AP, N81AP, ...; "Unknown" for a machine the table does not know). */
    char hw_machine[32];
    /* The MAC address of en0 that sysctl(NET_RT_IFLIST) reports. The game's device id is the
     * MD5 of it (docs/re/network.md); default 02:00:00:00:00:00, locally administered. */
    uint8_t en0_mac[6];
} mt_libc_config;

/* Host stdout/stderr and clock, MT_LIBC_DEFAULT_HW_MACHINE, MAC 02:00:00:00:00:00. */
mt_libc_config mt_libc_config_default(void) MT_NOEXCEPT;

/* Applies MAPLETHIEF_HW_MACHINE if it is set. Returns 0, or -1 (and leaves cfg unchanged) if
 * the value is empty or longer than 31 bytes. */
int mt_libc_config_from_env(mt_libc_config *cfg) MT_NOEXCEPT;

/* Makes `cfg` the configuration of the libc HLE functions (NULL: the defaults). Not
 * thread-safe; call at start-up or between tests. */
void mt_libc_configure(const mt_libc_config *cfg) MT_NOEXCEPT;

/* The hw.model of a hw.machine string ("iPhone3,1" -> "N90AP"), or "Unknown". */
const char *mt_libc_hw_model(const char *machine) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_HLE_LIBC_H */
