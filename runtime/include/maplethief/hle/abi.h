/* Argument and result marshalling for HLE functions: the iOS armv7 calling convention
 * (ADR 0019).
 *
 * An HLE function is an mt_import_fn, `void f(mt_cpu *cpu, uint32_t id)`, entered with the
 * guest registers exactly as the guest's call left them: arguments in r0-r3 and then on the
 * guest stack at sp, the return address in lr. It reads its arguments through an mt_args
 * cursor and writes its result with mt_ret_*:
 *
 *     static void hle_CFRunLoopTimerSetNextFireDate(mt_cpu *cpu, uint32_t id) {
 *         mt_args a = mt_args_begin(cpu);
 *         uint32_t timer = mt_arg_u32(&a);
 *         double date = mt_arg_f64(&a);
 *         ...
 *     }
 *
 * The iOS armv7 convention (Apple's "iOS ABI Function Call Guide"; clang's APCS ABI for
 * Darwin armv7), checked against the game's own call sites:
 *
 *   - Arguments are a sequence of 32-bit words: r0, r1, r2, r3, then [sp], [sp+4], ...
 *   - Soft-float: float and double arguments and results are in core registers, never in
 *     s/d registers (VFP is only used inside functions).
 *   - 64-bit values (long long, double) are 4-byte aligned, so they take the next two words
 *     whatever their position, and may be split between r3 and [sp]. The game's call of
 *     CFRunLoopTimerCreate at 0x8ED8 passes fireDate in r1:r2 and interval in r3:[sp].
 *     (Plain AAPCS instead starts a 64-bit value at an even register and an 8-byte aligned
 *     stack slot, never splitting it; mt_args_begin_aapcs gives that rule.)
 *   - A structure argument is passed by value as its words, like that many word arguments.
 *   - Results: 32-bit values and floats in r0; 64-bit values and doubles in r0 (low word)
 *     and r1 (high word). A structure of up to 4 bytes is returned in r0; a larger one in
 *     memory, at an address the caller passes as a hidden first argument in r0 (the *_stret
 *     message senders, and CGRectInset at 0xD7EC0: r0 = result, the CGRect in r1-r3, [sp]).
 *   - Variadic arguments are passed exactly like fixed ones (floats promoted to double by
 *     the caller). A va_list is a guest pointer to the next argument word in memory.
 *
 * Header-only, C17 that also compiles as C++20, like maplethief/recomp/. */
#ifndef MAPLETHIEF_HLE_ABI_H
#define MAPLETHIEF_HLE_ABI_H

#include <stdint.h>
#include <string.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* Reads arguments in order. ncrn: next core register (0-4); nsaa: guest address of the next
 * stacked word. */
typedef struct mt_args {
    mt_cpu *cpu;
    uint32_t ncrn;
    uint32_t nsaa;
    uint32_t aapcs; /* 1: AAPCS 64-bit alignment; 0: iOS (4-byte aligned 64-bit values) */
} mt_args;

/* Arguments of the current call, iOS rules. */
MT_INLINE mt_args mt_args_begin(mt_cpu *cpu) {
    mt_args a;
    a.cpu = cpu;
    a.ncrn = 0u;
    a.nsaa = cpu->r[MT_SP];
    a.aapcs = 0u;
    return a;
}

/* Arguments of the current call, base AAPCS rules (64-bit values in an even/odd register
 * pair or an 8-byte aligned stack slot). Not what iOS uses; for code that follows AAPCS. */
MT_INLINE mt_args mt_args_begin_aapcs(mt_cpu *cpu) {
    mt_args a = mt_args_begin(cpu);
    a.aapcs = 1u;
    return a;
}

/* Arguments of a function that returns a structure in memory: *result gets the hidden
 * pointer (r0), and the cursor starts at r1. */
MT_INLINE mt_args mt_args_begin_stret(mt_cpu *cpu, uint32_t *result) {
    mt_args a = mt_args_begin(cpu);
    *result = cpu->r[0];
    a.ncrn = 1u;
    return a;
}

/* Arguments from a guest va_list (a pointer to the next word in guest memory). */
MT_INLINE mt_args mt_args_va_list(mt_cpu *cpu, uint32_t ap) {
    mt_args a = mt_args_begin(cpu);
    a.ncrn = 4u;
    a.nsaa = ap;
    return a;
}

/* The next 32-bit word: a pointer, an int of up to 32 bits (sign- or zero-extended by the
 * caller), a float's bits. */
