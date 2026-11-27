// The script VM on the host (#386, ADR 0226, maplethief/scriptvm.h): hand-made bytecode on a
// synthetic guest, every one of the 67 opcodes and the differences from Lua 2.4 checked against
// the original's documented behaviour (docs/formats/script-out.md, docs/re/script-vm.md).
//
// The guest's VM helpers are stand-ins served by runtime_test's mt_dispatch double
// (scriptvm_test_dispatch): each records its call and does the documented work on the synthetic
// stack, so a test sees which helper the loop called, with what, and in what state.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/scriptvm.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

// Guest addresses: the VM's own globals are the game's, the rest is laid out for the test.
constexpr u32 k_mem = 0x120000u;
constexpr u32 k_g_stack = 0x1178ECu, k_g_top = 0x1178F0u, k_g_limit = 0x1178F4u;
constexpr u32 k_g_table = 0x11AA08u;
constexpr u32 k_lua = 0x1000u;     // the Lua object (+0xC the TFunc list)
constexpr u32 k_tfunc = 0x2000u;   // the TFunc being run
constexpr u32 k_reader = 0x2100u;  // its DataReader
constexpr u32 k_buf = 0x3000u;     // the reader's buffer
constexpr u32 k_mark = 0x10u;      // where the function's code starts in it
constexpr u32 k_table = 0x4000u;   // the Table object
constexpr u32 k_stack = 0x10000u;  // the stack's slots
constexpr u32 k_stack2 = 0x20000u; // where growStack moves it
constexpr u32 k_sp = 0x100000u;    // the caller's sp
constexpr u32 k_frame = k_sp - 0x2BCu;
// What the stand-ins answer.
constexpr u32 k_const0 = 0x5000u;  // Table::constant n: a TaggedString at k_const0 + 0x20 n
constexpr u32 k_sym0 = 0x6000u;    // Table::symbol n: a Symbol at k_sym0 + 0x10 n
constexpr u32 k_fslot0 = 0x8000u;  // FunctionSlots::get n: k_fslot0 + 0x40 n
constexpr u32 k_new0 = 0x9000u;    // operator new[]: bump allocations from here
constexpr u32 k_text = 0xA000u;    // the constants' C strings
constexpr u32 k_created = 0xB000u; // createString's TaggedString

constexpr u32 NIL = 0xFFFFFFFFu, NUM = 0xFFFFFFFEu, STR = 0xFFFFFFFDu, ARR = 0xFFFFFFFCu,
              FUN = 0xFFFFFFFBu, CFUN = 0xFFFFFFFAu, LINE = 0xFFFFFFF7u;

struct Call {
    u32 fn;
    u32 r[4];
    u32 at_sp; // the word at sp (comparison's fifth argument)
    u32 depth; // the stack's depth when called
};

struct State {
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(k_mem);
    mt_cpu cpu{};
    std::vector<Call> calls;
    u32 next_new = k_new0;
    u32 native_result = 77;   // doCall's first result
    bool flip_symbol = false; // pushGlobal makes the symbol it read a number
    std::string sprintf_out = "AB";
};
State *g = nullptr;

u32 get(u32 a) {
    u32 v = 0;
    std::memcpy(&v, &g->mem[a], 4);
    return v;
}
void put(u32 a, u32 v) { std::memcpy(&g->mem[a], &v, 4); }
u32 top() { return get(k_g_top); }
u32 stack() { return get(k_g_stack); }
u32 depth() { return (top() - stack()) / 8u; }
u32 tag(u32 index) { return get(stack() + 8u * index); }
u32 val(u32 index) { return get(stack() + 8u * index + 4u); }
void slot(u32 index, u32 t, u32 v) {
    put(stack() + 8u * index, t);
    put(stack() + 8u * index + 4u, v);
}
void push(u32 t, u32 v) {
    slot(depth(), t, v);
    put(k_g_top, top() + 8u);
}
void record(mt_cpu *cpu, u32 fn) {
    Call c{fn, {cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3]}, get(cpu->r[MT_SP]), depth()};
    g->calls.push_back(c);
}
std::vector<Call> calls_to(u32 fn) {
    std::vector<Call> out;
    for (const Call &c : g->calls) {
        if (c.fn == fn) {
            out.push_back(c);
        }
    }
    return out;
}

