# 22. Guest floating-point mode: iOS RunFast (FPSCR = DN | FZ), one fixed mode for VFP and NEON

- **Status:** Accepted
- **Date:** 2026-10-26
- **Issue:** #34
- **Supersedes:** the FPSCR part of [ADR 0014](0014-recompiler-design.md) "VFP" ("the rounding mode stays nearest-even and FZ/DN stay 0")

## Context

ADR 0014 assumed the guest's FPSCR starts at 0, because the binary never writes it: all 121 `vmrs` are `vmrs APSR_nzcv, fpscr` and there is no `vmsr` ([ANALYSIS.md](../ANALYSIS.md#instruction-mix)). Under that assumption VFP keeps denormals and propagates NaN operands, and only NEON (which always uses the ARM ARM's `StandardFPSCRValue()`) would flush to zero and return the default NaN.

But a thread does not start with FPSCR = 0 on iOS. The kernel sets it when it creates the thread's VFP state:

- **32-bit XNU** (`osfmk/arm/status.c`, `vfp_state_initialize`): `vfp_state->fpscr = FPSCR_DEFAULT;` under the comment "Set default VFP state to RunFast mode: flush-to-zero mode, default NaN mode, no enabled exceptions. On the VFP11, this allows the use of floating point without trapping to support code, which we do not provide. With the Cortex-A8, this allows the use of the (much faster) NFP pipeline for single-precision operations." `osfmk/arm/proc_reg.h` defines `FPSCR_DEFAULT` as `FPSCR_DN | FPSCR_FZ`, with `FPSCR_DN = 0x02000000` and `FPSCR_FZ = 0x01000000`. The source is [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu), tag `xnu-4570.1.46`.
- **64-bit XNU running a 32-bit process** (iPhone 5s to iOS 10, the last iOS with 32-bit apps): `osfmk/arm64/proc_reg.h` has `#define FPCR_DEFAULT_32 (FPCR_DN|FPCR_FZ)`, while 64-bit processes get only `FPCR_DN`.

The game targets iOS 3–4 (2010), and XNU's ARM sources were first published years later. The comment names the VFP11 (the ARMv6 core of the first iPhones) and the Cortex-A8 (iPhone 3GS), and both kernel lines agree, so the policy dates from the start and covers every device the game ran on. A check on a real armv7 device would confirm it beyond doubt (see Consequences).

So the guest computes with **FPSCR = 0x03000000**: round to nearest (RMode 00), flush-to-zero (FZ), default NaN (DN), and no trapped exceptions. That is exactly `StandardFPSCRValue()`, the mode NEON always uses (AHP is irrelevant: the binary has no half precision). VFP and NEON therefore behave alike in this binary.

It matters for bit-exactness. Under FZ, a denormal operand is zero (`FPUnpack`), so `vcmpe` of a denormal against `#0` is *equal*, `vcvt.f64.f32` of a denormal is ±0, and 2^-149 + 2^-126 = 2^-126. A result whose exact value is below 2^-126 (or 2^-1022) becomes a signed zero, and `FPRound` decides that *before* rounding: 2^-126 − 2^-150 (a tie the host rounds up to 2^-126) is flushed. Under DN, every NaN result is `0x7FC00000` / `0x7FF8000000000000`, whatever the operands' sign and payload.

## Options considered

1. **Keep FPSCR = 0 (ADR 0014).** It matches the host's IEEE default and needs no flushing code. It is wrong for this binary: it would differ from the device on every denormal and every NaN payload, and VFP would then disagree with NEON on the same values.
2. **Read the mode from `cpu->fpscr` at run time.** This is general, but every operation would branch on FZ and DN, for a register the binary never writes.
3. **One fixed mode, FPSCR = 0x03000000, compiled into the helpers.** It is exact for this binary, has no run-time mode test, and gives one set of helpers for VFP and NEON. If a `vmsr` ever appears, the recompiler must notice (see Consequences).

## Decision

Option 3. The recompiled code implements the ARM ARM floating-point pseudocode for **FPSCR = 0x03000000** (`MT_FPSCR_IOS` in `runtime/include/maplethief/recomp/cpu.h`) and nothing else:

- **Helpers** (`runtime/include/maplethief/recomp/vfp.h`) work on bit patterns and use the host's IEEE arithmetic for the rounding. Around it they apply the ARM rules explicitly:
  - inputs are flushed (`mt_f32_fz` / `mt_f64_fz`);
  - a NaN result becomes the default NaN;
  - a tiny result becomes a signed zero, judged on the exact value. Add and subtract results that small are exact. An f32 product is exact as a double. For f64 multiply, and for divide (where a tie such as `0x00FFFFFF / 2` does round up to the smallest normal), an exact comparison settles the one rounded value that is ambiguous, 2^-126 or 2^-1022.
  - `vcmp` flushes too. `vneg`/`vabs` and NEON `vneg` change only the sign bit (FPNeg, FPAbs).
