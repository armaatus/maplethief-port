/* VFP and NEON helpers for recompiled code (ADR 0014, ADR 0022).
 *
 * Registers are bit patterns in cpu->d[]; arithmetic converts to float / double with memcpy
 * (exact, no aliasing problems), computes in the host's IEEE arithmetic and converts back.
 *
 * The guest's floating-point mode is fixed (ADR 0022): iOS starts every thread with FPSCR =
 * DN | FZ (0x03000000, MT_FPSCR_IOS in cpu.h) and the binary never writes FPSCR. NEON uses
 * the same mode always (the "standard FPSCR value"). So every helper here implements the ARM
 * ARM pseudocode with
 *   - round to nearest, ties to even (the host default);
 *   - flush-to-zero (FPSCR.FZ): an input denormal is +-0 (FPUnpack), and a result whose
 *     exact value is below the smallest normal is +-0 (FPRound decides before rounding, so a
 *     result that would round up to the smallest normal is flushed too);
 *   - default NaN (FPSCR.DN): every NaN result is 0x7FC00000 / 0x7FF8000000000000, whatever
 *     the operands (FPProcessNaN). x86 would create 0xFFC00000 and propagate payloads.
 * The host must run with its own IEEE defaults (no FTZ/DAZ in MXCSR or FPCR): the helpers
 * flush explicitly and rely on the host keeping denormals to see them. All range checks
 * are explicit, so no C conversion is ever out of range (C leaves that undefined, and x86
 * returns 0x80000000 where ARM saturates). Floating-point contraction must be off: a guest
 * multiply and add are two roundings (pragma below; the build also passes
 * -ffp-contract=off). */
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

#define MT_F32_SIGN 0x80000000u
#define MT_F64_SIGN 0x8000000000000000ull
#define MT_F32_DEFAULT_NAN 0x7FC00000u
#define MT_F64_DEFAULT_NAN 0x7FF8000000000000ull
#define MT_F32_MIN_NORMAL 0x00800000u           /* 2^-126 */
#define MT_F64_MIN_NORMAL 0x0010000000000000ull /* 2^-1022 */
#define MT_F64_OF_F32_MIN 0x3810000000000000ull /* 2^-126 as a double */

