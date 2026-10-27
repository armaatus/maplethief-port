// Shared by the libc HLE files (libc.cpp, libc_sys.cpp): the active configuration, the
// runtime's diagnostics and the guest's output streams (ADR 0028).
#ifndef MAPLETHIEF_RUNTIME_HLE_LIBC_INTERNAL_H
#define MAPLETHIEF_RUNTIME_HLE_LIBC_INTERNAL_H

#include <cstddef>
#include <cstdint>

#include "maplethief/hle/libc.h"

#if defined(__GNUC__) || defined(__clang__)
#define MT_LIBC_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define MT_LIBC_PRINTF(f, a)
#endif

// The configuration of mt_libc_configure (or the defaults).
const mt_libc_config &mt_libc_cfg() noexcept;

// A runtime diagnostic (not guest output): one line, "libc: " prefixed, to the configured log sink
// or stderr.
void mt_libc_log(const char *fmt, ...) noexcept MT_LIBC_PRINTF(1, 2);

// Guest output on fd 1 or 2 through the configured sink.
void mt_libc_write(int fd, const char *data, std::size_t len) noexcept;

#endif // MAPLETHIEF_RUNTIME_HLE_LIBC_INTERNAL_H
