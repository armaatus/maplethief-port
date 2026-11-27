// The script VM's opcode loop on the host (#386, ADR 0226); see maplethief/scriptvm.h.
//
// What each opcode does is the recompiled Lua::execute 0x9BC11's behaviour, described in
// docs/formats/script-out.md (the opcode table) and docs/re/script-vm.md (the differences from
// Lua 2.4). The loop keeps the original's order of guest writes where a helper it calls could
// see them, and calls the same guest helpers with the same arguments.
#include "maplethief/scriptvm.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

// The VM's globals (docs/re/script-vm.md → Objects).
constexpr u32 k_stack = 0x1178ECu;        // g_luaStack: the first 8-byte {tag, value} slot
constexpr u32 k_top = 0x1178F0u;          // g_luaTop: the first free slot
constexpr u32 k_limit = 0x1178F4u;        // g_luaStackLimit: growStack when top reaches it
constexpr u32 k_table = 0x11AA08u;        // the Table the constant and symbol lookups take
constexpr u32 k_native_names = 0x117950u; // g_scriptNativeNames: 65 char *

// The guest helpers the original calls (Thumb bit set), with their arguments.
constexpr u32 k_error = 0x99161u;          // Lua::error (L, code): empty
constexpr u32 k_equal = 0x9916Du;          // Lua::equalObjects (L, a, b): 0 or 1
constexpr u32 k_push_indexed = 0x99235u;   // Lua::pushIndexed (L)
constexpr u32 k_store_indexed0 = 0x9930Du; // Lua::storeIndexed0 (L)
constexpr u32 k_link_function = 0x993B5u;  // Lua::linkFunction (L, tfunc)
constexpr u32 k_to_number = 0x9A521u;      // Lua::toNumberCheck (L, obj): 0 or 1
constexpr u32 k_grow_stack = 0x9A569u;     // Lua::growStack (L)
constexpr u32 k_comparison = 0x9B6B5u;     // Lua::comparison (L, lt, eq, gt, event at sp)
constexpr u32 k_push_global = 0x9B821u;    // Lua::pushGlobal (L, index sign-extended from 16)
constexpr u32 k_set_line = 0x9B8ADu;       // Lua::setLine (L, n)
constexpr u32 k_adjust_top = 0x9B979u;     // Lua::adjustTop (L, index)
constexpr u32 k_do_call = 0x9BA11u;        // Lua::doCall (L, base, nresults)
constexpr u32 k_constant = 0x9E165u;       // Table::constant (table, n)
constexpr u32 k_symbol = 0x9E18Du;         // Table::symbol (table, n)
constexpr u32 k_create_string = 0x9E361u;  // Table::createString (mark, text)
constexpr u32 k_function_slot = 0x9EC5Du;  // FunctionSlots::get (slot)
constexpr u32 k_sprintf = 0xA095u;         // sprintf (buffer, format, ...)
constexpr u32 k_new_array = 0xF0E30u;      // operator new[]'s import stub
// The comparison event names and CONCOP's "%s%s" in the image.
constexpr u32 k_event_lt = 0xE5EC0u, k_event_le = 0xE5EC3u, k_event_gt = 0xE5EC6u,
              k_event_ge = 0xE5EC9u;
constexpr u32 k_concat_format = 0xE27E5u;

// TFunc and its DataReader.
constexpr u32 k_tfunc_size = 0xCu;    // code size in bytes
constexpr u32 k_tfunc_pc = 0x1Cu;     // operand bytes read so far, kept beside the reader
constexpr u32 k_tfunc_reader = 0x20u; // DataReader *
constexpr u32 k_reader_buf = 0x4u;
constexpr u32 k_reader_pos = 0x8u;
constexpr u32 k_reader_mark = 0xCu; // where the code starts

// The original's frame (push of 4 registers and 0x2AC bytes): the scratch the loop keeps below
// the caller's sp, CONCOP's 100-byte buffer at the original's place in it.
constexpr u32 k_frame = 0x2BCu;
constexpr u32 k_concat_buffer = 0x17Cu;

