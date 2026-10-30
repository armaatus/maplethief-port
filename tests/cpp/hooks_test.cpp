// Guest function hooks (#143, ADR 0069) with synthetic guest functions: the registry
// (maplethief/hooks.h) and MT_FN_HOOK, the generated code's way in (maplethief/recomp/calls.h),
// written here as the recompiler writes a hooked function. A replacement, a wrapper that calls
// the original with changed arguments and changes its result, a handler that passes, switching
// on and off between calls, the statistics, and the port's OpenPortalURLAndExit hook on a guest
// with a URL in an image segment. The real game's hooked call is runtime.hooks_portal_real
// (hooks_real_test.cpp).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/calls.h"

namespace {

using u32 = std::uint32_t;

constexpr u32 k_fn = MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT;

// The "original": r0 = r0 * 10 + r1, and it counts its calls; it returns to lr like any lifted
// function.
int g_original_calls = 0;
void original(mt_cpu *MT_RESTRICT cpu) {
    g_original_calls++;
    cpu->r[0] = cpu->r[0] * 10u + cpu->r[1];
}

// The hooked function as the recompiler writes it (recomp/lift/core.py).
void hooked(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(k_fn, original); }

int replace(mt_cpu *cpu, const mt_hook_call *call) {
    CHECK(call->fn == k_fn);
    CHECK(call->original == original);
    mt_ret_u32(cpu, 7u);
    return MT_HOOK_HANDLED;
}

// Doubles the first argument, calls the original, adds 1 to its result.
int wrap(mt_cpu *cpu, const mt_hook_call *call) {
    *static_cast<int *>(call->user) += 1;
    cpu->r[0] *= 2u;
    call->original(cpu);
    cpu->r[0] += 1u;
    return MT_HOOK_HANDLED;
}

int pass(mt_cpu *cpu, const mt_hook_call *) {
    cpu->r[1] = 5u; // seen by the original that runs after it
    return MT_HOOK_PASS;
}

u32 run(mt_cpu &cpu, u32 a, u32 b) {
    cpu.r[0] = a;
    cpu.r[1] = b;
    cpu.r[MT_LR] = 0x1235u;
    hooked(&cpu);
    CHECK(cpu.r[MT_LR] == 0x1235u);
    return cpu.r[0];
}

struct Fresh {
    Fresh() {
        mt_hook_reset();
        g_original_calls = 0;
    }
    ~Fresh() { mt_hook_reset(); }
};

} // namespace

TEST_CASE("hooks: with no handler the original runs and the call is counted") {
    Fresh fresh;
    mt_cpu cpu{};
    CHECK(run(cpu, 3, 4) == 34u);
    CHECK(g_original_calls == 1);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.entries == 1u);
    CHECK(st.passed == 1u);
    CHECK(st.handled == 0u);
    CHECK(mt_hook_enabled(k_fn) == 0);
}

TEST_CASE("hooks: only the hookable functions take a handler") {
    Fresh fresh;
    CHECK(mt_hook_hookable(k_fn) == 1);
    CHECK(mt_hook_hookable(k_fn & ~1u) == 0); // keyed by the function pointer, Thumb bit set
    CHECK(mt_hook_hookable(0x65B55u) == 0);
    CHECK(mt_hook_register(0x65B55u, replace, nullptr, 1) == 0);
    CHECK(mt_hook_register(k_fn, nullptr, nullptr, 1) == 0);
    CHECK(mt_hook_set_enabled(k_fn, 1) == 0); // no handler yet
    CHECK(mt_hook_enabled(k_fn) == 0);
    // An address the runtime does not know is never handled: the original runs.
    mt_cpu cpu{};
    CHECK(mt_hook_entry(&cpu, 0x65B55u, original) == 0);
}

TEST_CASE("hooks: a replacement does the call and the original does not run") {
    Fresh fresh;
    mt_cpu cpu{};
    REQUIRE(mt_hook_register(k_fn, replace, nullptr, 1) == 1);
    CHECK(mt_hook_enabled(k_fn) == 1);
    CHECK(run(cpu, 3, 4) == 7u);
    CHECK(g_original_calls == 0);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.entries == 1u);
    CHECK(st.handled == 1u);
    CHECK(st.passed == 0u);
}

