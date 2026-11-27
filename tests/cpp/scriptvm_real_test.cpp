// Every shipped script through both script VMs (#431, ADR 0153's second proof, ADR 0226): each
// function of the 307 .out files of the bundle runs once in the recompiled Lua::execute 0x9BC11
// and once in the host VM (mt_scriptvm_execute), from the same guest memory, and the two runs
// must be equal step for step.
//
// The recompiled game boots from work/ as MapleThief does with --fixed-step 1 (the virtual
// clock) from a fresh data directory, to the main menu. In one turn of the run loop's turn hook
// the test then makes its own VM, as CStateGame::LoadGame 0x86C1D makes the game's: operator new
// of 0x18 bytes and Lua::Lua 0x9D5C5 (the owner 0: no native runs). For each file, in path order:
//
//   1. the file's bytes go into a guest buffer of their own (operator new; the load frees it,
//      so the next one gets its address) and Lua::loadBuffer 0x9D0A9 resets the VM and
//      undumps it. Main is the TFunc the game's four callers read after it (the global behind
//      the pointer at 0xF13BC); the other functions are the FunctionSlots table's (the global
//      behind 0xF13A4), as many as FunctionSlots::add 0x9ED09 counted.
//   2. Guest memory is kept: the image's writable segments, the heap up to its committed end and
//      the guest stack from 64 KiB below sp to its top.
//   3. For main, and for every slot function both right after the load (as the NPC-state pass
//      runs slot 0) and after main has run (as a callback runs), under each native answer set:
//      memory back as kept, Lua::callFunction 0x9CD65 with the host VM's hook off, the run
//      recorded; memory back, the same call with the hook on, recorded; the records compared.
//
// A record is the interleaved sequence of
//   - opcodes: the recompiled loop's are seen at TFunc::fetchU8 0x9EE95 when it is called from
//     the opcode fetch at 0x9BC2A (a hook that only looks and passes); the host loop's through
//     mt_scriptvm_set_step_observer. Each is (TFunc, pc, opcode).
//   - native calls: Lua::callNative 0x9B37D is hooked by a stub that does what the original's
//     frame code around its switch does (the argument count above base, the result index after
//     them) and records the native's index and its argument slots, then pushes one result for
//     a native that returns one (docs/re/script-vm.md → Natives, the Returns column): a number
//     from the run's answer set, or for getQuestTitle a string slot. So both VMs see the same
//     answers and no quest manager or hero is touched.
// and, after the call, its result and every byte of the kept memory except the stack below the
// call's sp (the frames: the original's scratch and the host's differ there by design, ADR
// 0226). The heap's numbers must not move in a run (no run allocates; the test says so if one
// does, as restoring memory alone would not undo it).
//
// Answer sets (16): every answer 0, 1, 2, -1 or 3, and eleven seeded mixes drawn from -1..5, 10,
// 20, 30, 50, 70, 100, 200 and 1000, so the branches on quest states, levels, job advancements
// and dialogue answers are taken both ways.
//
// Printed: files, functions, runs, opcodes and native calls compared, the distinct (TFunc, pc)
// reached. MT_SCRIPTVM_ONLY=<text> runs only the files whose path has it (for finding a
// difference; the totals' check then fails). Local only: it needs work/gen (python -m recomp all), the slice and the extracted
// bundle, and exits 77 (skipped) without them. Output (the data directory) goes to the test
// output directory (build/, never committed).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#if defined(MT_HAVE_GENERATED_CODE)