// Tags (Lua 2.4's).
constexpr u32 k_nil = 0xFFFFFFFFu;       // -1
constexpr u32 k_number = 0xFFFFFFFEu;    // -2
constexpr u32 k_string = 0xFFFFFFFDu;    // -3
constexpr u32 k_array = 0xFFFFFFFCu;     // -4
constexpr u32 k_function = 0xFFFFFFFBu;  // -5
constexpr u32 k_cfunction = 0xFFFFFFFAu; // -6
constexpr u32 k_line = 0xFFFFFFF7u;      // -9

// Lua 2.4's opcode names (docs/formats/script-out.md → Opcodes), for the trace.
constexpr const char *k_names[] = {
    "PUSHNIL",      "PUSH0",       "PUSH1",        "PUSH2",       "PUSHBYTE",    "PUSHWORD",
    "PUSHFLOAT",    "PUSHSTRING",  "PUSHFUNCTION", "PUSHLOCAL0",  "PUSHLOCAL1",  "PUSHLOCAL2",
    "PUSHLOCAL3",   "PUSHLOCAL4",  "PUSHLOCAL5",   "PUSHLOCAL6",  "PUSHLOCAL7",  "PUSHLOCAL8",
    "PUSHLOCAL9",   "PUSHLOCAL",   "PUSHGLOBAL",   "PUSHINDEXED", "PUSHSELF",    "STORELOCAL0",
    "STORELOCAL1",  "STORELOCAL2", "STORELOCAL3",  "STORELOCAL4", "STORELOCAL5", "STORELOCAL6",
    "STORELOCAL7",  "STORELOCAL8", "STORELOCAL9",  "STORELOCAL",  "STOREGLOBAL", "STOREINDEXED0",
    "STOREINDEXED", "STORELIST0",  "STORELIST",    "STORERECORD", "ADJUST0",     "ADJUST",
    "CREATEARRAY",  "EQOP",        "LTOP",         "LEOP",        "GTOP",        "GEOP",
    "ADDOP",        "SUBOP",       "MULTOP",       "DIVOP",       "POWOP",       "CONCOP",
    "MINUSOP",      "NOTOP",       "ONTJMP",       "ONFJMP",      "JMP",         "UPJMP",
    "IFFJMP",       "IFFUPJMP",    "POP",          "CALLFUNC",    "RETCODE0",    "RETCODE",
    "SETLINE",
};
static_assert(sizeof k_names / sizeof k_names[0] == 0x43, "67 opcodes");

std::atomic<u64> g_calls{0}, g_steps{0}, g_natives{0}, g_checks{0};
std::atomic<mt_scriptvm_step_fn> g_step_fn{nullptr};
std::atomic<void *> g_step_user{nullptr};

// The guest's signed division as the HLE's __divsi3 does it (0 for a zero divisor).
u32 sdiv(u32 a, u32 b) {
    const u32 ma = (a >> 31) != 0u ? 0u - a : a;
    const u32 mb = (b >> 31) != 0u ? 0u - b : b;
    const u32 q = mb == 0u ? 0u : ma / mb;
    return ((a ^ b) >> 31) != 0u ? 0u - q : q;
}

class Vm {
  public:
    Vm(mt_cpu *cpu, u32 lua, u32 tfunc, u32 base)
        : cpu_(cpu), lua_(lua), tfunc_(tfunc), base_(base), frame_(cpu->r[MT_SP] - k_frame),
          trace_(mt_trace_enabled("scriptvm") != 0) {}

    u32 run();

  private:
    mt_cpu *cpu_;
    u32 lua_, tfunc_, base_;
    u32 frame_;
    bool trace_;
    u32 aux_ = 0; // the last operand kept by the original's case (STORELOCALn reads it)
    u32 op_pc_ = 0;