TEST_CASE("hooks: a wrapper calls the original with changed arguments and changes its result") {
    Fresh fresh;
    mt_cpu cpu{};
    int calls = 0;
    REQUIRE(mt_hook_register(k_fn, wrap, &calls, 1) == 1);
    CHECK(run(cpu, 3, 4) == 65u); // (3 * 2) * 10 + 4 + 1
    CHECK(calls == 1);
    CHECK(g_original_calls == 1);
    CHECK(mt_hook_get_stats(k_fn).handled == 1u);
}

TEST_CASE("hooks: a handler that passes lets the original run after it") {
    Fresh fresh;
    mt_cpu cpu{};
    REQUIRE(mt_hook_register(k_fn, pass, nullptr, 1) == 1);
    CHECK(run(cpu, 3, 4) == 35u);
    CHECK(g_original_calls == 1);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.passed == 1u);
    CHECK(st.handled == 0u);
}

TEST_CASE("hooks: switched off and on at run time, between calls") {
    Fresh fresh;
    mt_cpu cpu{};
    REQUIRE(mt_hook_register(k_fn, replace, nullptr, 0) == 1);
    CHECK(mt_hook_enabled(k_fn) == 0);
    CHECK(run(cpu, 1, 2) == 12u); // off: the original
    CHECK(mt_hook_set_enabled(k_fn, 1) == 1);
    CHECK(run(cpu, 1, 2) == 7u);
    CHECK(mt_hook_set_enabled(k_fn, 0) == 1);
    CHECK(run(cpu, 1, 2) == 12u);
    // A second registration replaces the handler and keeps the numbers.
    int calls = 0;
    REQUIRE(mt_hook_register(k_fn, wrap, &calls, 1) == 1);
    CHECK(run(cpu, 1, 2) == 23u);
    mt_hook_unregister(k_fn);
    CHECK(mt_hook_enabled(k_fn) == 0);
    CHECK(run(cpu, 1, 2) == 12u);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.entries == 5u);
    CHECK(st.handled == 2u);
    CHECK(st.passed == 3u);
    CHECK(g_original_calls == 4);
}

TEST_CASE("hooks: OpenPortalURLAndExit returns instead of quitting when on") {
    Fresh fresh;
    // A guest with a __TEXT segment holding the URL, as the game's __cstring does.
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    const std::string url = "http://w.nexonmobile.com/CP/gift.php?GV=35";
    mt_segment seg{};
    std::strcpy(seg.name, "__TEXT");
    seg.vmaddr = 0x4000;
    seg.vmsize = 0x1000;
    seg.filesize = static_cast<u32>(url.size() + 1);
    seg.initprot = MT_PROT_READ | MT_PROT_EXECUTE;
    REQUIRE(mt_mem_map_segment(mem, &seg, reinterpret_cast<const std::uint8_t *>(url.c_str()),
                               url.size() + 1) == 0);
    const u32 regs[] = {0x5000, 0x4000, 0x11, 0x22, 0x44, 0x55, 0x66,  0x77,
                        0x88,   0x99,   0xAA, 0xBB, 0xCC, 0xDD, 0x1235};
    const auto call = [&] {
        for (u32 i = 0; i < 15; i++) {
            cpu.r[i] = regs[i];
        }
        g_original_calls = 0;
        hooked(&cpu);
    };
    mt_hooks_install_portal_no_exit(1);
    CHECK(mt_hook_enabled(k_fn) == 1);
    call();
    CHECK(g_original_calls == 0); // the stand-in for exit(1) never ran
    for (u32 i = 4; i < 15; i++) {
        CHECK(cpu.r[i] == regs[i]); // r4-r11, sp and lr as the caller left them
    }
    // Off: the original runs, as on the device.
    mt_hooks_install_portal_no_exit(0);
    CHECK(mt_hook_enabled(k_fn) == 0);
    call();
    CHECK(g_original_calls == 1);
    // A URL outside the image is not read (logged as "?").
    mt_hook_set_enabled(k_fn, 1);
    cpu.r[1] = 0x00800000u;
    g_original_calls = 0;
    hooked(&cpu);
    CHECK(g_original_calls == 0);
    mt_mem_destroy(mem);
}