MT_INLINE int mt_f32_is_nan(uint32_t x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }
MT_INLINE int mt_f64_is_nan(uint64_t x) {
    return (x & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
}

/* FPUnpack with FPSCR.FZ = 1: a denormal input is a zero of the same sign. */
MT_INLINE uint32_t mt_f32_fz(uint32_t x) { return (x & 0x7F800000u) ? x : x & MT_F32_SIGN; }
MT_INLINE uint64_t mt_f64_fz(uint64_t x) {
    return (x & 0x7FF0000000000000ull) ? x : x & MT_F64_SIGN;
}

/* The result of an operation whose host result r is exactly rounded and is denormal only if
 * the exact value is (true for add and subtract: a sum that small is exact): a NaN becomes
 * the default NaN, a denormal is flushed. */
MT_INLINE uint32_t mt_f32_result(uint32_t r) {
    return mt_f32_is_nan(r) ? MT_F32_DEFAULT_NAN : mt_f32_fz(r);
}
MT_INLINE uint64_t mt_f64_result(uint64_t r) {
    return mt_f64_is_nan(r) ? MT_F64_DEFAULT_NAN : mt_f64_fz(r);
}

/* f32 arithmetic (VFP and NEON lanes). */
MT_INLINE uint32_t mt_vadd_f32(uint32_t a, uint32_t b) {
    return mt_f32_result(mt_f32_bits(mt_f32(mt_f32_fz(a)) + mt_f32(mt_f32_fz(b))));
}
MT_INLINE uint32_t mt_vsub_f32(uint32_t a, uint32_t b) {
    return mt_f32_result(mt_f32_bits(mt_f32(mt_f32_fz(a)) - mt_f32(mt_f32_fz(b))));
}
MT_INLINE uint32_t mt_vmul_f32(uint32_t a, uint32_t b) {
    /* The product of two floats is exact as a double (48 significant bits, exponent >=
     * -252), so the tininess test sees the exact value and (float) rounds once. */
    double p = (double)mt_f32(mt_f32_fz(a)) * (double)mt_f32(mt_f32_fz(b));
    uint64_t pb = mt_f64_bits(p);
    if (mt_f64_is_nan(pb)) {
        return MT_F32_DEFAULT_NAN;
    }
    if ((pb & ~MT_F64_SIGN) < MT_F64_OF_F32_MIN) {
        return (uint32_t)(pb >> 32) & MT_F32_SIGN;
    }
    return mt_f32_bits((float)p);
}
MT_INLINE uint32_t mt_vdiv_f32(uint32_t a, uint32_t b) {
    float x = mt_f32(mt_f32_fz(a)), y = mt_f32(mt_f32_fz(b));
    uint32_t q = mt_f32_bits(x / y);
    if (mt_f32_is_nan(q)) {
        return MT_F32_DEFAULT_NAN;
    }
    if ((q & 0x7F800000u) == 0) {
        return q & MT_F32_SIGN;
    }
    /* Rounded up to the smallest normal: flushed if the exact |x / y| is below it, that is
     * if |x| < 2^-126 * |y| (both sides exact as doubles). It happens: 0x00FFFFFF / 2 is
     * 2^-126 - 2^-150, a tie that rounds to the even 2^-126. */
    if ((q & ~MT_F32_SIGN) == MT_F32_MIN_NORMAL &&
        (mt_f64_bits((double)x) & ~MT_F64_SIGN) <
            (mt_f64_bits(mt_f64(MT_F64_OF_F32_MIN) * (double)y) & ~MT_F64_SIGN)) {
        return q & MT_F32_SIGN;
    }
    return q;
}
MT_INLINE uint32_t mt_vsqrt_f32(uint32_t a) {
    return mt_f32_result(mt_f32_bits(sqrtf(mt_f32(mt_f32_fz(a)))));
}

/* f64 arithmetic. */
MT_INLINE uint64_t mt_vadd_f64(uint64_t a, uint64_t b) {
    return mt_f64_result(mt_f64_bits(mt_f64(mt_f64_fz(a)) + mt_f64(mt_f64_fz(b))));
}
MT_INLINE uint64_t mt_vsub_f64(uint64_t a, uint64_t b) {
    return mt_f64_result(mt_f64_bits(mt_f64(mt_f64_fz(a)) - mt_f64(mt_f64_fz(b))));
}
/* Whether the exact |x * y| is below 2^-1022, for normal x and y whose rounded product is
 * 2^-1022: Dekker's exact product (hi + lo) of |x| * 2^512 and |y|, compared with 2^-510.
 * Both factors are at most about 1 here, so nothing overflows or underflows. */
MT_INLINE int mt_f64_mul_tiny(double x, double y) {
    const double split = 134217729.0; /* 2^27 + 1 */
    double a = mt_f64(mt_f64_bits(x) & ~MT_F64_SIGN) * mt_f64(0x5FF0000000000000ull);
    double b = mt_f64(mt_f64_bits(y) & ~MT_F64_SIGN);
    double ca = split * a, cb = split * b;
    double ah = ca - (ca - a), al = a - ah, bh = cb - (cb - b), bl = b - bh;
    double hi = a * b;
    double lo = ((ah * bh - hi) + ah * bl + al * bh) + al * bl;
    double limit = mt_f64(0x2010000000000000ull); /* 2^-510 */
    return hi < limit || (hi == limit && lo < 0.0);
}
MT_INLINE uint64_t mt_vmul_f64(uint64_t a, uint64_t b) {
    double x = mt_f64(mt_f64_fz(a)), y = mt_f64(mt_f64_fz(b));
    uint64_t p = mt_f64_bits(x * y);
    if (mt_f64_is_nan(p)) {
        return MT_F64_DEFAULT_NAN;
    }
    if ((p & 0x7FF0000000000000ull) == 0 ||
        ((p & ~MT_F64_SIGN) == MT_F64_MIN_NORMAL && mt_f64_mul_tiny(x, y))) {
        return p & MT_F64_SIGN;
    }
    return p;
}
MT_INLINE uint64_t mt_vdiv_f64(uint64_t a, uint64_t b) {
    double x = mt_f64(mt_f64_fz(a)), y = mt_f64(mt_f64_fz(b));
    uint64_t q = mt_f64_bits(x / y);
    if (mt_f64_is_nan(q)) {
        return MT_F64_DEFAULT_NAN;
    }
    if ((q & 0x7FF0000000000000ull) == 0) {
        return q & MT_F64_SIGN;
    }
    /* Rounded up to 2^-1022: flushed if |x| < 2^-1022 * |y|, compared as |x| * 2^600 <
     * |y| * 2^-422 (exact: a quotient this small has |x| <= 4 and |y| >= 1). */
    if ((q & ~MT_F64_SIGN) == MT_F64_MIN_NORMAL) {
        double lhs = mt_f64(mt_f64_bits(x) & ~MT_F64_SIGN) * mt_f64(0x6570000000000000ull);
        double rhs = mt_f64(mt_f64_bits(y) & ~MT_F64_SIGN) * mt_f64(0x2590000000000000ull);
        if (lhs < rhs) {
            return q & MT_F64_SIGN;
        }
    }
    return q;
}
MT_INLINE uint64_t mt_vsqrt_f64(uint64_t a) {
    return mt_f64_result(mt_f64_bits(sqrt(mt_f64(mt_f64_fz(a)))));
}

/* vcvt.f64.f32 / vcvt.f32.f64 (FPConvert: the input is flushed, a NaN gives the default
 * NaN, the result is rounded to nearest and flushed when the exact value is tiny). */
MT_INLINE uint64_t mt_vcvt_f64_f32(uint32_t a) {
    a = mt_f32_fz(a);
    return mt_f32_is_nan(a) ? MT_F64_DEFAULT_NAN : mt_f64_bits((double)mt_f32(a));
}
MT_INLINE uint32_t mt_vcvt_f32_f64(uint64_t a) {
    a = mt_f64_fz(a);
    if (mt_f64_is_nan(a)) {
        return MT_F32_DEFAULT_NAN;
    }
    if ((a & ~MT_F64_SIGN) < MT_F64_OF_F32_MIN) {
        return (uint32_t)(a >> 32) & MT_F32_SIGN;
    }
    return mt_f32_bits((float)mt_f64(a));
}

/* vcvt to an integer, round toward zero (FPToFixed with round_zero; fbits = 0): a NaN gives
 * 0, a value out of range saturates (+-inf included), and a negative value converted to
 * unsigned is 0 once it is -1 or below (above -1 it truncates to 0 anyway). Every float and
 * double is exact as a double, and each C conversion below is in range. */
MT_INLINE uint32_t mt_cvt_s32_double(double x) {
    if (x != x) {
        return 0u;
    }
    if (x >= 2147483648.0) {
        return 0x7FFFFFFFu;
    }
    if (x <= -2147483649.0) {
        return 0x80000000u;
    }
    return (uint32_t)(int32_t)x;
}
MT_INLINE uint32_t mt_cvt_u32_double(double x) {
    if (!(x >= 1.0)) {
        return 0u; /* NaN, negative, or below 1 */
    }
    if (x >= 4294967296.0) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)x;
}
MT_INLINE uint32_t mt_vcvt_s32_f32(uint32_t a) { return mt_cvt_s32_double((double)mt_f32(a)); }
MT_INLINE uint32_t mt_vcvt_u32_f32(uint32_t a) { return mt_cvt_u32_double((double)mt_f32(a)); }
MT_INLINE uint32_t mt_vcvt_s32_f64(uint64_t a) { return mt_cvt_s32_double(mt_f64(a)); }
MT_INLINE uint32_t mt_vcvt_u32_f64(uint64_t a) { return mt_cvt_u32_double(mt_f64(a)); }