    // Guest memory, checked.
    bool reach(u32 addr, u32 size) const {
        return cpu_->mem != nullptr && static_cast<u64>(addr) + size <= cpu_->mem_size;
    }
    u32 rd(u32 addr) {
        u32 v = 0;
        if (!reach(addr, 4)) {
            check("read outside guest memory, 0 instead", addr);
            return 0;
        }
        std::memcpy(&v, cpu_->mem + addr, 4);
        return v;
    }
    u32 rd8(u32 addr) {
        if (!reach(addr, 1)) {
            check("read outside guest memory, 0 instead", addr);
            return 0;
        }
        return cpu_->mem[addr];
    }
    void wr(u32 addr, u32 v) {
        if (!reach(addr, 4)) {
            check("write outside guest memory, skipped", addr);
            return;
        }
        std::memcpy(cpu_->mem + addr, &v, 4);
    }
    void copy(u32 dst, u32 src) { // one 8-byte slot
        const u32 tag = rd(src), value = rd(src + 4);
        wr(dst, tag);
        wr(dst + 4, value);
    }
    // A problem the original would not notice: logged and counted, then the original's way.
    void check(const char *what, u32 value) {
        g_checks.fetch_add(1, std::memory_order_relaxed);
        mt_tracef("scriptvm", "check: tfunc 0x%08X pc %u: %s (0x%08X)",
                  static_cast<unsigned>(tfunc_), static_cast<unsigned>(op_pc_), what,
                  static_cast<unsigned>(value));
    }
    u32 peek8(u32 addr) const { return reach(addr, 1) ? cpu_->mem[addr] : 0u; } // for the trace

    // A guest helper, with the loop's frame as its sp.
    u32 call(u32 fn, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0, u32 argc = 4) {
        const u32 args[4] = {a0, a1, a2, a3};
        cpu_->r[MT_SP] = frame_;
        return mt_call_guest(cpu_, fn, args, argc, MT_HOOK_FN_LUA_EXECUTE);
    }

    // The stack.
    u32 top() { return rd(k_top); }
    u32 stack() { return rd(k_stack); }
    void set_top(u32 t) { wr(k_top, t); }
    u32 slot(u32 index) { // the slot of a stack index, checked against the stack's extent
        const u32 s = stack();
        if (index >= (rd(k_limit) - s) / 8u) {
            check("stack index outside the stack", index);
        }
        return s + 8u * index;
    }
    u32 depth() { return sdiv(top() - stack(), 8u); }
    void grow_if_full(u32 t) {
        if (t >= rd(k_limit)) {
            call(k_grow_stack, lua_);
        }
    }
    void push(u32 tag, u32 value) {
        const u32 t = top();
        wr(t, tag);
        wr(t + 4, value);
        set_top(t + 8);
        grow_if_full(t + 8);
    }
    void pop_into(u32 dst) {
        const u32 t = top() - 8u;
        set_top(t);
        copy(dst, t);
    }

    // The operands, read as the original's TFunc::fetchU8/U16/U32 do: the TFunc's count and the
    // reader's position both move.
    u32 fetch(u32 bytes) {
        const u32 pc = rd(tfunc_ + k_tfunc_pc) + bytes;
        wr(tfunc_ + k_tfunc_pc, pc);
        if (pc > rd(tfunc_ + k_tfunc_size)) {
            check("read past the end of the function's code, at byte", pc);
        }
        const u32 reader = rd(tfunc_ + k_tfunc_reader);
        const u32 at = rd(reader + k_reader_buf) + rd(reader + k_reader_pos);
        u32 v = 0;
        for (u32 i = 0; i < bytes; i++) {
            v |= rd8(at + i) << (8u * i);
        }
        wr(reader + k_reader_pos, rd(reader + k_reader_pos) + bytes);
        return v;
    }
    u32 fetch_aux(u32 bytes) {
        aux_ = fetch(bytes);
        return aux_;
    }
    void jump_forward(u32 n) {
        const u32 reader = rd(tfunc_ + k_tfunc_reader);
        wr(tfunc_ + k_tfunc_pc, rd(tfunc_ + k_tfunc_pc) + n);
        wr(reader + k_reader_pos, rd(reader + k_reader_pos) + n);
    }
    void jump_back(u32 n) {
        const u32 reader = rd(tfunc_ + k_tfunc_reader);
        const u32 pc = rd(tfunc_ + k_tfunc_pc) - n;
        wr(tfunc_ + k_tfunc_pc, pc);
        wr(reader + k_reader_pos, rd(reader + k_reader_mark) + pc);
    }
    void rewind() {
        const u32 reader = rd(tfunc_ + k_tfunc_reader);
        wr(tfunc_ + k_tfunc_pc, 0);
        wr(reader + k_reader_pos, rd(reader + k_reader_mark));
    }