// The stand-ins (docs/re/script-vm.md for what the guest's do).
void s_error(mt_cpu *cpu) { record(cpu, 0x99161u); }
void s_equal(mt_cpu *cpu) {
    record(cpu, 0x9916Du);
    cpu->r[0] = get(cpu->r[1]) == get(cpu->r[2]) && get(cpu->r[1] + 4) == get(cpu->r[2] + 4);
}
void s_push_indexed(mt_cpu *cpu) { record(cpu, 0x99235u); }
void s_store_indexed0(mt_cpu *cpu) { record(cpu, 0x9930Du); }
void s_link_function(mt_cpu *cpu) { // the TFunc onto L's list, marked 0
    record(cpu, 0x993B5u);
    put(cpu->r[1] + 4, get(cpu->r[0] + 0xC));
    put(cpu->r[0] + 0xC, cpu->r[1]);
    put(cpu->r[1] + 8, 0);
}
void s_to_number(mt_cpu *cpu) { // a number; a string becomes atol of its text
    record(cpu, 0x9A521u);
    const u32 o = cpu->r[1];
    if (get(o) == STR) {
        const char *text = reinterpret_cast<const char *>(&g->mem[get(get(o + 4) + 0xC)]);
        put(o + 4, static_cast<u32>(std::strtol(text, nullptr, 10))); // atol: no error either
        put(o, NUM);
    }
    cpu->r[0] = get(o) == NUM ? 1u : 0u;
}
void s_grow_stack(mt_cpu *cpu) { // doubles the stack and moves it
    record(cpu, 0x9A569u);
    const u32 used = top() - stack(), size = get(k_g_limit) - stack();
    std::memmove(&g->mem[k_stack2], &g->mem[stack()], size);
    put(k_g_stack, k_stack2);
    put(k_g_top, k_stack2 + used);
    put(k_g_limit, k_stack2 + 2u * size);
}
void s_comparison(mt_cpu *cpu) { // the two numbers popped into one tag
    record(cpu, 0x9B6B5u);
    const auto l = static_cast<i32>(val(depth() - 2u)), r = static_cast<i32>(val(depth() - 1u));
    put(k_g_top, top() - 8u);
    slot(depth() - 1u, l < r ? cpu->r[1] : l == r ? cpu->r[2] : cpu->r[3], 0);
}
void s_push_global(mt_cpu *cpu) { // the symbol's tag and value
    record(cpu, 0x9B821u);
    const u32 s = k_sym0 + 0x10u * (cpu->r[1] & 0xFFFFu);
    push(get(s + 8), get(s + 12));
    if (g->flip_symbol) {
        put(s + 8, NUM);
    }
}
void s_set_line(mt_cpu *cpu) { // Lua 2.4's open_stack(n): the top n slots up by one
    record(cpu, 0x9B8ADu);
    const u32 n = cpu->r[1];
    for (u32 i = 0; i < n; i++) {
        const u32 to = depth() - i;
        slot(to, tag(to - 1u), val(to - 1u));
    }
    put(k_g_top, top() + 8u);
}
void s_adjust_top(mt_cpu *cpu) { // top = stack + index, filled with nil
    record(cpu, 0x9B979u);
    while (depth() < cpu->r[1]) {
        push(NIL, 0);
    }
    put(k_g_top, stack() + 8u * cpu->r[1]);
}
void s_do_call(mt_cpu *cpu) { // the function and its arguments replaced by its results
    record(cpu, 0x9BA11u);
    put(k_g_top, stack() + 8u * (cpu->r[1] - 1u));
    for (u32 i = 0; i < cpu->r[2] && cpu->r[2] != 255u; i++) {
        push(NUM, g->native_result + i);
    }
}
void s_constant(mt_cpu *cpu) {
    record(cpu, 0x9E165u);
    cpu->r[0] = k_const0 + 0x20u * cpu->r[1];
}
void s_symbol(mt_cpu *cpu) {
    record(cpu, 0x9E18Du);
    cpu->r[0] = k_sym0 + 0x10u * cpu->r[1];
}
void s_create_string(mt_cpu *cpu) {
    record(cpu, 0x9E361u);
    cpu->r[0] = k_created;
}
void s_function_slot(mt_cpu *cpu) {
    record(cpu, 0x9EC5Du);
    cpu->r[0] = k_fslot0 + 0x40u * cpu->r[0];
}
void s_sprintf(mt_cpu *cpu) {
    record(cpu, 0xA095u);
    std::memcpy(&g->mem[cpu->r[0]], g->sprintf_out.c_str(), g->sprintf_out.size() + 1);
}
void s_new_array(mt_cpu *cpu) {
    record(cpu, 0xF0E30u);
    cpu->r[0] = g->next_new;
    g->next_new += 0x100u;
}