#include "clock.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/scriptvm.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Guest facts (docs/re/script-vm.md, docs/formats/script-out.md).
constexpr u32 k_canvas = 0x11A950;        // SkyCanvas::GetInstance's instance; +0x0C the state
constexpr u32 k_on_key_pressed = 0x9462D; // CStateMainMenu::OnKeyPressed, vtable slot 6
constexpr u32 k_operator_new = 0xF0E34;   // operator new's import stub (ARM)
constexpr u32 k_lua_ctor = 0x9D5C5;       // Lua::Lua (this, owner)
constexpr u32 k_load_buffer = 0x9D0A9;    // Lua::loadBuffer (L, buffer, size)
constexpr u32 k_call_function = 0x9CD65;  // Lua::callFunction (L, tfunc)
constexpr u32 k_grow_stack = 0x9A569;     // Lua::growStack (L)
constexpr u32 k_main_ptr = 0xF13BC;       // -> the main TFunc * the undump left
constexpr u32 k_slots_ptr = 0xF13A4;      // -> FunctionSlots' table (30 TFunc *)
// FunctionSlots::add 0x9ED09 reads its count and table through two pc-relative literals:
// `ldr r2, [pc, #0x34]; add r2, pc` at 0x9ED18 and `ldr r0, [pc, #0x40]; add r0, pc` at 0x9ED12.
constexpr u32 k_slot_count_lit = 0x9ED50, k_slot_count_pc = 0x9ED1E;
constexpr u32 k_slot_table_lit = 0x9ED54, k_slot_table_pc = 0x9ED18;
constexpr u32 k_stack = 0x1178EC, k_top = 0x1178F0, k_limit = 0x1178F4;
constexpr u32 k_tfunc_pc = 0x1C, k_tfunc_reader = 0x20, k_reader_buf = 0x4, k_reader_pos = 0x8;
constexpr u32 k_fetch_return = 0x9BC2E; // after Lua::execute's opcode fetch (bl at 0x9BC2A)
constexpr u32 k_number = 0xFFFFFFFEu, k_string = 0xFFFFFFFDu;
constexpr u32 k_below_sp = 0x10000;    // stack kept below sp: the calls' frames
constexpr u32 k_chunk = 0x1000;
constexpr int k_answer_sets = 16;

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    g_failures += ok ? 0 : 1;
}

u32 rd(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (addr != 0 && addr + 4ull <= cpu->mem_size) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}
void wr(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", static_cast<unsigned>(v));
    return buf;
}

// A guest call from the turn hook, the caller's registers kept.
u32 call(mt_cpu *cpu, u32 fn, std::initializer_list<u32> args) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, fn, args.begin(), static_cast<u32>(args.size()), fn);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

u32 main_menu(const mt_cpu *cpu) {
    const u32 canvas = rd(cpu, k_canvas);
    const u32 state = canvas != 0 ? rd(cpu, canvas + 0x0C) : 0;
    const u32 vtable = state != 0 ? rd(cpu, state) : 0;
    return vtable != 0 && rd(cpu, vtable + 6 * 4) == k_on_key_pressed ? state : 0;
}

// --- one run's record ----------------------------------------------------------------------

enum : u32 { kStep = 0, kNative = 1 };

struct Record {
    std::vector<u32> events; // kStep tfunc pc op | kNative index nargs (tag value)...
    u64 steps = 0, natives = 0;
    u32 result = 0;
    std::vector<std::pair<u32, std::vector<std::uint8_t>>> memory; // changed chunks
    mt_heap_stats heap{};
};

struct Recorder {
    Record *record = nullptr;
    int answers = 0;   // the answer set
    u32 answered = 0;  // natives answered in this run
    u64 rng = 0;       // the mixed sets' state
    mt_cpu *cpu = nullptr;
} g_rec;

void step(u32 tfunc, u32 pc, u32 op) {
    if (g_rec.record == nullptr) {
        return;
    }
    g_rec.record->events.insert(g_rec.record->events.end(), {kStep, tfunc, pc, op});
    g_rec.record->steps++;
}

void host_step(void *, u32 tfunc, u32 pc, u32 op) { step(tfunc, pc, op); }

// TFunc::fetchU8's hook: the recompiled loop's opcode fetches, then the original.
int fetch_observer(mt_cpu *cpu, const mt_hook_call *) {
    if ((cpu->r[MT_LR] & ~1u) == k_fetch_return) {
        const u32 tfunc = cpu->r[0];
        const u32 reader = rd(cpu, tfunc + k_tfunc_reader);
        const u32 at = rd(cpu, reader + k_reader_buf) + rd(cpu, reader + k_reader_pos);
        step(tfunc, rd(cpu, tfunc + k_tfunc_pc), at < cpu->mem_size ? cpu->mem[at] : 0x100u);
    }
    return MT_HOOK_PASS;
}

// The answer a native gives in this run.
u32 answer() {
    static constexpr int k_constant[] = {0, 1, 2, -1, 3};
    g_rec.answered++;
    if (g_rec.answers < 5) {
        return static_cast<u32>(k_constant[g_rec.answers]);
    }
    g_rec.rng ^= g_rec.rng << 13;
    g_rec.rng ^= g_rec.rng >> 7;
    g_rec.rng ^= g_rec.rng << 17;
    static constexpr int k_pool[] = {-1, 0, 1, 2, 3, 4, 5, 10, 20, 30, 50, 70, 100, 200, 1000};
    return static_cast<u32>(k_pool[g_rec.rng % (sizeof k_pool / sizeof k_pool[0])]);
}

