// The import table at run time (ADR 0019): the iOS armv7 argument marshalling of
// maplethief/hle/abi.h, linking HLE implementations by name, the default handler for
// unimplemented imports and its policies, data objects and binds. Synthetic imports only;
// tests/test_recomp_imports.py runs the same code under recompiled guest code.
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"

#include <doctest/doctest.h>

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_MSC_VER)
// setjmp in C++ (C4611): no object with a destructor lives across it here (see trapped()).
#pragma warning(disable : 4611)
#endif

namespace {

// A guest with 64 KiB of memory and sp at 0x8000.
struct Guest {
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x10000);
    mt_cpu cpu{};
    Guest() {
        cpu.mem = mem.data();
        cpu.mem_size = mem.size();
        cpu.r[MT_SP] = 0x8000u;
    }
    void put32(std::uint32_t addr, std::uint32_t v) { std::memcpy(&mem[addr], &v, 4); }
    std::uint32_t get32(std::uint32_t addr) const {
        std::uint32_t v;
        std::memcpy(&v, &mem[addr], 4);
        return v;
    }
};

std::uint64_t bits(double d) { return mt_f64_bits(d); }

// mt_trap: the stop policy ends in it. Records the trap and jumps back to the test.
std::jmp_buf *g_trap_jmp = nullptr;
std::uint32_t g_trap[3];

// Runs mt_import_unimplemented(cpu, id); returns true if it stopped through mt_trap. No C++
// object lives across the setjmp.
bool trapped(mt_cpu *cpu, std::uint32_t id) {
    std::jmp_buf jmp;
    g_trap_jmp = &jmp;
    if (setjmp(jmp) != 0) {
        g_trap_jmp = nullptr;
        return true;
    }
    mt_import_unimplemented(cpu, id);
    g_trap_jmp = nullptr;
    return false;
}

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

void hle_answer(mt_cpu *cpu, std::uint32_t) { cpu->r[0] = 42u; }

std::uint32_t g_init_id = 0, g_init_addr = 0;
void init_true(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) {
    g_init_id = id;
    g_init_addr = addr;
    mt_st32(cpu, addr, 0xCAFEF00Du, 0u);
}

const mt_import_desc k_descs[] = {
    {"_answer", "/usr/lib/libSystem.B.dylib", MT_IMPORT_FUNCTION, 0x2000u, 0u, 0u},
    {"_missing", "/System/Library/Frameworks/UIKit.framework/UIKit", MT_IMPORT_FUNCTION, 0x2004u,
     0u, 0u},
    {"_kCFBooleanTrue", "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
     MT_IMPORT_DATA, 0u, 4u, 4u},
    {"_CGRectZero", "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
     MT_IMPORT_DATA, 0u, 16u, 16u},
    {"_objc_msgSend_stret", "/usr/lib/libobjc.A.dylib", MT_IMPORT_FUNCTION | MT_IMPORT_STRET,
     0xFFFE0010u, 0u, 0u},
};
constexpr std::uint32_t k_count = sizeof k_descs / sizeof k_descs[0];
const mt_hle_entry k_hle[] = {
    {"_answer", hle_answer, nullptr},
    {"_kCFBooleanTrue", nullptr, init_true},
    {"_unused", hle_answer, nullptr},
};

mt_import_fn g_table[k_count];

void link(std::uint32_t policy) {
    mt_import_config cfg = mt_import_config_default();
    cfg.unimplemented = policy;
    cfg.log = capture;
    g_log.clear();
    CHECK(mt_imports_link(k_descs, k_count, g_table, k_hle, 3u, &cfg) == 2u);
}

struct Bump {
    std::uint32_t next;
};
std::uint32_t bump(void *user, std::uint32_t size, std::uint32_t align) {
    auto *b = static_cast<Bump *>(user);
    const std::uint32_t at = (b->next + align - 1u) & ~(align - 1u);
    b->next = at + size;
    return at;
}

std::uint32_t g_callback_r0 = 0;
void guest_callback(mt_cpu *MT_RESTRICT cpu) {
    g_callback_r0 = cpu->r[0];
    cpu->r[0] = cpu->r[0] + cpu->r[1];
    cpu->r[2] = 0xDEADu; // clobbers an argument register, as a callee may
}

} // namespace