/* vcvt from an integer (FixedToFP, round to nearest). The integer is exact as a double, so
 * (float) rounds once. */
MT_INLINE double mt_s32_double(uint32_t a) {
    return (double)(a ^ 0x80000000u) - 2147483648.0; /* exact; no signed conversion */
}
MT_INLINE uint32_t mt_vcvt_f32_s32(uint32_t a) { return mt_f32_bits((float)mt_s32_double(a)); }
MT_INLINE uint32_t mt_vcvt_f32_u32(uint32_t a) { return mt_f32_bits((float)(double)a); }
MT_INLINE uint64_t mt_vcvt_f64_s32(uint32_t a) { return mt_f64_bits(mt_s32_double(a)); }
MT_INLINE uint64_t mt_vcvt_f64_u32(uint32_t a) { return mt_f64_bits((double)a); }

/* vcmp / vcmpe: the FPSCR N, Z, C, V bits (31..28) of comparing a with b (FPCompare: the
 * inputs are flushed first, so a denormal equals zero). */
MT_INLINE uint32_t mt_vcmp_f32(uint32_t a, uint32_t b) {
    float x = mt_f32(mt_f32_fz(a)), y = mt_f32(mt_f32_fz(b));
    return x == y ? 0x60000000u : x < y ? 0x80000000u : x > y ? 0x20000000u : 0x30000000u;
}
MT_INLINE uint32_t mt_vcmp_f64(uint64_t a, uint64_t b) {
    double x = mt_f64(mt_f64_fz(a)), y = mt_f64(mt_f64_fz(b));
    return x == y ? 0x60000000u : x < y ? 0x80000000u : x > y ? 0x20000000u : 0x30000000u;
}