// The guest's Lua::execute behind its hook, as the generated code has it.
int g_original_runs = 0;
void execute_original(mt_cpu *MT_RESTRICT cpu) {
    g_original_runs++;
    cpu->r[0] = 0x1234u;
}
void hooked_execute(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_LUA_EXECUTE, execute_original);
}

// A fresh guest: an empty stack of `slots` slots, the function's code, the reader at its start.
struct Guest {
    State state;
    explicit Guest(const std::vector<std::uint8_t> &code, u32 slots = 64) {
        g = &state;
        mt_hook_reset();
        mt_scriptvm_reset_stats();
        state.cpu.mem = state.mem.data();
        state.cpu.mem_size = state.mem.size();
        state.cpu.r[MT_SP] = k_sp;
        state.cpu.r[MT_LR] = 0x4321u;
        for (u32 i = 4; i < 12; i++) {
            state.cpu.r[i] = 0x100u + i;
        }
        put(k_g_stack, k_stack);
        put(k_g_top, k_stack);
        put(k_g_limit, k_stack + 8u * slots);
        put(k_g_table, k_table);
        put(k_tfunc + 0xC, static_cast<u32>(code.size()));
        put(k_tfunc + 0x1C, 0);
        put(k_tfunc + 0x20, k_reader);
        put(k_reader + 4, k_buf);
        put(k_reader + 8, k_mark);
        put(k_reader + 0xC, k_mark);
        std::memcpy(&state.mem[k_buf + k_mark], code.data(), code.size());
        // Constants 0..3: "10", "-3", "x", "y"; symbols n: tag nil.
        const char *texts[] = {"10", "-3", "x", "y"};
        for (u32 i = 0; i < 4; i++) {
            std::memcpy(&state.mem[k_text + 0x10u * i], texts[i], std::strlen(texts[i]) + 1);
            put(k_const0 + 0x20u * i + 0xC, k_text + 0x10u * i);
        }
    }
    ~Guest() {
        mt_hook_reset();
        g = nullptr;
    }
    // Runs the loop at `base` and checks that it kept sp, r4-r11 and lr.
    u32 run(u32 base = 0) {
        const u32 result = mt_scriptvm_execute(&state.cpu, k_lua, k_tfunc, base);
        CHECK(state.cpu.r[MT_SP] == k_sp);
        CHECK(state.cpu.r[MT_LR] == 0x4321u);
        for (u32 i = 4; i < 12; i++) {
            CHECK(state.cpu.r[i] == 0x100u + i);
        }
        return result;
    }
    u32 pc() const { return get(k_tfunc + 0x1C); }
    u32 pos() const { return get(k_reader + 8); }
};

} // namespace

mt_guest_fn scriptvm_test_dispatch(u32 target) {
    switch (target) {
    case 0x99161u:
        return s_error;
    case 0x9916Du:
        return s_equal;
    case 0x99235u:
        return s_push_indexed;
    case 0x9930Du:
        return s_store_indexed0;
    case 0x993B5u:
        return s_link_function;
    case 0x9A521u:
        return s_to_number;
    case 0x9A569u:
        return s_grow_stack;
    case 0x9B6B5u:
        return s_comparison;
    case 0x9B821u:
        return s_push_global;
    case 0x9B8ADu:
        return s_set_line;
    case 0x9B979u:
        return s_adjust_top;
    case 0x9BA11u:
        return s_do_call;
    case 0x9E165u:
        return s_constant;
    case 0x9E18Du:
        return s_symbol;
    case 0x9E361u:
        return s_create_string;
    case 0x9EC5Du:
        return s_function_slot;
    case 0xA095u:
        return s_sprintf;
    case 0xF0E30u:
        return s_new_array;
    default:
        return nullptr;
    }
}