    void arith(u32 op);
    void concat();
    void store_indexed(u32 n);
    void store_list(u32 offset, u32 n);
    void store_record(u32 n);
    void call_function(u32 nargs, u32 nresults);
    void set_line(u32 line);
    void trace_step(u32 op);
};

void Vm::trace_step(u32 op) {
    if (!trace_) {
        return;
    }
    // The operand bytes, peeked without moving the reader.
    const u32 reader = rd(tfunc_ + k_tfunc_reader);
    const u32 at = rd(reader + k_reader_buf) + rd(reader + k_reader_pos);
    mt_tracef("scriptvm", "tfunc 0x%08X pc %u: %s [%02X %02X %02X %02X] base %u depth %u",
              static_cast<unsigned>(tfunc_), static_cast<unsigned>(op_pc_),
              op < 0x43u ? k_names[op] : "(none: skipped)", static_cast<unsigned>(peek8(at)),
              static_cast<unsigned>(peek8(at + 1)), static_cast<unsigned>(peek8(at + 2)),
              static_cast<unsigned>(peek8(at + 3)), static_cast<unsigned>(base_),
              static_cast<unsigned>(depth()));
}

// ADDOP..DIVOP: both operands made numbers (the right one first; the left is not tried when
// the right fails), then the integer operation into the left one. A failed check skips the
// operation (Lua::error's fallback is gone): both operands stay.
void Vm::arith(u32 op) {
    const u32 t = top();
    const u32 l = t - 16u, r = t - 8u;
    if (call(k_to_number, lua_, r) == 0u || call(k_to_number, lua_, l) == 0u) {
        return;
    }
    const u32 a = rd(l + 4), b = rd(r + 4);
    u32 v = 0;
    switch (op) {
    case 0x30:
        v = a + b;
        break;
    case 0x31:
        v = a - b;
        break;
    case 0x32:
        v = a * b;
        break;
    default:
        v = sdiv(a, b);
        break;
    }
    wr(l + 4, v);
    set_top(top() - 8u);
}

// CONCOP: the same number checks as arithmetic (so two strings become numbers), then
// sprintf("%s%s") of the two value words read as TaggedString pointers (the text at +0xC) into
// the frame's 100-byte buffer, interned by createString into the left slot's value word.
void Vm::concat() {
    const u32 t = top();
    const u32 l = t - 16u, r = t - 8u;
    if (call(k_to_number, lua_, r) == 0u || call(k_to_number, lua_, l) == 0u) {
        return;
    }
    const u32 a = rd(rd(l + 4) + 0xCu), b = rd(rd(r + 4) + 0xCu);
    const u32 buffer = frame_ + k_concat_buffer;
    call(k_sprintf, buffer, k_concat_format, a, b);
    wr(l + 4, call(k_create_string, 0, buffer, 0, 0, 2));
    set_top(top() - 8u);
}

// STOREINDEXED n: t i a_n..a_1 v. An array: t[i] = v through the array's slot pointer, pop v.
// Not an array: Lua 2.4's settable fallback frame (v t i v, two slots up) is built and
// Lua::error(L, 6) stands where the fallback call was, so the stack keeps the two slots.
void Vm::store_indexed(u32 n) {
    u32 t = top();
    const u32 tbl = t - 8u * (n + 3u), idx = t - 8u * (n + 2u);
    if (rd(tbl) == k_array) {
        const u32 cell = rd(rd(tbl + 4) + 4u * rd(idx + 4));
        copy(cell, t - 8u);
        set_top(top() - 8u);
        return;
    }
    if (t + 16u >= rd(k_limit)) {
        call(k_grow_stack, lua_);
    }
    t = top();
    copy(t + 8u, t - 8u);
    copy(t, t - 8u * (n + 2u));
    copy(t - 8u, t - 8u * (n + 3u));
    set_top(top() + 16u);
    call(k_error, lua_, 6);
}