bool returns_number(u32 index) {
    switch (index) {
    case 19: // getLevel
    case 20: // getQuestState
    case 23: // checkRequire
    case 34: // getReward
    case 35: // getEvtIndex
    case 40: // getMainQuest
    case 44: // getItem
    case 46: // getEquipState
    case 53: // getHeroClass
    case 54: // getClassUpCount
    case 56: // checkItemCount
    case 61: // isInvenAvailable
        return true;
    default:
        return false;
    }
}

// Lua::callNative's stub (L, index, base): the original's frame words (CnResults = the
// arguments above base, CBase after them) are its own business and come back as they were, so
// it returns CBase and leaves L alone.
int native_stub(mt_cpu *cpu, const mt_hook_call *) {
    const u32 index = cpu->r[1], base = cpu->r[2];
    const u32 stack = rd(cpu, k_stack);
    const u32 depth = static_cast<u32>(static_cast<std::int32_t>(rd(cpu, k_top) - stack) / 8);
    const u32 nargs = depth - base;
    if (g_rec.record != nullptr) {
        Record &rec = *g_rec.record;
        rec.events.insert(rec.events.end(), {kNative, index, nargs});
        for (u32 i = 0; i < nargs && i < 16; i++) {
            const u32 slot = stack + 8u * (base + i);
            rec.events.insert(rec.events.end(), {rd(cpu, slot), rd(cpu, slot + 4)});
        }
        rec.natives++;
    }
    const bool number = returns_number(index);
    if (number || index == 57) { // 57: getQuestTitle, a string
        const u32 t = rd(cpu, k_top);
        wr(cpu, t, number ? k_number : k_string);
        wr(cpu, t + 4, number ? answer() : 0u);
        wr(cpu, k_top, t + 8);
        if (t + 8 >= rd(cpu, k_limit)) {
            call(cpu, k_grow_stack, {cpu->r[0]});
        }
    }
    cpu->r[0] = base + nargs;
    return MT_HOOK_HANDLED;
}

// --- guest memory, kept and compared ---------------------------------------------------------

struct Range {
    u32 lo, hi;
    bool compared;
    std::vector<std::uint8_t> bytes;
};

struct Kept {
    std::vector<Range> ranges;
    mt_heap_stats heap{};
};

Kept keep(mt_cpu *cpu, u32 stack_top) {
    Kept k;
    mt_mem *mem = mt_mem_from_cpu(cpu);
    u32 n = 0;
    const mt_segment *segs = mt_mem_segments(mem, &n);
    for (u32 i = 0; i < n; i++) {
        if ((segs[i].initprot & MT_PROT_WRITE) != 0 && segs[i].vmsize != 0) {
            k.ranges.push_back({segs[i].vmaddr, segs[i].vmaddr + segs[i].vmsize, true, {}});
        }
    }
    mt_heap_get_stats(mem, &k.heap);
    k.ranges.push_back({MT_GUEST_HEAP_BASE, k.heap.committed, true, {}});
    const u32 sp = cpu->r[MT_SP];
    k.ranges.push_back({sp - k_below_sp, sp, false, {}});
    k.ranges.push_back({sp, stack_top, true, {}});
    for (Range &r : k.ranges) {
        r.bytes.assign(cpu->mem + r.lo, cpu->mem + r.hi);
    }
    return k;
}

// Puts kept memory back; the compared chunks that had changed go to `changed` (if not null).
void restore(mt_cpu *cpu, const Kept &k, std::vector<std::pair<u32, std::vector<std::uint8_t>>> *changed) {
    for (const Range &r : k.ranges) {
        for (u32 at = r.lo; at < r.hi; at += k_chunk) {
            const u32 n = std::min(k_chunk, r.hi - at);
            std::uint8_t *live = cpu->mem + at;
            const std::uint8_t *old = r.bytes.data() + (at - r.lo);
            if (std::memcmp(live, old, n) != 0) {
                if (changed != nullptr && r.compared) {
                    changed->emplace_back(at, std::vector<std::uint8_t>(live, live + n));
                }
                std::memcpy(live, old, n);
            }
        }
    }
}

// --- the corpus ------------------------------------------------------------------------------

struct Totals {
    u64 files = 0, functions = 0, runs = 0, steps = 0, natives = 0, differences = 0;
    std::set<std::tuple<u64, u32, u32>> reached; // (file, tfunc, pc)
};