extern "C" void mt_trap(mt_cpu *, std::uint32_t kind, std::uint32_t pc,
                        std::uint32_t detail) noexcept {
    g_trap[0] = kind;
    g_trap[1] = pc;
    g_trap[2] = detail;
    if (g_trap_jmp != nullptr) {
        std::longjmp(*g_trap_jmp, 1);
    }
    std::abort();
}

extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t target, std::uint32_t) noexcept {
    if (target != 0x3001u) {
        std::abort();
    }
    return guest_callback;
}

TEST_CASE("iOS arguments: 64-bit values take the next two words, split over r3 and the stack") {
    // CFRunLoopTimerCreate(allocator, fireDate, interval, flags, order, callout, context) as
    // the game calls it at 0x8ED8: fireDate in r1:r2, interval in r3:[sp].
    Guest g;
    const std::uint64_t fire = bits(1234.5), interval = bits(0.001);
    g.cpu.r[0] = 0x11u;
    g.cpu.r[1] = static_cast<std::uint32_t>(fire);
    g.cpu.r[2] = static_cast<std::uint32_t>(fire >> 32);
    g.cpu.r[3] = static_cast<std::uint32_t>(interval);
    g.put32(0x8000u, static_cast<std::uint32_t>(interval >> 32));
    g.put32(0x8004u, 0u);      // flags
    g.put32(0x8008u, 0u);      // order
    g.put32(0x800Cu, 0x8F95u); // callout
    g.put32(0x8010u, 0x7F00u); // context
    mt_args a = mt_args_begin(&g.cpu);
    CHECK(mt_arg_u32(&a) == 0x11u);
    CHECK(mt_arg_f64(&a) == 1234.5);
    CHECK(mt_arg_f64(&a) == 0.001);
    CHECK(mt_arg_u32(&a) == 0u);
    CHECK(mt_arg_i32(&a) == 0);
    CHECK(mt_arg_u32(&a) == 0x8F95u);
    CHECK(mt_arg_u32(&a) == 0x7F00u);
}

TEST_CASE("AAPCS arguments: 64-bit values in an even/odd pair or an 8-byte aligned slot") {
    Guest g;
    g.cpu.r[0] = 0x11u;
    g.cpu.r[2] = 0x89ABCDEFu; // r1 is skipped
    g.cpu.r[3] = 0x01234567u;
    g.put32(0x8000u, 7u); // a 32-bit argument after the pair
    g.put32(0x8008u, 0x22222222u);
    g.put32(0x800Cu, 0x33333333u); // the next 64-bit value: aligned to 0x8008
    mt_args a = mt_args_begin_aapcs(&g.cpu);
    CHECK(mt_arg_u32(&a) == 0x11u);
    CHECK(mt_arg_u64(&a) == 0x0123456789ABCDEFull);
    CHECK(mt_arg_u32(&a) == 7u);
    CHECK(mt_arg_u64(&a) == 0x3333333322222222ull);
}