- **Conversions to integer** (`vcvt.s32/u32.f32/f64`, VFP and NEON) round toward zero, give 0 for a NaN, and saturate out-of-range values and infinities (`FPToFixed` + `SatQ`). The C code range-checks before every cast, because C leaves out-of-range conversion undefined and x86 returns `0x80000000`. **Conversions from integer** go through an exact double and round once.
- **NEON f32 lanes** use the same helpers (`mt_neon_*`, two lanes per D register; a Q register is two D halves).
- **`cpu->fpscr`** starts at `MT_FPSCR_IOS` (the snippet runtime does this; the game runtime must too). Only its NZCV bits change (`vcmp`/`vcmpe`, read by `vmrs APSR_nzcv`), and the generated code ignores its control bits.
- **Host requirement:** the host FPU runs in its IEEE default (no FTZ/DAZ in MXCSR or FPCR, round to nearest), and contraction stays off (ADR 0014). The helpers flush explicitly and rely on the host keeping denormals.

Reference values (all in hand-written snippets whose expectations come from the ARM ARM pseudocode, cross-checked against QEMU through Unicorn 2.1.4 with FPSCR = 0x03000000; with FPSCR = 0, Unicorn disagrees on 13 of them, which shows the snippets do distinguish the modes):

| Operation | Input | Result |
| --- | --- | --- |
| `vadd.f32` | 2^-149 (denormal) + 2^-126 | 0x00800000 (FZ = 0: 0x00800001) |
| `vsub.f32` | 0x00800000 − 0x00800001 | 0x80000000 (−0) |
| `vmul.f32` | (1 − 2^-24) × 2^-126 | 0 (it would round to 2^-126) |
| `vmul.f64` | (1 − 2^-53) × 2^-1022 | 0 |
| `vdiv.f32` | 0x00FFFFFF / 2.0 | 0 (a tie that rounds to 2^-126) |
| `vdiv.f32` | denormal / denormal | 0x7FC00000 (0 / 0) |
| `vadd.f32` | 0xFFC00123 + 1.0 | 0x7FC00000 (FZ = 0: 0xFFC00123) |
| `vcmpe.f32` | 0x00000001 vs #0 | equal (Z, C) |
| `vcvt.f64.f32` | 0x80000001 / 0x7F800001 | 0x8000000000000000 / 0x7FF8000000000000 |
| `vcvt.f32.f64` | 2^-126 − 2^-160 / −1e300 | 0 / 0xFF800000 |
| `vcvt.s32.f32` | NaN, +inf, 2^31, −2^31 − 256, −0.99 | 0, 0x7FFFFFFF, 0x7FFFFFFF, 0x80000000, 0 |
| `vcvt.s32.f64` | −2147483648.9, −2147483649.0, 2147483647.9 | 0x80000000, 0x80000000, 0x7FFFFFFF |
| `vcvt.u32.f64` | −0.9, −1.0, 4294967295.9, −inf, NaN | 0, 0, 0xFFFFFFFF, 0, 0 |
| `vcvt.f32.s32` | 2^24 + 1, 2^31 − 1 | 0x4B800000, 0x4F000000 (ties to even) |
| `vcvt.f32.u32` | 0xFFFFFFFF | 0x4F800000 |
| NEON `vneg.f32` | 0x7FC00001, 0x00000001 | 0xFFC00001, 0x80000001 (sign only) |

## Consequences

- **Bit-exact on every host.** The suite runs every snippet with clang/gcc and MSVC in CI. A scratch differential run (not in the repo; #41 builds the real harness) compared 145,000 random and boundary-targeted vectors for every VFP/NEON arithmetic, compare and conversion helper with Unicorn: no difference at `-O0` or `-O2`, on arm64 and on x86_64 (Rosetta).
- **Two ADR 0014-era snippets changed** their expectation to the default NaN. They had expected NaN propagation, and they are renamed `vadd_f32_*_nan_gives_default_nan`.
- **A `vmsr` would silently be ignored.** The binary has none (the census would show the form). If a later binary or patch has one, add a census check and either trap or model the mode.
- **The game runtime** must initialise each guest thread's `cpu->fpscr` to `MT_FPSCR_IOS`. It must also keep the host FPU in IEEE default mode around generated code; audio or graphics libraries that set FTZ/DAZ on their own threads don't matter, since guest code runs on our threads.
- **How we'd know this was wrong:** a playthrough or a real-device trace disagrees on a denormal or NaN value. The check is cheap: on a jailbroken or development armv7 device, `vmrs r0, fpscr` at app start should read `0x03000000`. If Joris has such a device, that is a `needs-human` confirmation. Until then, the XNU sources above are the evidence.