TEST_CASE("scriptvm: the pushes, PUSHNIL's tag alone, and RETCODE0 returning base") {
    // PUSHNIL PUSH0 PUSH1 PUSH2 PUSHBYTE 200 PUSHWORD 0x1234 PUSHSTRING 2 PUSHFUNCTION 3
    // RETCODE0
    Guest vm(
        {0x00, 0x01, 0x02, 0x03, 0x04, 200, 0x05, 0x34, 0x12, 0x07, 2, 0, 0x08, 3, 0, 0, 0, 0x40});
    slot(0, 0, 0x5555u); // PUSHNIL leaves the value word as it was
    CHECK(vm.run(0) == 0u);
    REQUIRE(depth() == 8u);
    CHECK((tag(0) == NIL && val(0) == 0x5555u));
    CHECK((tag(1) == NUM && val(1) == 0u));
    CHECK((tag(2) == NUM && val(2) == 1u));
    CHECK((tag(3) == NUM && val(3) == 2u));
    CHECK((tag(4) == NUM && val(4) == 200u));
    CHECK((tag(5) == NUM && val(5) == 0x1234u));
    CHECK((tag(6) == STR && val(6) == k_const0 + 0x40u));
    CHECK((tag(7) == FUN && val(7) == k_fslot0 + 0xC0u));
    // PUSHSTRING asked the Table at 0x11AA08; PUSHFUNCTION linked the slot's TFunc onto L+0xC.
    REQUIRE(calls_to(0x9E165u).size() == 1u);
    CHECK(calls_to(0x9E165u)[0].r[0] == k_table);
    CHECK(get(k_lua + 0xC) == k_fslot0 + 0xC0u);
    CHECK(calls_to(0x993B5u).size() == 1u);
    // RETCODE0 rewound the reader: pc 0, position at the mark.
    CHECK(vm.pc() == 0u);
    CHECK(vm.pos() == k_mark);
    const mt_scriptvm_stats s = mt_scriptvm_get_stats();
    CHECK(s.calls == 1u);
    CHECK(s.steps == 9u);
    CHECK(s.checks == 0u);
}

TEST_CASE("scriptvm: a push that reaches the limit grows the stack, and the loop follows it") {
    Guest vm({0x02, 0x02, 0x02, 0x03, 0x40}, 2);
    vm.run(0);
    CHECK(calls_to(0x9A569u).size() == 2u); // at 2 and at 4 slots
    CHECK(stack() == k_stack2);
    REQUIRE(depth() == 4u);
    CHECK((val(0) == 1u && val(2) == 1u && val(3) == 2u));
}

TEST_CASE("scriptvm: PUSHFLOAT and PUSHSELF skip no operand (differences 1 and 2)") {
    // PUSHFLOAT's 4 bytes run as PUSH1 PUSH2 PUSH0 PUSH0; PUSHSELF's 2 as PUSH2 PUSH1.
    Guest vm({0x06, 0x02, 0x03, 0x01, 0x01, 0x16, 0x03, 0x02, 0x40});
    vm.run(0);
    REQUIRE(depth() == 6u);
    const u32 want[] = {1, 2, 0, 0, 2, 1};
    for (u32 i = 0; i < 6; i++) {
        CHECK((tag(i) == NUM && val(i) == want[i]));
    }
}

TEST_CASE("scriptvm: locals, and STORELOCALn storing to the last operand - 23 (difference 3)") {
    // base 2. PUSHLOCAL0..9 and PUSHLOCAL 3 copy locals; STORELOCAL 1 stores one.
    std::vector<std::uint8_t> code;
    for (std::uint8_t op = 0x09; op <= 0x12; op++) {
        code.push_back(op);
    }
    // PUSHLOCAL 3, PUSHBYTE 0x18, STORELOCAL 1 (pops 0x18 into local 1), PUSH2, STORELOCAL5
    // (aux is 1 from STORELOCAL's operand: local 1 - 23, below the stack), PUSHBYTE 0x19, PUSH0,
    // STORELOCAL0 (aux 0x19: local 2), POP, RETCODE0.
    for (const int b :
         {0x13, 3, 0x04, 0x18, 0x21, 1, 0x03, 0x1C, 0x04, 0x19, 0x01, 0x17, 0x3E, 0x40}) {
        code.push_back(static_cast<std::uint8_t>(b));
    }
    Guest vm(code);
    slot(0, NUM, 0xA0);
    slot(1, NUM, 0xA1);
    for (u32 i = 0; i < 12; i++) {
        slot(2 + i, NUM, 100 + i);
    }
    put(k_g_top, k_stack + 8u * 14u);
    const u32 below = k_stack + 8u * (2u + 1u - 23u);
    vm.run(2);
    for (u32 i = 0; i < 10; i++) {
        CHECK(val(14 + i) == 100u + i); // PUSHLOCAL0..9
    }
    CHECK(val(24) == 103u);                             // PUSHLOCAL 3
    CHECK(val(3) == 0x18u);                             // STORELOCAL 1: local 1 is base + 1
    CHECK((get(below) == NUM && get(below + 4) == 2u)); // STORELOCAL5 with aux 1
    CHECK(val(4) == 0u);   // STORELOCAL0 with aux 0x19: local 2 = base + 2, not local 0
    CHECK(val(2) == 100u); // local 0 untouched
    CHECK(mt_scriptvm_get_stats().checks == 1u); // the store below the stack was logged
}

