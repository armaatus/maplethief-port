/* VFP helpers for recompiled code (ADR 0014; completed by #34, which adds the NEON lane
 * operations, conversions to and from integers and the remaining forms).
 *
 * Registers are bit patterns in cpu->d[]; arithmetic converts to float / double with memcpy
 * (exact, no aliasing problems), computes in the host's IEEE arithmetic and converts back.
 * Host and guest agree on every result for round-to-nearest-even with denormals kept (the
 * guest FPSCR never changes, see cpu.h) except for which NaN comes out: ARM returns the
 * first signalling, else the first quiet NaN operand, quietened, and its default NaN
 * (0x7FC00000 / 0x7FF8000000000000) when an operation creates one; x86 creates a NaN with
 * the sign bit set. The mt_v*_f32/f64 helpers apply the ARM rules, so results are
 * bit-identical on every host. Floating-point contraction must be off: a guest multiply and
 * add are two roundings (pragma below; the build also passes -ffp-contract=off). */
#ifndef MAPLETHIEF_RECOMP_VFP_H
#define MAPLETHIEF_RECOMP_VFP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "maplethief/recomp/compiler.h"
#include "maplethief/recomp/cpu.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

MT_EXTERN_C_BEGIN

/* Single-precision register n (0-31): a half of d[n / 2]. */
MT_INLINE uint32_t mt_s_get(const mt_cpu *MT_RESTRICT cpu, unsigned n) {
    return (uint32_t)(cpu->d[n >> 1] >> ((n & 1u) * 32u));
}
MT_INLINE void mt_s_set(mt_cpu *MT_RESTRICT cpu, unsigned n, uint32_t bits) {
    unsigned shift = (n & 1u) * 32u;
    cpu->d[n >> 1] =
        (cpu->d[n >> 1] & ~((uint64_t)0xFFFFFFFFu << shift)) | ((uint64_t)bits << shift);
}

MT_INLINE float mt_f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
MT_INLINE uint32_t mt_f32_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}
MT_INLINE double mt_f64(uint64_t bits) {
    double f;
    memcpy(&f, &bits, 8);
    return f;
}
MT_INLINE uint64_t mt_f64_bits(double f) {
    uint64_t bits;
    memcpy(&bits, &f, 8);
    return bits;
}