/* NEON: one 64-bit D register as two f32 / 32-bit lanes (lane 0 = the low half). The f32
 * lane operations use the same mode as VFP above (StandardFPSCRValue: DN, FZ, nearest). */
MT_INLINE uint64_t mt_lanes(uint32_t lo, uint32_t hi) { return (uint64_t)hi << 32 | lo; }
#define MT_LO(x) ((uint32_t)(x))
#define MT_HI(x) ((uint32_t)((x) >> 32))
MT_INLINE uint64_t mt_neon_vadd_f32(uint64_t a, uint64_t b) {
    return mt_lanes(mt_vadd_f32(MT_LO(a), MT_LO(b)), mt_vadd_f32(MT_HI(a), MT_HI(b)));
}
MT_INLINE uint64_t mt_neon_vsub_f32(uint64_t a, uint64_t b) {
    return mt_lanes(mt_vsub_f32(MT_LO(a), MT_LO(b)), mt_vsub_f32(MT_HI(a), MT_HI(b)));
}
MT_INLINE uint64_t mt_neon_vmul_f32(uint64_t a, uint64_t b) {
    return mt_lanes(mt_vmul_f32(MT_LO(a), MT_LO(b)), mt_vmul_f32(MT_HI(a), MT_HI(b)));
}
MT_INLINE uint64_t mt_neon_vcvt_f32_s32(uint64_t a) {
    return mt_lanes(mt_vcvt_f32_s32(MT_LO(a)), mt_vcvt_f32_s32(MT_HI(a)));
}
MT_INLINE uint64_t mt_neon_vcvt_f32_u32(uint64_t a) {
    return mt_lanes(mt_vcvt_f32_u32(MT_LO(a)), mt_vcvt_f32_u32(MT_HI(a)));
}
MT_INLINE uint64_t mt_neon_vcvt_s32_f32(uint64_t a) {
    return mt_lanes(mt_vcvt_s32_f32(MT_LO(a)), mt_vcvt_s32_f32(MT_HI(a)));
}
MT_INLINE uint64_t mt_neon_vcvt_u32_f32(uint64_t a) {
    return mt_lanes(mt_vcvt_u32_f32(MT_LO(a)), mt_vcvt_u32_f32(MT_HI(a)));
}

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_VFP_H */