// STORELIST0 / STORELIST m n: t v_1..v_n. For i = n..1 the slot above the values becomes the
// number i + offset, and the array's cell at that index takes the value below it, popped.
void Vm::store_list(u32 offset, u32 n) {
    const u32 tbl = top() - 8u * n - 8u;
    for (; n != 0u; n--) {
        const u32 t = top();
        wr(t, k_number);
        wr(t + 4, n + offset);
        const u32 cell = rd(rd(tbl + 4) + 4u * rd(t + 4));
        copy(cell, t - 8u);
        set_top(top() - 8u);
    }
}

// STORERECORD n s_1..s_n: as STORELIST, keyed by the string constant's address (flat arrays).
void Vm::store_record(u32 n) {
    const u32 tbl = top() - 8u * n - 8u;
    for (; n != 0u; n--) {
        fetch_aux(2);
        const u32 t = top();
        wr(t, k_string);
        wr(t + 4, call(k_constant, rd(k_table), aux_, 0, 0, 2));
        const u32 cell = rd(rd(tbl + 4) + 4u * rd(t + 4));
        copy(cell, t - 8u);
        set_top(top() - 8u);
    }
}

// CALLFUNC nargs nresults: Lua::doCall (L, index of the first argument, nresults).
void Vm::call_function(u32 nargs, u32 nresults) {
    const u32 first = depth() - nargs;
    const u32 fn = stack() + 8u * (first - 1u);
    if (rd(fn) == k_cfunction) {
        g_natives.fetch_add(1, std::memory_order_relaxed);
        if (trace_) {
            const u32 index = rd(fn + 4);
            const u32 name = index < 65u ? rd(k_native_names + 4u * index) : 0u;
            char text[40] = "?";
            for (u32 i = 0; name != 0u && i + 1u < sizeof text; i++) {
                text[i] = static_cast<char>(rd8(name + i));
                text[i + 1u] = '\0';
                if (text[i] == '\0') {
                    break;
                }
            }
            char args[160] = "";
            std::size_t used = 0;
            for (u32 i = 0; i < nargs && used < sizeof args; i++) {
                const u32 a = fn + 8u * (i + 1u);
                const int n =
                    std::snprintf(args + used, sizeof args - used, "%s%d:%d", i == 0u ? "" : ", ",
                                  static_cast<int>(rd(a)), static_cast<int>(rd(a + 4)));
                used += n > 0 ? static_cast<std::size_t>(n) : 0u;
            }
            mt_tracef("scriptvm", "native %u %s (%s) -> %u results", static_cast<unsigned>(index),
                      text, args, static_cast<unsigned>(nresults));
        }
    }
    call(k_do_call, lua_, first, nresults, 0, 3);
}

// SETLINE w: the slot below base is made a LUA_T_LINE slot once (Lua::setLine opens a slot and
// base moves up by one), then holds the line.
void Vm::set_line(u32 line) {
    if (rd(stack() + 8u * base_ - 8u) != k_line) {
        call(k_set_line, lua_, depth() - base_, 0, 0, 2);
        base_ += 1u;
        wr(stack() + 8u * base_ - 8u, k_line);
    }
    wr(stack() + 8u * base_ - 4u, line);
}