TEST_CASE("scriptvm: each of STORELOCAL0..9 stores to the last operand - 23") {
    for (std::uint8_t op = 0x17; op <= 0x20; op++) {
        // PUSHBYTE 0x1A (aux 0x1A: local 3), PUSH2 (no operand), STORELOCALn, RETCODE0; base 1.
        Guest vm({0x04, 0x1A, 0x03, op, 0x40});
        put(k_g_top, k_stack + 8u * 12u);
        vm.run(1);
        CHECK((tag(4) == NUM && val(4) == 2u));
        CHECK(depth() == 13u);
        CHECK(mt_scriptvm_get_stats().checks == 0u);
    }
}

TEST_CASE("scriptvm: a builtin goes to doCall like any native (difference 7 lies beyond it)") {
    // assert (index 0) has no dispatcher case in Lua::callNative: the loop hands it on unchanged.
    Guest vm({0x3F, 1, 0, 0x40});
    push(CFUN, 0);
    push(NUM, 1);
    vm.run(0);
    REQUIRE(calls_to(0x9BA11u).size() == 1u);
    CHECK(calls_to(0x9BA11u)[0].r[1] == 1u);
    CHECK(mt_scriptvm_get_stats().natives == 1u);
}

TEST_CASE("scriptvm: PUSHGLOBAL sign-extends its index, STOREGLOBAL writes the symbol's slot") {
    // PUSHGLOBAL 0xFFFF, PUSHBYTE 9, STOREGLOBAL 5, PUSHGLOBAL 5, RETCODE0
    Guest vm({0x14, 0xFF, 0xFF, 0x04, 9, 0x22, 5, 0, 0x14, 5, 0, 0x40});
    vm.run(0);
    const std::vector<Call> pg = calls_to(0x9B821u);
    REQUIRE(pg.size() == 2u);
    CHECK(pg[0].r[1] == 0xFFFFFFFFu);
    CHECK(pg[1].r[1] == 5u);
    CHECK(calls_to(0x9E18Du)[0].r[0] == k_table);
    CHECK((get(k_sym0 + 0x50u + 8u) == NUM && get(k_sym0 + 0x50u + 12u) == 9u));
    REQUIRE(depth() == 2u);
    CHECK((tag(1) == NUM && val(1) == 9u));
}

TEST_CASE("scriptvm: arrays are flat, indexed by the key's value word (difference 4)") {
    // CREATEARRAY 3; STORELIST0 2 (t[2] = b, t[1] = a); STORELIST 1 1 (t[41] = c);
    // STORERECORD 1 "x" (t[address of constant 2] = d); STOREINDEXED0; PUSHINDEXED.
    Guest vm({0x2A, 3,    0, 0x02, 0x03, 0x25, 2, 0x04, 7,    0x26, 1,
              1,    0x04, 8, 0x27, 1,    2,    0, 0x23, 0x15, 0x40});
    // The cells: pointers to slots at 0xC000 + 8 i for the indexes the stores use.
    const u32 cells = k_new0;
    const auto cell_for = [&](u32 index) { return 0xC000u + 8u * (index % 0x100u); };
    for (u32 index : {1u, 2u, 41u, k_const0 + 0x40u}) {
        put(cells + 4u * index, cell_for(index));
    }
    vm.run(0);
    REQUIRE(calls_to(0xF0E30u).size() == 1u);
    CHECK(calls_to(0xF0E30u)[0].r[0] == 16u); // (3 + 1) pointers
    REQUIRE(depth() == 1u);
    CHECK((tag(0) == ARR && val(0) == cells));
    CHECK((get(cell_for(1)) == NUM && get(cell_for(1) + 4) == 1u));
    CHECK((get(cell_for(2)) == NUM && get(cell_for(2) + 4) == 2u));
    CHECK((get(cell_for(41)) == NUM && get(cell_for(41) + 4) == 7u));
    CHECK((get(cell_for(k_const0 + 0x40u)) == NUM && get(cell_for(k_const0 + 0x40u) + 4) == 8u));
    // The keys were written in the slot above the values: STORERECORD's string is the last.
    CHECK((tag(2) == STR && val(2) == k_const0 + 0x40u));
    CHECK(calls_to(0x9930Du).size() == 1u); // STOREINDEXED0 and PUSHINDEXED are the guest's
    CHECK(calls_to(0x99235u).size() == 1u);
}