// One function under one answer set: the original, then the host, from the same memory.
void compare(mt_cpu *cpu, const Kept &k, u32 lua, u32 tfunc, int answers, const std::string &what,
             u64 file, Totals &t) {
    Record rec[2];
    for (int host = 0; host < 2; host++) {
        g_rec.record = &rec[host];
        g_rec.answers = answers;
        g_rec.answered = 0;
        g_rec.rng = 0x9E3779B97F4A7C15ull * static_cast<u64>(answers + 1);
        mt_hook_set_enabled(MT_HOOK_FN_LUA_EXECUTE, host);
        rec[host].result = call(cpu, k_call_function, {lua, tfunc});
        g_rec.record = nullptr;
        mt_heap_get_stats(mt_mem_from_cpu(cpu), &rec[host].heap);
        restore(cpu, k, &rec[host].memory);
    }
    mt_hook_set_enabled(MT_HOOK_FN_LUA_EXECUTE, 0);
    t.runs += 2;
    t.steps += rec[0].steps;
    t.natives += rec[0].natives;
    for (std::size_t i = 0; i + 3 < rec[0].events.size();) {
        if (rec[0].events[i] == kStep) {
            t.reached.emplace(file, rec[0].events[i + 1], rec[0].events[i + 2]);
            i += 4;
        } else {
            i += 3 + 2 * std::min<u32>(rec[0].events[i + 2], 16);
        }
    }
    std::string diff;
    const auto heap_same = [&k](const mt_heap_stats &h) {
        return h.live_blocks == k.heap.live_blocks && h.live_bytes == k.heap.live_bytes &&
               h.top == k.heap.top && h.committed == k.heap.committed;
    };
    if (rec[0].steps == 0) {
        diff = "the recompiled loop's opcode fetches were not seen";
    } else if (rec[0].events != rec[1].events) {
        std::size_t i = 0;
        while (i < rec[0].events.size() && i < rec[1].events.size() &&
               rec[0].events[i] == rec[1].events[i]) {
            i++;
        }
        diff = "the records differ at word " + std::to_string(i) + " (" +
               std::to_string(rec[0].events.size()) + " and " +
               std::to_string(rec[1].events.size()) + " words; steps " +
               std::to_string(rec[0].steps) + " and " + std::to_string(rec[1].steps) + ")";
    } else if (rec[0].result != rec[1].result) {
        diff = "callFunction's results differ: " + hex(rec[0].result) + " and " +
               hex(rec[1].result);
    } else if (!heap_same(rec[0].heap) || !heap_same(rec[1].heap)) {
        diff = "a run moved the heap (" + std::to_string(rec[0].heap.live_blocks) + "/" +
               std::to_string(rec[1].heap.live_blocks) + " blocks, kept " +
               std::to_string(k.heap.live_blocks) + "): memory alone cannot undo it";
    } else if (rec[0].memory != rec[1].memory) {
        u32 at = 0;
        const auto &a = rec[0].memory, &b = rec[1].memory;
        for (std::size_t i = 0; i < std::max(a.size(), b.size()); i++) {
            if (i >= a.size() || i >= b.size() || a[i].first != b[i].first) {
                at = i < a.size() ? a[i].first : b[i].first;
                break;
            }
            if (a[i].second != b[i].second) {
                std::size_t j = 0;
                while (a[i].second[j] == b[i].second[j]) {
                    j++;
                }
                at = a[i].first + static_cast<u32>(j);
                break;
            }
        }
        char where[160] = "";
        mt_mem_describe(mt_mem_from_cpu(cpu), at, where, sizeof where);
        diff = "guest memory differs first at " + hex(at) + " (" + where + ")";
    }
    if (!diff.empty()) {
        t.differences++;
        if (t.differences <= 20) {
            check(false, what + ", answers " + std::to_string(answers) + ": " + diff);
        }
    }
}

struct Run {
    fe::VirtualClock clock;
    u64 turns = 0;
    double first = 0;
    bool done = false;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
};
Run *g_run = nullptr;

// The function slots' count and table, read as FunctionSlots::add reads them.
u32 slot_count(const mt_cpu *cpu) {
    return rd(cpu, rd(cpu, k_slot_count_lit) + k_slot_count_pc);
}
u32 slot_table(const mt_cpu *cpu) {
    return rd(cpu, rd(cpu, k_slot_table_lit) + k_slot_table_pc);
}