u32 Vm::run() {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    for (;;) {
        op_pc_ = rd(tfunc_ + k_tfunc_pc);
        const u32 op = fetch(1);
        g_steps.fetch_add(1, std::memory_order_relaxed);
        if (const mt_scriptvm_step_fn observe = g_step_fn.load(std::memory_order_acquire)) {
            observe(g_step_user.load(std::memory_order_relaxed), tfunc_, op_pc_, op);
        }
        trace_step(op);
        switch (op) {
        case 0x00: { // PUSHNIL: the tag only
            const u32 t = top();
            wr(t, k_nil);
            set_top(t + 8u);
            grow_if_full(t + 8u);
            break;
        }
        case 0x01:
        case 0x02:
        case 0x03:
            push(k_number, op - 1u);
            break; // PUSH0..2
        case 0x04:
            push(k_number, fetch_aux(1));
            break; // PUSHBYTE
        case 0x05:
            push(k_number, fetch_aux(2));
            break; // PUSHWORD
        case 0x06:
            break;   // PUSHFLOAT: nothing, its 4 operand bytes run as opcodes
        case 0x07: { // PUSHSTRING
            fetch_aux(2);
            const u32 t = top();
            wr(t, k_string);
            wr(t + 4, call(k_constant, rd(k_table), aux_, 0, 0, 2));
            set_top(top() + 8u);
            grow_if_full(top());
            break;
        }
        case 0x08: { // PUSHFUNCTION: the slot's TFunc, linked onto L's list each time
            const u32 tf = call(k_function_slot, fetch_aux(4), 0, 0, 0, 1);
            call(k_link_function, lua_, tf, 0, 0, 2);
            push(k_function, tf);
            break;
        }
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
        case 0x10:
        case 0x11:
        case 0x12: { // PUSHLOCAL0..9
            const u32 from = slot(base_ + op - 0x09u);
            const u32 t = top();
            copy(t, from);
            set_top(top() + 8u);
            grow_if_full(top());
            break;
        }
        case 0x13: { // PUSHLOCAL
            const u32 from = slot(base_ + fetch_aux(1));
            const u32 t = top();
            copy(t, from);
            set_top(top() + 8u);
            grow_if_full(top());
            break;
        }
        case 0x14: // PUSHGLOBAL
            call(k_push_global, lua_,
                 static_cast<u32>(static_cast<i32>(static_cast<std::int16_t>(fetch_aux(2)))), 0, 0,
                 2);
            break;
        case 0x15:
            call(k_push_indexed, lua_, 0, 0, 0, 1);
            break; // PUSHINDEXED
        case 0x16:
            break; // PUSHSELF: nothing, its 2 operand bytes run as opcodes
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x20: // STORELOCAL0..9: the local is the last operand - 23, not n
            pop_into(slot(base_ + aux_ - 0x17u));
            break;
        case 0x21:
            pop_into(slot(base_ + fetch_aux(1)));
            break;   // STORELOCAL
        case 0x22: { // STOREGLOBAL
            const u32 symbol = call(k_symbol, rd(k_table), fetch_aux(2), 0, 0, 2);
            pop_into(symbol + 8u);
            break;
        }
        case 0x23:
            call(k_store_indexed0, lua_, 0, 0, 0, 1);
            break; // STOREINDEXED0
        case 0x24:
            store_indexed(fetch(1));
            break; // STOREINDEXED
        case 0x25:
            store_list(0, fetch(1));
            break;   // STORELIST0
        case 0x26: { // STORELIST
            const u32 m = fetch(1);
            store_list(m * 40u, fetch(1));
            break;
        }
        case 0x27:
            store_record(fetch(1));
            break; // STORERECORD
        case 0x28:
            call(k_adjust_top, lua_, base_, 0, 0, 2);
            break; // ADJUST0
        case 0x29:
            call(k_adjust_top, lua_, base_ + fetch(1), 0, 0, 2);
            break;   // ADJUST
        case 0x2A: { // CREATEARRAY: new TObject *[w + 1], uninitialised
            const u32 n = fetch_aux(2);
            const u32 t = top();
            wr(t + 4, call(k_new_array, (n + 1u) * 4u, 0, 0, 0, 1));
            wr(top(), k_array);
            set_top(top() + 8u);
            grow_if_full(top());
            break;
        }
        case 0x2B: { // EQOP: number 1 or nil (whose value word is also 1)
            const u32 t = top();
            const u32 eq = call(k_equal, lua_, t - 16u, t - 8u, 0, 3);
            set_top(top() - 8u);
            wr(top() - 8u, eq != 0u ? k_number : k_nil);
            wr(top() - 4u, 1);
            break;
        }
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F: { // LTOP LEOP GTOP GEOP
            // (tag if less, if equal, if greater, event name)
            static constexpr u32 k_cmp[4][4] = {{k_number, k_nil, k_nil, k_event_lt},
                                                {k_number, k_number, k_nil, k_event_le},
                                                {k_nil, k_nil, k_number, k_event_gt},
                                                {k_nil, k_number, k_number, k_event_ge}};
            const u32 *c = k_cmp[op - 0x2Cu];
            wr(frame_, c[3]); // the fifth argument, at sp
            call(k_comparison, lua_, c[0], c[1], c[2]);
            break;
        }
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
            arith(op);
            break; // ADDOP..DIVOP
        case 0x34:
            break; // POWOP: nothing, both operands stay
        case 0x35:
            concat();
            break;   // CONCOP
        case 0x36: { // MINUSOP
            const u32 x = top() - 8u;
            if (call(k_to_number, lua_, x, 0, 0, 2) != 0u) {
                wr(top() - 4u, 0u - rd(top() - 4u));
            }
            break;
        }
        case 0x37: { // NOTOP: number 1 for nil, else nil (value word 1)
            const u32 x = top() - 8u;
            wr(x, rd(x) == k_nil ? k_number : k_nil);
            wr(top() - 4u, 1);
            break;
        }
        case 0x38:
        case 0x39: { // ONTJMP ONFJMP: jump if not nil / if nil, no pop
            fetch_aux(2);
            if ((rd(top() - 8u) == k_nil) == (op == 0x39u)) {
                jump_forward(aux_);
            }
            break;
        }
        case 0x3A:
            jump_forward(fetch_aux(2));
            break; // JMP
        case 0x3B:
            jump_back(fetch_aux(2));
            break; // UPJMP
        case 0x3C:
        case 0x3D: { // IFFJMP IFFUPJMP: pop, jump if nil
            fetch_aux(2);
            set_top(top() - 8u);
            if (rd(top()) == k_nil) {
                if (op == 0x3Cu) {
                    jump_forward(aux_);
                } else {
                    jump_back(aux_);
                }
            }
            break;
        }
        case 0x3E:
            set_top(top() - 8u);
            break;   // POP
        case 0x3F: { // CALLFUNC
            const u32 nargs = fetch(1);
            call_function(nargs, fetch(1));
            break;
        }
        case 0x40:
        case 0x41: { // RETCODE0 RETCODE: the reader is rewound before RETCODE's
                     // operand is read, so it reads the function's first byte
            rewind();
            const u32 n = op == 0x40u ? 0u : fetch(1);
            return base_ + n;
        }
        case 0x42:
            set_line(fetch_aux(2));
            break; // SETLINE
        default:
            break; // above 0x42: a 1-byte no-op
        }
    }
}

