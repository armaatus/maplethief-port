// libgcc_s.1.dylib: the integer division and conversion helpers clang calls on armv7, which
// has no divide instruction (ADR 0019). On iOS libgcc_s re-exports compiler-rt, whose generic
// C versions define the results here: a division by zero gives a quotient of 0 (and so a
// remainder equal to the dividend), INT_MIN / -1 wraps to INT_MIN.
#include "hle/hle.h"

#include <cstdint>

#include "maplethief/hle/abi.h"

namespace {

std::uint32_t udiv32(std::uint32_t a, std::uint32_t b) { return b == 0u ? 0u : a / b; }

// Signed quotient through magnitudes, as compiler-rt does, so INT_MIN / -1 is defined.
std::uint32_t sdiv32(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t sa = a >> 31, sb = b >> 31;
    const std::uint32_t q = udiv32(sa != 0u ? 0u - a : a, sb != 0u ? 0u - b : b);
    return sa != sb ? 0u - q : q;
}

std::uint64_t udiv64(std::uint64_t a, std::uint64_t b) { return b == 0u ? 0u : a / b; }

std::uint64_t sdiv64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t sa = a >> 63, sb = b >> 63;
    const std::uint64_t q = udiv64(sa != 0u ? 0u - a : a, sb != 0u ? 0u - b : b);
    return sa != sb ? 0u - q : q;
}

} // namespace

extern "C" {

// int __divsi3(int a, int b)
void mt_hle_divsi3(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t n = mt_arg_u32(&a), d = mt_arg_u32(&a);
    mt_ret_u32(cpu, sdiv32(n, d));
}

// int __modsi3(int a, int b): a - (a / b) * b
void mt_hle_modsi3(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t n = mt_arg_u32(&a), d = mt_arg_u32(&a);
    mt_ret_u32(cpu, n - sdiv32(n, d) * d);
}

// unsigned __udivsi3(unsigned a, unsigned b)
void mt_hle_udivsi3(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t n = mt_arg_u32(&a), d = mt_arg_u32(&a);
    mt_ret_u32(cpu, udiv32(n, d));
}

// unsigned __umodsi3(unsigned a, unsigned b)
void mt_hle_umodsi3(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t n = mt_arg_u32(&a), d = mt_arg_u32(&a);
    mt_ret_u32(cpu, n - udiv32(n, d) * d);
}

// long long __divdi3(long long a, long long b): a in r0:r1, b in r2:r3, result in r0:r1
void mt_hle_divdi3(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint64_t n = mt_arg_u64(&a), d = mt_arg_u64(&a);
    mt_ret_u64(cpu, sdiv64(n, d));
}

// unsigned long long __udivdi3(unsigned long long a, unsigned long long b)
void mt_hle_udivdi3(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint64_t n = mt_arg_u64(&a), d = mt_arg_u64(&a);
    mt_ret_u64(cpu, udiv64(n, d));
}

// float __floatdisf(long long a): correctly rounded, to nearest even, like compiler-rt
void mt_hle_floatdisf(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_ret_f32(cpu, static_cast<float>(mt_arg_i64(&a)));
}

} // extern "C"