TEST_CASE("struct arguments, struct results through r0, varargs and va_list") {
    Guest g;
    // CGRectInset(CGRect, dx, dy) at 0xD7EC0: r0 = result, the rect in r1-r3 and [sp].
    const float rect[4] = {1.0f, 2.0f, 30.0f, 40.0f};
    g.cpu.r[0] = 0x9000u;
    g.cpu.r[1] = mt_f32_bits(rect[0]);
    g.cpu.r[2] = mt_f32_bits(rect[1]);
    g.cpu.r[3] = mt_f32_bits(rect[2]);
    g.put32(0x8000u, mt_f32_bits(rect[3]));
    g.put32(0x8004u, mt_f32_bits(5.0f));
    g.put32(0x8008u, mt_f32_bits(5.0f));
    std::uint32_t result = 0;
    mt_args a = mt_args_begin_stret(&g.cpu, &result);
    float in[4];
    mt_arg_struct(&a, in, sizeof in);
    const float dx = mt_arg_f32(&a), dy = mt_arg_f32(&a);
    CHECK(result == 0x9000u);
    CHECK(std::memcmp(in, rect, sizeof rect) == 0);
    CHECK(dx == 5.0f);
    CHECK(dy == 5.0f);
    const float out[4] = {in[0] + dx, in[1] + dy, in[2] - 2 * dx, in[3] - 2 * dy};
    mt_ret_struct(&g.cpu, result, out, sizeof out);
    CHECK(mt_f32(g.get32(0x9008u)) == 20.0f);

    // A 3-byte structure comes back in r0.
    const std::uint8_t small[3] = {1, 2, 3};
    mt_ret_struct(&g.cpu, 0u, small, sizeof small);
    CHECK(g.cpu.r[0] == 0x030201u);

    // printf("%d %f", 7, 2.5): the double follows in r2:r3, unaligned rules as for fixed
    // arguments. vprintf(fmt, ap): the same words, read from memory.
    g.cpu.r[0] = 0x100u;
    g.cpu.r[1] = 7u;
    g.cpu.r[2] = static_cast<std::uint32_t>(bits(2.5));
    g.cpu.r[3] = static_cast<std::uint32_t>(bits(2.5) >> 32);
    a = mt_args_begin(&g.cpu);
    CHECK(mt_arg_u32(&a) == 0x100u);
    CHECK(mt_arg_i32(&a) == 7);
    CHECK(mt_arg_f64(&a) == 2.5);
    g.put32(0xA000u, 9u);
    g.put32(0xA004u, static_cast<std::uint32_t>(bits(-1.0)));
    g.put32(0xA008u, static_cast<std::uint32_t>(bits(-1.0) >> 32));
    a = mt_args_va_list(&g.cpu, 0xA000u);
    CHECK(mt_arg_u32(&a) == 9u);
    CHECK(mt_arg_f64(&a) == -1.0);

    mt_ret_f64(&g.cpu, 0.5);
    CHECK((static_cast<std::uint64_t>(g.cpu.r[1]) << 32 | g.cpu.r[0]) == bits(0.5));
    mt_ret_f32(&g.cpu, 1.0f);
    CHECK(g.cpu.r[0] == 0x3F800000u);
    mt_ret_u64(&g.cpu, 0x1122334455667788ull);
    CHECK(g.cpu.r[0] == 0x55667788u);
    CHECK(g.cpu.r[1] == 0x11223344u);
}

TEST_CASE("an HLE function calls a guest function back with mt_call_guest") {
    Guest g;
    g.cpu.r[MT_LR] = 0x1235u;
    const std::uint32_t args[2] = {40u, 2u};
    CHECK(mt_call_guest(&g.cpu, 0x3001u, args, 2u, 0x2000u) == 42u);
    CHECK(g_callback_r0 == 40u);
    CHECK(g.cpu.r[MT_LR] == 0x1235u);
}

TEST_CASE("linking fills every slot: the HLE function by name, else the default handler") {
    link(MT_UNIMPLEMENTED_STOP);
    CHECK(g_table[0] == hle_answer);
    CHECK(g_table[1] == mt_import_unimplemented);
    CHECK(g_table[2] == mt_import_unimplemented); // data: never called
    CHECK(g_table[3] == mt_import_unimplemented);
    CHECK(g_table[4] == mt_import_unimplemented);
    Guest g;
    g_table[0](&g.cpu, 0u);
    CHECK(g.cpu.r[0] == 42u);
}

TEST_CASE("an unimplemented import logs its name and caller, then stops by default") {
    link(MT_UNIMPLEMENTED_STOP);
    Guest g;
    g.cpu.r[MT_LR] = 0x0002A3C5u;
    CHECK(trapped(&g.cpu, 1u));
    CHECK(g_trap[0] == MT_TRAP_IMPORT);
    CHECK(g_trap[1] == 0x0002A3C5u);
    CHECK(g_trap[2] == 1u);
    REQUIRE(g_log.size() == 1u);
    CHECK(g_log[0] == "unimplemented import _missing (id 1, UIKit) called from 0x0002A3C5: "
                      "stopping");
    CHECK(mt_import_unimplemented_calls(1u) == 1u);
}