/* NaN selection of the ARM ARM's FPProcessNaNs (FPSCR.DN = 0). */
#define MT_F32_DEFAULT_NAN 0x7FC00000u
#define MT_F64_DEFAULT_NAN 0x7FF8000000000000ull
MT_INLINE int mt_f32_is_nan(uint32_t x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }
MT_INLINE int mt_f32_is_snan(uint32_t x) { return mt_f32_is_nan(x) && !(x & 0x00400000u); }
MT_INLINE int mt_f64_is_nan(uint64_t x) {
    return (x & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
}
MT_INLINE int mt_f64_is_snan(uint64_t x) {
    return mt_f64_is_nan(x) && !(x & 0x0008000000000000ull);
}

MT_INLINE uint32_t mt_f32_nan2(uint32_t r, uint32_t a, uint32_t b) {
    if (MT_LIKELY(!mt_f32_is_nan(r))) {
        return r;
    }
    if (mt_f32_is_snan(a)) {
        return a | 0x00400000u;
    }
    if (mt_f32_is_snan(b)) {
        return b | 0x00400000u;
    }
    if (mt_f32_is_nan(a)) {
        return a;
    }
    if (mt_f32_is_nan(b)) {
        return b;
    }
    return MT_F32_DEFAULT_NAN;
}
MT_INLINE uint64_t mt_f64_nan2(uint64_t r, uint64_t a, uint64_t b) {
    if (MT_LIKELY(!mt_f64_is_nan(r))) {
        return r;
    }
    if (mt_f64_is_snan(a)) {
        return a | 0x0008000000000000ull;
    }
    if (mt_f64_is_snan(b)) {
        return b | 0x0008000000000000ull;
    }
    if (mt_f64_is_nan(a)) {
        return a;
    }
    if (mt_f64_is_nan(b)) {
        return b;
    }
    return MT_F64_DEFAULT_NAN;
}

/* Arithmetic on bit patterns, with ARM NaN results. */
MT_INLINE uint32_t mt_vadd_f32(uint32_t a, uint32_t b) {
    return mt_f32_nan2(mt_f32_bits(mt_f32(a) + mt_f32(b)), a, b);
}
MT_INLINE uint32_t mt_vsub_f32(uint32_t a, uint32_t b) {
    return mt_f32_nan2(mt_f32_bits(mt_f32(a) - mt_f32(b)), a, b);
}
MT_INLINE uint32_t mt_vmul_f32(uint32_t a, uint32_t b) {
    return mt_f32_nan2(mt_f32_bits(mt_f32(a) * mt_f32(b)), a, b);
}
MT_INLINE uint32_t mt_vdiv_f32(uint32_t a, uint32_t b) {
    return mt_f32_nan2(mt_f32_bits(mt_f32(a) / mt_f32(b)), a, b);
}
MT_INLINE uint32_t mt_vsqrt_f32(uint32_t a) {
    return mt_f32_nan2(mt_f32_bits(sqrtf(mt_f32(a))), a, a);
}
MT_INLINE uint64_t mt_vadd_f64(uint64_t a, uint64_t b) {
    return mt_f64_nan2(mt_f64_bits(mt_f64(a) + mt_f64(b)), a, b);
}
MT_INLINE uint64_t mt_vsub_f64(uint64_t a, uint64_t b) {
    return mt_f64_nan2(mt_f64_bits(mt_f64(a) - mt_f64(b)), a, b);
}
MT_INLINE uint64_t mt_vmul_f64(uint64_t a, uint64_t b) {
    return mt_f64_nan2(mt_f64_bits(mt_f64(a) * mt_f64(b)), a, b);
}
MT_INLINE uint64_t mt_vdiv_f64(uint64_t a, uint64_t b) {
    return mt_f64_nan2(mt_f64_bits(mt_f64(a) / mt_f64(b)), a, b);
}
MT_INLINE uint64_t mt_vsqrt_f64(uint64_t a) {
    return mt_f64_nan2(mt_f64_bits(sqrt(mt_f64(a))), a, a);
}

/* vcvt.f64.f32 / vcvt.f32.f64 (round to nearest; a NaN is quietened, its payload kept as
 * far as it fits, which is what the host conversion does too). */
MT_INLINE uint64_t mt_vcvt_f64_f32(uint32_t a) {
    if (mt_f32_is_nan(a)) {
        return ((uint64_t)(a & 0x80000000u) << 32) | 0x7FF8000000000000ull |
               ((uint64_t)(a & 0x003FFFFFu) << 29);
    }
    return mt_f64_bits((double)mt_f32(a));
}
MT_INLINE uint32_t mt_vcvt_f32_f64(uint64_t a) {
    if (mt_f64_is_nan(a)) {
        return (uint32_t)((a >> 32) & 0x80000000u) | 0x7FC00000u |
               (uint32_t)((a >> 29) & 0x003FFFFFu);
    }
    return mt_f32_bits((float)mt_f64(a));
}

/* vcmp / vcmpe: the FPSCR N, Z, C, V bits (31..28) of comparing a with b. */
MT_INLINE uint32_t mt_vcmp_f32(uint32_t a, uint32_t b) {
    float x = mt_f32(a), y = mt_f32(b);
    return x == y ? 0x60000000u : x < y ? 0x80000000u : x > y ? 0x20000000u : 0x30000000u;
}
MT_INLINE uint32_t mt_vcmp_f64(uint64_t a, uint64_t b) {
    double x = mt_f64(a), y = mt_f64(b);
    return x == y ? 0x60000000u : x < y ? 0x80000000u : x > y ? 0x20000000u : 0x30000000u;
}

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_VFP_H */
