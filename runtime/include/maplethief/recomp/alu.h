/* Integer helpers for recompiled code (ADR 0014; the full set grows with #33).
 *
 * Generated code does its arithmetic on uint32_t, where wrap-around is defined, and writes
 * flag computations inline. These helpers cover what C does not define for every input:
 * shifts by an amount of 32 or more (ARM takes the bottom byte of the shift register),
 * rotations, sign extension and the bit-manipulation instructions. All are portable C; a
 * compiler builtin is used only where a fallback exists. */
#ifndef MAPLETHIEF_RECOMP_ALU_H
#define MAPLETHIEF_RECOMP_ALU_H

#include <stdint.h>

#include "maplethief/recomp/compiler.h"

MT_EXTERN_C_BEGIN

/* Sign extension without implementation-defined conversions. */
MT_INLINE uint32_t mt_sx8(uint32_t x) { return ((x & 0xFFu) ^ 0x80u) - 0x80u; }
MT_INLINE uint32_t mt_sx16(uint32_t x) { return ((x & 0xFFFFu) ^ 0x8000u) - 0x8000u; }

/* Shifts by register (LSL/LSR/ASR/ROR Rd, Rn, Rm): `n` is the bottom byte of Rm, 0-255. */
MT_INLINE uint32_t mt_lsl(uint32_t x, uint32_t n) { return n >= 32u ? 0u : x << n; }
MT_INLINE uint32_t mt_lsr(uint32_t x, uint32_t n) { return n >= 32u ? 0u : x >> n; }
MT_INLINE uint32_t mt_asr(uint32_t x, uint32_t n) {
    return (uint32_t)((int32_t)x >> (n >= 32u ? 31u : n));
}
MT_INLINE uint32_t mt_ror(uint32_t x, uint32_t n) {
    n &= 31u;
    return n == 0u ? x : (x >> n) | (x << (32u - n));
}

/* The same with the shifter carry-out (ARM ARM Shift_C). `c` is the carry flag before the
 * instruction; it is the result's carry when n == 0. */
MT_INLINE uint32_t mt_lsl_c(uint32_t x, uint32_t n, uint32_t c) {
    return n == 0u ? c : n <= 32u ? (uint32_t)(((uint64_t)x << n) >> 32) & 1u : 0u;
}
MT_INLINE uint32_t mt_lsr_c(uint32_t x, uint32_t n, uint32_t c) {
    return n == 0u ? c : n <= 32u ? (x >> (n - 1u)) & 1u : 0u;
}
MT_INLINE uint32_t mt_asr_c(uint32_t x, uint32_t n, uint32_t c) {
    return n == 0u ? c : n <= 32u ? (uint32_t)((int32_t)x >> (n - 1u)) & 1u : x >> 31;
}
MT_INLINE uint32_t mt_ror_c(uint32_t x, uint32_t n, uint32_t c) {
    return n == 0u ? c : (mt_ror(x, n) >> 31);
}

MT_INLINE uint32_t mt_clz(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x == 0u ? 32u : (uint32_t)__builtin_clz(x);
#else
    uint32_t n = 0u;
    if (x == 0u) {
        return 32u;
    }
    while ((x & 0x80000000u) == 0u) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}

MT_INLINE uint32_t mt_rev(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}
MT_INLINE uint32_t mt_rev16(uint32_t x) {
    return ((x >> 8) & 0x00FF00FFu) | ((x << 8) & 0xFF00FF00u);
}

/* Signed multiplies: the 64-bit product of two guest words read as signed. */
MT_INLINE uint64_t mt_smul64(uint32_t a, uint32_t b) {
    return (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
}

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_ALU_H */