TEST_CASE("continue returns 0 and logs the first call; trace logs every call") {
    link(MT_UNIMPLEMENTED_CONTINUE);
    Guest g;
    g.cpu.r[0] = 5u;
    g.cpu.r[1] = 6u;
    g.cpu.r[MT_LR] = 0x1001u;
    CHECK_FALSE(trapped(&g.cpu, 1u));
    CHECK(g.cpu.r[0] == 0u);
    CHECK(g.cpu.r[1] == 0u);
    g.cpu.r[MT_LR] = 0x1101u;
    CHECK_FALSE(trapped(&g.cpu, 1u));
    CHECK_FALSE(trapped(&g.cpu, 4u));
    REQUIRE(g_log.size() == 2u);
    CHECK(g_log[0] == "unimplemented import _missing (id 1, UIKit) called from 0x00001001: "
                      "returning 0");
    CHECK(g_log[1] == "unimplemented import _objc_msgSend_stret (id 4, libobjc.A.dylib) called "
                      "from 0x00001101: returning 0 (struct result left unwritten)");
    CHECK(mt_import_unimplemented_calls(1u) == 2u);

    link(MT_UNIMPLEMENTED_TRACE);
    CHECK_FALSE(trapped(&g.cpu, 1u));
    CHECK_FALSE(trapped(&g.cpu, 1u));
    CHECK(g_log.size() == 2u);

    // A data import's slot is never a function: calling it stops whatever the policy.
    CHECK(trapped(&g.cpu, 2u));
    CHECK(g_log.back().find("_kCFBooleanTrue (id 2, CoreFoundation, data)") != std::string::npos);
    // So does an id beyond the table.
    CHECK(trapped(&g.cpu, 99u));
    CHECK(g_trap[2] == 99u);
}

TEST_CASE("the policy comes from MAPLETHIEF_UNIMPLEMENTED_IMPORT") {
    std::uint32_t p = 99u;
    CHECK(mt_unimplemented_policy_parse("continue", &p) == 0);
    CHECK(p == MT_UNIMPLEMENTED_CONTINUE);
    CHECK(mt_unimplemented_policy_parse("trace", &p) == 0);
    CHECK(p == MT_UNIMPLEMENTED_TRACE);
    CHECK(mt_unimplemented_policy_parse("stop", &p) == 0);
    CHECK(p == MT_UNIMPLEMENTED_STOP);
    CHECK(mt_unimplemented_policy_parse("Stop", &p) == -1);

    mt_import_config cfg = mt_import_config_default();
    CHECK(cfg.unimplemented == MT_UNIMPLEMENTED_STOP);
    cfg.log = capture;
    g_log.clear();
#if defined(_WIN32)
    _putenv_s(MT_UNIMPLEMENTED_IMPORT_ENV, "continue");
#else
    setenv(MT_UNIMPLEMENTED_IMPORT_ENV, "continue", 1);
#endif
    CHECK(mt_import_config_from_env(&cfg) == 0);
    CHECK(cfg.unimplemented == MT_UNIMPLEMENTED_CONTINUE);
#if defined(_WIN32)
    _putenv_s(MT_UNIMPLEMENTED_IMPORT_ENV, "sometimes");
#else
    setenv(MT_UNIMPLEMENTED_IMPORT_ENV, "sometimes", 1);
#endif
    CHECK(mt_import_config_from_env(&cfg) == -1);
    CHECK(cfg.unimplemented == MT_UNIMPLEMENTED_CONTINUE);
    REQUIRE(g_log.size() == 1u);
    CHECK(g_log[0].find("sometimes") != std::string::npos);
#if defined(_WIN32)
    _putenv_s(MT_UNIMPLEMENTED_IMPORT_ENV, "");
#else
    unsetenv(MT_UNIMPLEMENTED_IMPORT_ENV);
#endif
    cfg = mt_import_config_default();
    CHECK(mt_import_config_from_env(&cfg) == 0);
    CHECK(cfg.unimplemented == MT_UNIMPLEMENTED_STOP);
}

TEST_CASE("data imports get zeroed guest objects, init hooks run, binds are written") {
    link(MT_UNIMPLEMENTED_STOP);
    Guest g;
    std::memset(g.mem.data() + 0xC000u, 0xEE, 0x100u); // not yet zero
    Bump heap{0xC001u};
    std::uint32_t addrs[k_count] = {};
    CHECK(mt_imports_place(&g.cpu, k_descs, k_count, k_hle, 3u, bump, &heap, addrs) == 0);
    CHECK(addrs[0] == 0x2000u);
    CHECK(addrs[1] == 0x2004u);
    CHECK(addrs[2] == 0xC004u);
    CHECK(addrs[3] == 0xC010u); // 16-byte aligned
    CHECK(addrs[4] == 0xFFFE0010u);
    CHECK(g_init_id == 2u);
    CHECK(g_init_addr == 0xC004u);
    CHECK(g.get32(0xC004u) == 0xCAFEF00Du);
    for (std::uint32_t i = 0; i < 16u; i++) {
        CHECK(g.mem[0xC010u + i] == 0u);
    }

    const mt_import_bind binds[] = {
        {0x4000u, 3u, 0}, {0x4004u, 2u, 8}, {0x4008u, 1u, 0}, {0xFFFFu, 0u, 0}, {0x400Cu, 9u, 0}};
    CHECK(mt_imports_apply_binds(&g.cpu, binds, 5u, addrs, k_count) == -1);
    CHECK(g.get32(0x4000u) == 0xC010u);
    CHECK(g.get32(0x4004u) == 0xC00Cu);
    CHECK(g.get32(0x4008u) == 0x2004u);
    CHECK(g.get32(0x400Cu) == 0u);
    CHECK(mt_imports_apply_binds(&g.cpu, binds, 3u, addrs, k_count) == 0);

    // No room: the allocator fails.
    CHECK(mt_imports_place(&g.cpu, k_descs, k_count, k_hle, 3u, nullptr, nullptr, addrs) == -1);
}

