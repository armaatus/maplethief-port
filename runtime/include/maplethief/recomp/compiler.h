/* Compiler portability macros for recompiled code (ADR 0014).
 *
 * Generated C (work/gen/) is C17 and must compile with clang (shipping builds: Apple clang,
 * clang-cl, clang) and with MSVC cl /std:c17 (CI portability check), ADR 0002. Every
 * compiler-specific feature goes through a macro here, with a portable fallback. The runtime
 * (C++20) includes these headers too, so they also compile as C++. */
#ifndef MAPLETHIEF_RECOMP_COMPILER_H
#define MAPLETHIEF_RECOMP_COMPILER_H

#include <float.h>
#include <stdint.h>

#ifdef __cplusplus
#define MT_EXTERN_C_BEGIN extern "C" {
#define MT_EXTERN_C_END }
#define MT_NOEXCEPT noexcept
#define MT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define MT_EXTERN_C_BEGIN
#define MT_EXTERN_C_END
#define MT_NOEXCEPT
#define MT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* `restrict` on the CPU-state parameter: the generated code only reaches the mt_cpu object
 * through that pointer (the runtime must never touch it through another pointer while a
 * generated function runs), so the host compiler may keep guest registers in host registers
 * between calls and guest memory accesses. */
#if defined(__cplusplus)
#define MT_RESTRICT __restrict
#else
#define MT_RESTRICT restrict
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MT_INLINE static inline __attribute__((always_inline))
#define MT_NORETURN __attribute__((noreturn))
#define MT_LIKELY(x) __builtin_expect(!!(x), 1)
#define MT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
#define MT_INLINE static __forceinline
#define MT_NORETURN __declspec(noreturn)
#define MT_LIKELY(x) (x)
#define MT_UNLIKELY(x) (x)
#else
#define MT_INLINE static inline
#define MT_NORETURN
#define MT_LIKELY(x) (x)
#define MT_UNLIKELY(x) (x)
#endif

#if defined(__has_attribute)
#define MT_HAS_ATTRIBUTE(x) __has_attribute(x)
#else
#define MT_HAS_ATTRIBUTE(x) 0
#endif

/* MT_TAILCALL(f(cpu)): a guest tail call (`b other_function`, `bx rN` to another function).
 * With clang it is a guaranteed tail call (musttail), so chains of guest tail calls do not
 * grow the host stack. Elsewhere it is a plain call followed by return: correct, but each
 * tail call costs one host frame until the chain returns. `return f(cpu);` in a void
 * function is valid C only as an extension, hence the -Wpedantic suppression. */
#if defined(__clang__) && !defined(__cplusplus) && MT_HAS_ATTRIBUTE(musttail)
#define MT_TAILCALL(call)                                                                          \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wpedantic\"")            \
        __attribute__((musttail)) return call;                                                     \
    _Pragma("clang diagnostic pop")
#else
#define MT_TAILCALL(call)                                                                          \
    do {                                                                                           \
        call;                                                                                      \
        return;                                                                                    \
    } while (0)
#endif

/* The generated code's arithmetic assumes what every supported compiler does: two's
 * complement integers, arithmetic right shift of negative values, 32-bit int, a little-endian
 * host (guest memory is little-endian and is accessed with memcpy), and IEEE binary32/binary64
 * evaluated in their own precision (no x87). */
MT_STATIC_ASSERT((-1 >> 1) == -1, "arithmetic right shift of negative values required");
MT_STATIC_ASSERT(sizeof(int) == 4, "32-bit int required");
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
MT_STATIC_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian host required");
#endif
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != -1
#error "recompiled code needs FLT_EVAL_METHOD 0 (SSE2/NEON floating point, no x87)"
#endif

#endif /* MAPLETHIEF_RECOMP_COMPILER_H */
