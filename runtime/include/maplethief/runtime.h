/* MapleThief HLE runtime: the C interface (ADR 0002). Generated code and the platform layer
 * call the runtime only through extern "C" functions declared in headers like this one.
 * Placeholder until M4. */
#ifndef MAPLETHIEF_RUNTIME_H
#define MAPLETHIEF_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#define MT_NOEXCEPT noexcept
#else
#define MT_NOEXCEPT
#endif

/* Returns a static, NUL-terminated name and version of the runtime, e.g. for logs. */
const char *mt_runtime_version(void) MT_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* MAPLETHIEF_RUNTIME_H */