MT_INLINE uint32_t mt_arg_u32(mt_args *a) {
    uint32_t v;
    if (a->ncrn < 4u) {
        return a->cpu->r[a->ncrn++];
    }
    v = mt_ld32(a->cpu, a->nsaa, 0u);
    a->nsaa += 4u;
    return v;
}

MT_INLINE int32_t mt_arg_i32(mt_args *a) { return (int32_t)mt_arg_u32(a); }

/* The next 64-bit value (long long, double's bits): low word first. */
MT_INLINE uint64_t mt_arg_u64(mt_args *a) {
    uint32_t lo, hi;
    if (a->aapcs) {
        if (a->ncrn < 4u) {
            a->ncrn = (a->ncrn + 1u) & ~1u;
        }
        if (a->ncrn >= 4u) {
            a->ncrn = 4u;
            a->nsaa = (a->nsaa + 7u) & ~7u;
        }
    }
    lo = mt_arg_u32(a);
    hi = mt_arg_u32(a);
    return ((uint64_t)hi << 32) | lo;
}

MT_INLINE int64_t mt_arg_i64(mt_args *a) { return (int64_t)mt_arg_u64(a); }

MT_INLINE float mt_arg_f32(mt_args *a) { return mt_f32(mt_arg_u32(a)); }

MT_INLINE double mt_arg_f64(mt_args *a) { return mt_f64(mt_arg_u64(a)); }

/* A structure passed by value: `size` bytes (rounded up to whole words) into `out`. */
MT_INLINE void mt_arg_struct(mt_args *a, void *out, uint32_t size) {
    uint8_t *dst = (uint8_t *)out;
    uint32_t done;
    for (done = 0u; done < size; done += 4u) {
        uint32_t word = mt_arg_u32(a);
        uint32_t n = size - done < 4u ? size - done : 4u;
        memcpy(dst + done, &word, n);
    }
}

/* Results. */
MT_INLINE void mt_ret_u32(mt_cpu *cpu, uint32_t v) { cpu->r[0] = v; }
MT_INLINE void mt_ret_u64(mt_cpu *cpu, uint64_t v) {
    cpu->r[0] = (uint32_t)v;
    cpu->r[1] = (uint32_t)(v >> 32);
}
MT_INLINE void mt_ret_f32(mt_cpu *cpu, float v) { cpu->r[0] = mt_f32_bits(v); }
MT_INLINE void mt_ret_f64(mt_cpu *cpu, double v) { mt_ret_u64(cpu, mt_f64_bits(v)); }

/* A structure result of `size` bytes: in r0 if it has at most 4 bytes, otherwise stored at
 * guest address `result` (the hidden pointer of mt_args_begin_stret). */
MT_INLINE void mt_ret_struct(mt_cpu *cpu, uint32_t result, const void *value, uint32_t size) {
    if (size <= 4u) {
        uint32_t word = 0u;
        memcpy(&word, value, size);
        cpu->r[0] = word;
        return;
    }
    memcpy(mt_host(cpu, result, size, 0u), value, size);
}

/* The return address of an HLE function that a guest function calls back into: the guest
 * function returns to it (MT_RETURN checks it against its entry lr). Never executed. */
#define MT_HLE_RETURN 0xFFFFFFF5u

/* Calls guest function pointer `fn` (Thumb bit significant) with `argc` <= 4 word arguments
 * in r0-r3 from an HLE function; returns r0 (r1 holds the high word of a 64-bit result).
 * lr is preserved for the HLE function's own caller. r0-r3 and r12 are clobbered, as by any
 * call; the guest preserves r4-r11 and sp. `site` is the guest address reported if `fn` is
 * not a known function (e.g. the import's guest code address). */
MT_INLINE uint32_t mt_call_guest(mt_cpu *cpu, uint32_t fn, const uint32_t *args, uint32_t argc,
                                 uint32_t site) {
    uint32_t lr = cpu->r[MT_LR];
    uint32_t i;
    for (i = 0u; i < argc && i < 4u; i++) {
        cpu->r[i] = args[i];
    }
    cpu->r[MT_LR] = MT_HLE_RETURN;
    mt_dispatch(cpu, fn, site)(cpu);
    cpu->r[MT_LR] = lr;
    return cpu->r[0];
}

MT_EXTERN_C_END

#endif /* MAPLETHIEF_HLE_ABI_H */