void corpus(mt_cpu *cpu) {
    Run &r = *g_run;
    // The guest stack's extent around sp.
    char where[160] = "";
    mt_mem_describe(mt_mem_from_cpu(cpu), cpu->r[MT_SP], where, sizeof where);
    unsigned stack_lo = 0, stack_hi = 0;
    const char *at = std::strstr(where, "guest stack ");
    check(at != nullptr && std::sscanf(at, "guest stack 0x%x-0x%x", &stack_lo, &stack_hi) == 2 &&
              stack_lo + k_below_sp <= cpu->r[MT_SP] && cpu->r[MT_SP] < stack_hi,
          "the turn hook's sp " + hex(cpu->r[MT_SP]) + " is on a guest stack (" + where + ")");
    if (stack_hi == 0) {
        return;
    }
    // The VM, as LoadGame makes it, and its stack grown well past what a script needs, so no
    // run grows it (a realloc in a run could not be undone by putting memory back).
    const u32 lua = call(cpu, k_operator_new, {0x18});
    call(cpu, k_lua_ctor, {lua, 0});
    for (int i = 0; i < 5; i++) {
        wr(cpu, k_top, rd(cpu, k_limit));
        call(cpu, k_grow_stack, {lua});
    }
    wr(cpu, k_top, rd(cpu, k_stack));
    const u32 slots = (rd(cpu, k_limit) - rd(cpu, k_stack)) / 8;
    check(lua != 0 && slots >= 320,
          "a Lua VM at " + hex(lua) + ", its stack " + std::to_string(slots) + " slots");
    mt_hook_register(MT_HOOK_FN_TFUNC_FETCH_U8, fetch_observer, nullptr, 1);
    mt_hook_register(MT_HOOK_FN_LUA_CALL_NATIVE, native_stub, nullptr, 1);
    mt_hooks_install_native_script_vm(0);
    mt_scriptvm_set_step_observer(host_step, nullptr);
    mt_scriptvm_reset_stats();

    Totals t;
    u64 max_slots = 0;
    for (const auto &[name, bytes] : r.files) {
        if (const char *only = std::getenv("MT_SCRIPTVM_ONLY");
            only != nullptr && name.find(only) == std::string::npos) {
            continue;
        }
        // A buffer of its own: the load frees it (a buffer kept for the next file would be
        // handed out again during that file's undump).
        const u32 buffer = call(cpu, k_operator_new, {static_cast<u32>(bytes.size())});
        std::memcpy(cpu->mem + buffer, bytes.data(), bytes.size());
        wr(cpu, k_top, rd(cpu, k_stack));
        call(cpu, k_load_buffer, {lua, buffer, static_cast<u32>(bytes.size())});
        const u32 main = rd(cpu, rd(cpu, k_main_ptr));
        const u32 count = slot_count(cpu);
        const u32 table = rd(cpu, rd(cpu, k_slots_ptr));
        if (main == 0 || count > 30 || table != slot_table(cpu)) {
            check(false, name + ": main " + hex(main) + ", " + std::to_string(count) +
                             " slots at " + hex(table) + " (FunctionSlots::add's " +
                             hex(slot_table(cpu)) + ")");
            continue;
        }
        max_slots = std::max<u64>(max_slots, count);
        t.files++;
        t.functions += 1 + count;
        const u64 file = t.files;
        {
            const Kept loaded = keep(cpu, stack_hi);
            for (int a = 0; a < k_answer_sets; a++) {
                compare(cpu, loaded, lua, main, a, name + " main", file, t);
            }
            for (u32 s = 0; s < count; s++) {
                for (int a = 0; a < k_answer_sets; a++) {
                    compare(cpu, loaded, lua, rd(cpu, table + 4 * s), a,
                            name + " slot " + std::to_string(s) + " after the load", file, t);
                }
            }
        }
        if (count != 0) {
            // After main (the recompiled loop, every answer 0), as a callback runs.
            g_rec.answers = 0;
            call(cpu, k_call_function, {lua, main});
            const Kept after_main = keep(cpu, stack_hi);
            for (u32 s = 0; s < count; s++) {
                for (int a = 0; a < k_answer_sets; a++) {
                    compare(cpu, after_main, lua, rd(cpu, table + 4 * s), a,
                            name + " slot " + std::to_string(s) + " after main", file, t);
                }
            }
        }
    }
    mt_scriptvm_set_step_observer(nullptr, nullptr);
    mt_hook_unregister(MT_HOOK_FN_TFUNC_FETCH_U8);
    mt_hook_unregister(MT_HOOK_FN_LUA_CALL_NATIVE);
    mt_hook_unregister(MT_HOOK_FN_LUA_EXECUTE);
    const mt_scriptvm_stats vm = mt_scriptvm_get_stats();
    std::printf("scriptvm corpus: %llu files, %llu functions (at most %llu slots in one), %llu "
                "runs, %llu opcodes and %llu native calls compared, %zu distinct (function, pc) "
                "reached; the host loop ran %llu calls, %llu opcodes, %llu checks\n",
                static_cast<unsigned long long>(t.files),
                static_cast<unsigned long long>(t.functions),
                static_cast<unsigned long long>(max_slots),
                static_cast<unsigned long long>(t.runs), static_cast<unsigned long long>(t.steps),
                static_cast<unsigned long long>(t.natives), t.reached.size(),
                static_cast<unsigned long long>(vm.calls),
                static_cast<unsigned long long>(vm.steps),
                static_cast<unsigned long long>(vm.checks));
    check(t.files == r.files.size() && t.files == 307 && t.functions == 1086,
          "all " + std::to_string(r.files.size()) + " files loaded: " + std::to_string(t.files) +
              " files, " + std::to_string(t.functions) +
              " functions (docs/formats/script-out.md: 307 files, 1,086 functions)");
    check(vm.steps == t.steps, "the host loop ran as many opcodes as the recompiled one (" +
                                   std::to_string(vm.steps) + ")");
    check(t.differences == 0, std::to_string(t.differences) + " runs with differences");
}