TEST_CASE("scriptvm: STOREINDEXED stores through an array, and leaves two slots on a non-array") {
    // t i a1 v with t an array: t[i] = v, one pop.
    Guest vm({0x24, 1, 0x40});
    put(k_new0 + 4u * 6u, 0xC000u);
    push(ARR, k_new0);
    push(NUM, 6);
    push(NUM, 99); // a1
    push(STR, 0x777);
    vm.run(0);
    CHECK((get(0xC000u) == STR && get(0xC004u) == 0x777u));
    CHECK(depth() == 3u);
    CHECK(calls_to(0x99161u).empty());
}

TEST_CASE("scriptvm: STOREINDEXED on a non-array builds the fallback frame and calls error 6 "
          "(difference 5)") {
    Guest vm({0x24, 0, 0x40}, 4);
    push(NUM, 1); // t, not an array
    push(NUM, 2); // i
    push(NUM, 3); // v
    vm.run(0);
    CHECK(calls_to(0x9A569u).size() == 1u); // top + 2 reached the 4-slot limit
    REQUIRE(depth() == 5u);
    const u32 want[] = {1, 2, 1, 2, 3}; // t i | t i v: two slots more than before
    for (u32 i = 0; i < 5; i++) {
        CHECK(val(i) == want[i]);
    }
    REQUIRE(calls_to(0x99161u).size() == 1u);
    CHECK(calls_to(0x99161u)[0].r[1] == 6u);
}

TEST_CASE("scriptvm: ADJUST0 and ADJUST set the top through Lua::adjustTop") {
    Guest vm({0x28, 0x29, 3, 0x40});
    vm.run(2);
    const std::vector<Call> c = calls_to(0x9B979u);
    REQUIRE(c.size() == 2u);
    CHECK(c[0].r[1] == 2u);
    CHECK(c[1].r[1] == 5u);
}

TEST_CASE("scriptvm: EQOP gives number 1 or nil, both with value word 1; NOTOP the same way") {
    // 2 == 2, 1 == 2, not nil, not 1
    Guest vm({0x03, 0x03, 0x2B, 0x02, 0x03, 0x2B, 0x00, 0x37, 0x02, 0x37, 0x40});
    vm.run(0);
    REQUIRE(depth() == 4u);
    CHECK((tag(0) == NUM && val(0) == 1u));
    CHECK((tag(1) == NIL && val(1) == 1u));
    CHECK((tag(2) == NUM && val(2) == 1u));
    CHECK((tag(3) == NIL && val(3) == 1u));
    const std::vector<Call> c = calls_to(0x9916Du);
    REQUIRE(c.size() == 2u);
    CHECK((c[0].r[1] == k_stack && c[0].r[2] == k_stack + 8u));
}

TEST_CASE("scriptvm: LTOP LEOP GTOP GEOP pass Lua::comparison their tags and event") {
    Guest vm({0x02, 0x03, 0x2C, 0x02, 0x03, 0x2D, 0x02, 0x03, 0x2E, 0x02, 0x03, 0x2F, 0x40});
    vm.run(0);
    const std::vector<Call> c = calls_to(0x9B6B5u);
    REQUIRE(c.size() == 4u);
    const u32 want[4][4] = {{NUM, NIL, NIL, 0xE5EC0u},
                            {NUM, NUM, NIL, 0xE5EC3u},
                            {NIL, NIL, NUM, 0xE5EC6u},
                            {NIL, NUM, NUM, 0xE5EC9u}};
    for (u32 i = 0; i < 4; i++) {
        CHECK(c[i].r[0] == k_lua);
        CHECK(c[i].r[1] == want[i][0]);
        CHECK(c[i].r[2] == want[i][1]);
        CHECK(c[i].r[3] == want[i][2]);
        CHECK(c[i].at_sp == want[i][3]);
    }
    CHECK(depth() == 4u); // 1 < 2 is true for each: lt NUM, le NUM, gt NIL, ge NIL
    CHECK((tag(0) == NUM && tag(1) == NUM && tag(2) == NIL && tag(3) == NIL));
}