int execute_hook(mt_cpu *cpu, const mt_hook_call *call) {
    (void)call;
    cpu->r[0] = mt_scriptvm_execute(cpu, cpu->r[0], cpu->r[1], cpu->r[2]);
    return MT_HOOK_HANDLED;
}

} // namespace

extern "C" std::uint32_t mt_scriptvm_execute(mt_cpu *cpu, std::uint32_t lua, std::uint32_t tfunc,
                                             std::uint32_t base) noexcept {
    const u32 sp = cpu->r[MT_SP];
    u32 saved[8];
    std::memcpy(saved, &cpu->r[4], sizeof saved); // r4-r11, which guest callees keep anyway
    Vm vm(cpu, lua, tfunc, base);
    const u32 result = vm.run();
    std::memcpy(&cpu->r[4], saved, sizeof saved);
    cpu->r[MT_SP] = sp;
    return result;
}

extern "C" void mt_hooks_install_native_script_vm(int enabled) noexcept {
    mt_hook_register(MT_HOOK_FN_LUA_EXECUTE, execute_hook, nullptr, enabled);
}

extern "C" void mt_scriptvm_set_step_observer(mt_scriptvm_step_fn fn, void *user) noexcept {
    g_step_user.store(user, std::memory_order_relaxed);
    g_step_fn.store(fn, std::memory_order_release);
}

extern "C" mt_scriptvm_stats mt_scriptvm_get_stats(void) noexcept {
    mt_scriptvm_stats s{};
    s.calls = g_calls.load(std::memory_order_relaxed);
    s.steps = g_steps.load(std::memory_order_relaxed);
    s.natives = g_natives.load(std::memory_order_relaxed);
    s.checks = g_checks.load(std::memory_order_relaxed);
    return s;
}

extern "C" void mt_scriptvm_reset_stats(void) noexcept {
    g_calls.store(0, std::memory_order_relaxed);
    g_steps.store(0, std::memory_order_relaxed);
    g_natives.store(0, std::memory_order_relaxed);
    g_checks.store(0, std::memory_order_relaxed);
}