TEST_CASE("the libgcc helpers follow compiler-rt") {
    std::uint32_t n = 0;
    const mt_hle_entry *hle = mt_hle_registry(&n);
    const mt_import_desc descs[] = {
        {"___divsi3", "libgcc_s", MT_IMPORT_FUNCTION, 0x10u, 0u, 0u},
        {"___modsi3", "libgcc_s", MT_IMPORT_FUNCTION, 0x14u, 0u, 0u},
        {"___udivsi3", "libgcc_s", MT_IMPORT_FUNCTION, 0x18u, 0u, 0u},
        {"___umodsi3", "libgcc_s", MT_IMPORT_FUNCTION, 0x1Cu, 0u, 0u},
        {"___divdi3", "libgcc_s", MT_IMPORT_FUNCTION, 0x20u, 0u, 0u},
        {"___udivdi3", "libgcc_s", MT_IMPORT_FUNCTION, 0x24u, 0u, 0u},
        {"___floatdisf", "libgcc_s", MT_IMPORT_FUNCTION, 0x28u, 0u, 0u},
    };
    mt_import_fn table[7];
    CHECK(mt_imports_link(descs, 7u, table, hle, n, nullptr) == 0u);
    Guest g;
    auto call2 = [&](int i, std::uint32_t a, std::uint32_t b) {
        g.cpu.r[0] = a;
        g.cpu.r[1] = b;
        table[i](&g.cpu, static_cast<std::uint32_t>(i));
        return g.cpu.r[0];
    };
    auto call4 = [&](int i, std::uint64_t a, std::uint64_t b) {
        g.cpu.r[0] = static_cast<std::uint32_t>(a);
        g.cpu.r[1] = static_cast<std::uint32_t>(a >> 32);
        g.cpu.r[2] = static_cast<std::uint32_t>(b);
        g.cpu.r[3] = static_cast<std::uint32_t>(b >> 32);
        table[i](&g.cpu, static_cast<std::uint32_t>(i));
        return static_cast<std::uint64_t>(g.cpu.r[1]) << 32 | g.cpu.r[0];
    };
    CHECK(call2(0, static_cast<std::uint32_t>(-7), 2u) == static_cast<std::uint32_t>(-3));
    CHECK(call2(0, 0x80000000u, 0xFFFFFFFFu) == 0x80000000u);
    CHECK(call2(0, 5u, 0u) == 0u);
    CHECK(call2(1, static_cast<std::uint32_t>(-7), 2u) == static_cast<std::uint32_t>(-1));
    CHECK(call2(1, 7u, 0u) == 7u);
    CHECK(call2(2, 0xFFFFFFFFu, 16u) == 0x0FFFFFFFu);
    CHECK(call2(3, 0xFFFFFFFFu, 16u) == 15u);
    CHECK(call4(4, static_cast<std::uint64_t>(-1000000000000ll), 1000ull) ==
          static_cast<std::uint64_t>(-1000000000ll));
    CHECK(call4(5, 0xFFFFFFFFFFFFFFFFull, 0x100000000ull) == 0xFFFFFFFFull);
    g.cpu.r[0] = 0x00000001u; // 2^53 + 1 rounds to 2^53 (nearest even)
    g.cpu.r[1] = 0x00200000u;
    table[6](&g.cpu, 6u);
    CHECK(g.cpu.r[0] == mt_f32_bits(9007199254740992.0f));
}