TEST_CASE("scriptvm: integer arithmetic, string operands made numbers, POWOP a no-op "
          "(difference 1)") {
    // 200 + 7, 200 - 7, 200 * 7, 200 / 7, -7 / 2 (-3), 5 / 0 (0, as __divsi3), "10" + 2, -(5),
    // 2 ^ 3 (both stay)
    Guest vm({0x04, 200,  0x04, 7,    0x30, 0x04, 200,  0x04, 7,    0x31, 0x04, 200,  0x04, 7,
              0x32, 0x04, 200,  0x04, 7,    0x33, 0x04, 7,    0x36, 0x03, 0x33, 0x04, 5,    0x01,
              0x33, 0x07, 0,    0,    0x03, 0x30, 0x04, 5,    0x36, 0x03, 0x04, 3,    0x34, 0x40});
    vm.run(0);
    REQUIRE(depth() == 10u);
    const i32 want[] = {207, 193, 1400, 28, -3, 0, 12, -5, 2, 3};
    for (u32 i = 0; i < 10; i++) {
        CHECK((tag(i) == NUM && static_cast<i32>(val(i)) == want[i]));
    }
}

TEST_CASE("scriptvm: a failed number check skips the operation (difference 5)") {
    // nil + 1: the right operand is checked first and passes, the left fails; nothing changes.
    // 1 + nil: the right fails and the left is never checked.
    Guest vm({0x00, 0x02, 0x30, 0x02, 0x00, 0x30, 0x40});
    vm.run(0);
    CHECK(depth() == 4u);
    const std::vector<Call> c = calls_to(0x9A521u);
    REQUIRE(c.size() == 3u);
    CHECK(c[0].r[1] == k_stack + 8u);  // right of the first ADDOP
    CHECK(c[1].r[1] == k_stack);       // its left
    CHECK(c[2].r[1] == k_stack + 24u); // right of the second, and no left
}

TEST_CASE("scriptvm: CONCOP checks its operands as numbers, then formats their value words") {
    // "10" .. "-3": the number check makes both strings numbers (atol), so sprintf's two %s get
    // the words at 10 + 0xC and at -3 + 0xC (9), not the strings' texts. No shipped file has it.
    Guest vm({0x07, 0, 0, 0x07, 1, 0, 0x35, 0x40});
    put(10u + 0xCu, 0x1111u);
    put(9u, 0x2222u);
    vm.run(0);
    const std::vector<Call> s = calls_to(0xA095u);
    REQUIRE(s.size() == 1u);
    CHECK(s[0].r[0] == k_frame + 0x17Cu); // the original's 100-byte buffer in its frame
    CHECK(s[0].r[1] == 0xE27E5u);
    CHECK(s[0].r[2] == 0x1111u);
    CHECK(s[0].r[3] == 0x2222u);
    REQUIRE(calls_to(0x9E361u).size() == 1u);
    CHECK(calls_to(0x9E361u)[0].r[0] == 0u);
    CHECK(calls_to(0x9E361u)[0].r[1] == k_frame + 0x17Cu);
    REQUIRE(depth() == 1u);
    CHECK((tag(0) == NUM && val(0) == k_created)); // the tag stays the number's
}

TEST_CASE("scriptvm: jumps move the TFunc's count and the reader together") {
    // 0: JMP +4 -> 7; 3: PUSH1; 4: RETCODE0; 5: pad; 7: PUSH0; 8: UPJMP 8 -> 3
    Guest vm({0x3A, 4, 0, 0x02, 0x40, 0xEE, 0xEE, 0x01, 0x3B, 8, 0});
    vm.run(0);
    REQUIRE(depth() == 2u);
    CHECK((val(0) == 0u && val(1) == 1u));
    CHECK(vm.pc() == 0u);
}

TEST_CASE("scriptvm: ONTJMP and ONFJMP keep the value; IFFJMP and IFFUPJMP pop it") {
    // PUSH1; ONTJMP +1 (taken: skips a PUSH2); ONFJMP +1 (not taken); PUSHNIL; ONFJMP +1
    // (taken); PUSHNIL; IFFJMP +1 (pops, taken); PUSH1; IFFJMP +1 (pops, not taken); PUSH1;
    // POP; RETCODE0.
    Guest vm({0x02, 0x38, 1, 0, 0x03, 0x39, 1,    0, 0x00, 0x39, 1,    0,   0x03,
              0x00, 0x3C, 1, 0, 0x03, 0x02, 0x3C, 1, 0,    0x02, 0x3E, 0x40});
    vm.run(0);
    REQUIRE(depth() == 2u);
    CHECK((tag(0) == NUM && val(0) == 1u));
    CHECK(tag(1) == NIL);
}