int turn(void *, mt_cpu *cpu, double now, double) {
    Run &r = *g_run;
    if (r.turns++ == 0) {
        r.first = now;
    }
    r.clock.advance(1000000);
    if (main_menu(cpu) == 0) {
        if (now - r.first > 20.0) {
            check(false, "CStateMainMenu is the engine's state within 20 s");
            return 1;
        }
        return 0;
    }
    corpus(cpu);
    r.done = true;
    return 1;
}

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int run(const fs::path &out) {
    const fs::path data = out / "data";
    fs::remove_all(data);
    fs::create_directories(data);
    Run r;
    const fs::path scripts = fs::path(MT_BUNDLE_DIR) / "script";
    for (const auto &e : fs::recursive_directory_iterator(scripts)) {
        if (e.is_regular_file() && e.path().extension() == ".out") {
            r.files.emplace_back(fs::relative(e.path(), scripts).generic_string(),
                                 read_file(e.path()));
        }
    }
    std::sort(r.files.begin(), r.files.end());
    g_run = &r;
    mt_hook_reset();
    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    mt_stubs_set_data_dir(data.string().c_str());
    mt_diag_configure_from_env();
    mt_input_configure(nullptr);
    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = turn;
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    mt_import_config_from_env(&cfg.imports);
    cfg.app_main = mt_uikit_app_main;
    cfg.app_user = &ui;
    if (!r.clock.install()) {
        check(false, "the virtual clock is installed");
        return 1;
    }
    const std::vector<std::uint8_t> slice = read_file(MT_ARMV7_SLICE);
    mt_program prog = MT_PROGRAM_GENERATED;
    mt_process *p = mt_process_load(&cfg, &prog, slice.data(), slice.size());
    if (p == nullptr) {
        check(false, "the boot loads the slice");
        return 1;
    }
    const std::uint32_t outcome = mt_process_run(p);
    const mt_process_result *res = mt_process_result_of(p);
    if (outcome == MT_PROCESS_TRAPPED) {
        std::printf("trap kind %u at pc 0x%08X detail 0x%08X\n", res->trap_kind, res->trap_pc,
                    res->trap_detail);
    }
    check(outcome == MT_PROCESS_STOPPED && r.done,
          "the corpus ran in the main menu's turn and the test ended the run");
    mt_process_destroy(p);
    mt_cf_configure(nullptr);
    mt_stubs_set_data_dir(nullptr);
    r.clock.uninstall();
    mt_hook_reset();
    g_run = nullptr;
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::printf("usage: scriptvm_real_test OUT_DIR\n");
        return 2;
    }
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    const fs::path out = argv[1];
    fs::create_directories(out);
    if (run(out) != 0) {
        return 1;
    }
    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

#else

int main() {
    std::printf("skipped: built without generated code (work/gen: python -m recomp all)\n");
    return 77;
}

#endif
