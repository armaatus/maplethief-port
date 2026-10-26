// The recompiler's support headers (runtime/include/maplethief/recomp/, ADR 0014) compile as
// C++20 as well as C17: the runtime includes them to read and write the guest state.
#include "maplethief/recomp/dispatch.h"
#include "maplethief/recomp/recomp.h"
#include "maplethief/sjlj.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

static_assert(offsetof(mt_cpu, r) == 0);
static_assert(offsetof(mt_cpu, d) == 72);
static_assert(sizeof(mt_cpu::d) == 32 * 8);
static_assert(MT_TRAP_NO_FUNCTION == 8);
static_assert(offsetof(mt_fn_index, first) == 16);

TEST_CASE("single registers alias the halves of d0-d15") {
    mt_cpu cpu{};
    mt_s_set(&cpu, 0u, 0x11111111u);
    mt_s_set(&cpu, 1u, 0x22222222u);
    mt_s_set(&cpu, 31u, 0x33333333u);
    CHECK(cpu.d[0] == 0x2222222211111111ull);
    CHECK(cpu.d[15] == 0x3333333300000000ull);
    CHECK(mt_s_get(&cpu, 1u) == 0x22222222u);
}

TEST_CASE("VFP helpers give the iOS mode's results on every host (ADR 0022)") {
    CHECK(mt_vadd_f32(0x7F800000u, 0xFF800000u) == MT_F32_DEFAULT_NAN); // inf + -inf
    CHECK(mt_vsqrt_f64(0xBFF0000000000000ull) == MT_F64_DEFAULT_NAN);   // sqrt(-1)
    CHECK(mt_vmul_f32(0x7F800001u, 0x7FC00000u) == MT_F32_DEFAULT_NAN); // DN: no propagation
    CHECK(mt_vadd_f32(0x3F800000u, 0x40000000u) == 0x40400000u);        // 1 + 2 = 3
    CHECK(mt_vcmp_f32(0x3F800000u, 0x7FC00000u) == 0x30000000u);        // unordered: C, V
    CHECK(mt_vadd_f32(0x00000001u, 0x00800000u) == 0x00800000u);        // FZ: denormal in
    CHECK(mt_vmul_f32(0x3F7FFFFFu, 0x00800000u) == 0u);                 // FZ before rounding
    CHECK(mt_vcmp_f32(0x00000001u, 0u) == 0x60000000u);                 // denormal == 0
}

TEST_CASE("float to integer conversions saturate and map NaN to 0 (FPToFixed)") {
    CHECK(mt_vcvt_s32_f32(0x7FC00000u) == 0u);
    CHECK(mt_vcvt_s32_f32(0x4F000000u) == 0x7FFFFFFFu);  // 2^31
    CHECK(mt_vcvt_s32_f32(0xFF800000u) == 0x80000000u);  // -inf
    CHECK(mt_vcvt_u32_f64(0xBFF0000000000000ull) == 0u); // -1.0
    CHECK(mt_vcvt_u32_f32(0x4F800000u) == 0xFFFFFFFFu);  // 2^32
    CHECK(mt_vcvt_f32_u32(0xFFFFFFFFu) == 0x4F800000u);
    CHECK(mt_neon_vcvt_s32_f32(0xC0200000BFC00000ull) == 0xFFFFFFFEFFFFFFFFull); // -2.5, -1.5
}

TEST_CASE("integer helpers follow ARM, not C, for out-of-range shifts") {
    CHECK(mt_lsl(1u, 32u) == 0u);
    CHECK(mt_asr(0x80000000u, 200u) == 0xFFFFFFFFu);
    CHECK(mt_lsl_c(0x80000001u, 1u, 0u) == 1u);
    CHECK(mt_lsr_c(0x80000000u, 32u, 0u) == 1u);
    CHECK(mt_ror(0x12345678u, 8u) == 0x78123456u);
    CHECK(mt_sx8(0x80u) == 0xFFFFFF80u);
    CHECK(mt_sx16(0x7FFFu) == 0x7FFFu);
    CHECK(mt_clz(0u) == 32u);
    CHECK(mt_rev(0x11223344u) == 0x44332211u);
}

TEST_CASE("guest memory reads and writes little-endian bytes in the window") {
    std::uint8_t window[64] = {};
    mt_cpu cpu{};
    cpu.mem = window;
    cpu.mem_size = sizeof window;
    mt_st32(&cpu, 3u, 0xA1B2C3D4u, 0u); // unaligned, as ARMv7 allows
    CHECK(window[3] == 0xD4);
    CHECK(window[6] == 0xA1);
    CHECK(mt_ld16(&cpu, 4u, 0u) == 0xB2C3u);
    CHECK(mt_ld64(&cpu, 0u, 0u) == 0x00A1B2C3D4000000ull);
}

TEST_CASE("the SjLj unwinder's header compiles as C++ and matches the guest context") {
    static_assert(MT_SJLJ_FC_JBUF == 0x20u && MT_SJLJ_FC_LSDA == 0x1Cu);
    mt_sjlj_state state{}; // zero-initialised: the empty state
    CHECK(state.top == 0u);
    CHECK(state.frames[MT_SJLJ_MAX_FRAMES - 1].jbuf == 0u);
}

// The bounds-checked model needs the runtime's fault handler; the test provides one.
extern "C" void mt_mem_fault(mt_cpu *, std::uint32_t, std::uint32_t, std::uint32_t) noexcept {
    std::abort();
}