TEST_CASE("scriptvm: IFFUPJMP loops back while the value is nil") {
    // 0: PUSHGLOBAL 1 (nil the first time, a number after); 3: IFFUPJMP 6 -> 0 while nil;
    // 6: RETCODE0
    Guest vm({0x14, 1, 0, 0x3D, 6, 0, 0x40});
    put(k_sym0 + 0x10u + 8u, NIL);
    g->flip_symbol = true;
    vm.run(0);
    CHECK(calls_to(0x9B821u).size() == 2u);
    CHECK(depth() == 0u);
}

TEST_CASE("scriptvm: CALLFUNC hands doCall the first argument's index and nresults") {
    // x f a b, CALLFUNC 2 1: doCall (L, 2, 1).
    Guest vm({0x3F, 2, 1, 0x40});
    push(NUM, 5);
    push(FUN, k_fslot0);
    push(NUM, 1);
    push(NUM, 2);
    vm.run(0);
    const std::vector<Call> c = calls_to(0x9BA11u);
    REQUIRE(c.size() == 1u);
    CHECK((c[0].r[0] == k_lua && c[0].r[1] == 2u && c[0].r[2] == 1u));
    CHECK(mt_scriptvm_get_stats().natives == 0u);
    REQUIRE(depth() == 2u);
    CHECK(val(1) == 77u);
}

TEST_CASE("scriptvm: CALLFUNC counts a native call") {
    Guest vm({0x3F, 1, 0, 0x40});
    push(CFUN, 20);
    push(NUM, 3);
    vm.run(0);
    REQUIRE(calls_to(0x9BA11u).size() == 1u);
    CHECK(calls_to(0x9BA11u)[0].r[1] == 1u);
    CHECK(mt_scriptvm_get_stats().natives == 1u);
    CHECK(depth() == 0u); // the stand-in dropped f and its argument, 0 results
}

TEST_CASE("scriptvm: RETCODE reads its operand after the rewind: the function's first byte") {
    Guest vm({0x02, 0x02, 0x41, 9});
    CHECK(vm.run(1) == 1u + 0x02u); // base + code[0], not base + 9
    CHECK(vm.pc() == 1u);
    CHECK(vm.pos() == k_mark + 1u);
}

TEST_CASE("scriptvm: SETLINE opens a line slot below base once, then updates its line") {
    // base 1 with one local; SETLINE 7, PUSH2, SETLINE 8, RETCODE0
    Guest vm({0x42, 7, 0, 0x03, 0x42, 8, 0, 0x40});
    slot(0, FUN, 0x8000u);
    slot(1, NUM, 42);
    put(k_g_top, k_stack + 16u);
    CHECK(vm.run(1) == 2u); // base moved up by one
    REQUIRE(calls_to(0x9B8ADu).size() == 1u);
    CHECK(calls_to(0x9B8ADu)[0].r[1] == 1u); // depth 2 - base 1
    CHECK((tag(1) == LINE && val(1) == 8u));
    CHECK((tag(2) == NUM && val(2) == 42u));
    CHECK((tag(3) == NUM && val(3) == 2u));
}

TEST_CASE("scriptvm: a byte above 0x42 is a 1-byte no-op; a read past the code is logged") {
    Guest vm({0x43, 0xFF, 0x02, 0x40});
    vm.run(0);
    CHECK(depth() == 1u);
    CHECK(mt_scriptvm_get_stats().checks == 0u);
    // PUSHBYTE's operand and the RETCODE0 after it lie past the function's 2 bytes: both reads
    // are logged, and done as the original does them.
    Guest past({0x02, 0x04});
    g->mem[k_buf + k_mark + 2] = 5;
    g->mem[k_buf + k_mark + 3] = 0x40;
    past.run(0);
    REQUIRE(depth() == 2u);
    CHECK(val(1) == 5u);
    CHECK(mt_scriptvm_get_stats().checks == 2u);
}

TEST_CASE("scriptvm: the hook runs the host loop when on, and the original when off") {
    Guest vm({0x02, 0x40});
    mt_hooks_install_native_script_vm(1);
    vm.state.cpu.r[0] = k_lua;
    vm.state.cpu.r[1] = k_tfunc;
    vm.state.cpu.r[2] = 0;
    g_original_runs = 0;
    hooked_execute(&vm.state.cpu);
    CHECK(vm.state.cpu.r[0] == 0u);
    CHECK(g_original_runs == 0);
    CHECK(depth() == 1u);
    mt_hooks_install_native_script_vm(0);
    hooked_execute(&vm.state.cpu);
    CHECK(vm.state.cpu.r[0] == 0x1234u);
    CHECK(g_original_runs == 1);
}
