// The shadow runs (#430, ADR 0226): Lua::execute run as the original and as the host loop from
// one state, compared; see maplethief/scriptvm.h.
//
// A state is kept as the chunks of guest memory that differ from the entry's (the baseline), the
// heap's host bookkeeping and the registers. The chunks are 4 KiB of the compared ranges, in
// address order, so the first difference found is the lowest address.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/scriptvm.h"
#include "mem/mem_internal.h"
#include "scriptvm/scriptvm_internal.h"

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 k_chunk = 0x1000u;
constexpr u32 k_synthetic_dead = 0x10000u; // a synthetic guest's dead stack below the entry sp
constexpr std::size_t k_report_ops = 48u;  // the rewrite's last opcode lines in a report

struct Range {
    u32 lo, hi;
};

// The heap's host bookkeeping (mt::mem::Heap without its lock). Equal without the committed end:
// pages a run committed stay committed when a state is put back (zeroed), so where the heap's
// pages end is not part of what a run did; its top is.
struct Book {
    bool has = false;
    u32 top = 0, committed = 0;
    std::map<u32, u32> live;
    u64 live_bytes = 0;
    std::map<u32, u32> free_by_addr;
    std::set<std::pair<u32, u32>> free_by_size;
    std::vector<u32> small[mt::mem::k_small_classes];

    bool operator==(const Book &o) const {
        if (has != o.has || top != o.top || live != o.live || live_bytes != o.live_bytes ||
            free_by_addr != o.free_by_addr || free_by_size != o.free_by_size) {
            return false;
        }
        for (u32 c = 0; c < mt::mem::k_small_classes; c++) {
            if (small[c] != o.small[c]) {
                return false;
            }
        }
        return true;
    }
};

Book book_of(mt_mem *mem) {
    Book b;
    if (mem == nullptr) {
        return b;
    }
    const std::lock_guard<std::mutex> lock(mem->heap.mu);
    const mt::mem::Heap &h = mem->heap;
    b.has = true;
    b.top = h.top;
    b.committed = h.committed;
    b.live = h.live;
    b.live_bytes = h.live_bytes;
    b.free_by_addr = h.free_by_addr;
    b.free_by_size = h.free_by_size;
    for (u32 c = 0; c < mt::mem::k_small_classes; c++) {
        b.small[c] = h.small[c];
    }
    return b;
}

void apply_book(mt_mem *mem, const Book &b) {
    if (mem == nullptr || !b.has) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mem->heap.mu);
    mt::mem::Heap &h = mem->heap;
    h.top = b.top;
    // Pages committed after the state stay committed (and zeroed by the caller's chunks), so the
    // heap's committed end never goes below what is mapped read-write.
    h.committed = std::max(h.committed, b.committed);
    h.live = b.live;
    h.live_bytes = b.live_bytes;
    h.free_by_addr = b.free_by_addr;
    h.free_by_size = b.free_by_size;
    for (u32 c = 0; c < mt::mem::k_small_classes; c++) {
        h.small[c] = b.small[c];
    }
}

// What a function call leaves in the registers, and the rest a restore puts back.
struct Regs {
    u32 r[16] = {};
    u32 cpsr = 0, fpscr = 0;
    u64 d[32] = {};
};

Regs regs_of(const mt_cpu *cpu) {
    Regs g;
    std::memcpy(g.r, cpu->r, sizeof g.r);
    g.cpsr = cpu->cpsr;
    g.fpscr = cpu->fpscr;
    std::memcpy(g.d, cpu->d, sizeof g.d);
    return g;
}

void apply_regs(mt_cpu *cpu, const Regs &g) {
    std::memcpy(cpu->r, g.r, sizeof g.r);
    cpu->cpsr = g.cpsr;
    cpu->fpscr = g.fpscr;
    std::memcpy(cpu->d, g.d, sizeof g.d);
}

struct State {
    std::map<u32, std::vector<u8>> chunks; // chunk address -> bytes, where not the baseline's
    Book book;
    Regs regs;
};

struct NativeCall {
    u32 lua = 0, index = 0, base = 0, sp = 0, lr = 0;
    State before, after;
};

struct Difference {
    bool found = false;
    std::string what;
    u32 address = 0, original = 0, rewrite = 0;
};

enum class Mode { idle, original, rewrite };

// One shadow run's records, and the process's numbers.
struct Shadow {
    mt_scriptvm_execute_fn rewrite = nullptr;
    Mode mode = Mode::idle;
    mt_cpu *cpu = nullptr;
    mt_mem *mem = nullptr;
    std::vector<Range> ranges; // compared, ascending; ranges[heap] grows with the heap
    int heap = -1;
    std::map<u32, std::vector<u8>> baseline; // the entry's chunks
    std::vector<NativeCall> natives;         // run 1's
    std::size_t next_native = 0;             // run 2's
    Difference first;                        // this run's first
    u32 first_checkpoint = 0;
    int in_native = 0; // inside a native that runs: its own calls are not the run's
    bool first_at_end = false;
    std::vector<std::string> ops; // run 2's opcode lines

    u64 runs = 0, native_count = 0, differences = 0;
    bool have_process_first = false;
    mt_scriptvm_shadow_difference process_first{};
};
Shadow g_shadow;

// --- the compared ranges and their chunks -------------------------------------------------------

void set_ranges(Shadow &s, u32 sp) {
    s.ranges.clear();
    s.heap = -1;
    if (s.mem == nullptr) {
        const u32 size = static_cast<u32>(std::min<u64>(s.cpu->mem_size, 0xFFFFFFFFu));
        const u32 dead = sp >= k_synthetic_dead ? sp - k_synthetic_dead : 0u;
        s.ranges.push_back({0u, dead});
        s.ranges.push_back({std::min(sp, size), size});
        return;
    }
    u32 n = 0;
    const mt_segment *segs = mt_mem_segments(s.mem, &n);
    for (u32 i = 0; i < n; i++) {
        if ((segs[i].initprot & MT_PROT_WRITE) != 0 && segs[i].vmsize != 0) {
            s.ranges.push_back({segs[i].vmaddr, segs[i].vmaddr + segs[i].vmsize});
        }
    }
    {
        const std::lock_guard<std::mutex> lock(s.mem->heap.mu);
        s.heap = static_cast<int>(s.ranges.size());
        s.ranges.push_back({MT_GUEST_HEAP_BASE, s.mem->heap.committed});
    }
    {
        const std::lock_guard<std::mutex> lock(s.mem->stacks_mu);
        for (const mt::mem::Stack &st : s.mem->stacks) {
            if (sp >= st.base && sp < st.top) {
                s.ranges.push_back({sp, st.top});
            }
        }
    }
    std::sort(s.ranges.begin(), s.ranges.end(),
              [](const Range &a, const Range &b) { return a.lo < b.lo; });
    for (std::size_t i = 0; i < s.ranges.size(); i++) {
        if (s.ranges[i].lo == MT_GUEST_HEAP_BASE) {
            s.heap = static_cast<int>(i);
        }
    }
}

// The heap's range reaches its committed end, which only grows while a run lasts (a restore
// keeps the pages, zeroed).
void grow_heap(Shadow &s) {
    if (s.mem == nullptr || s.heap < 0) {
        return;
    }
    const std::lock_guard<std::mutex> lock(s.mem->heap.mu);
    Range &r = s.ranges[static_cast<std::size_t>(s.heap)];
    r.hi = std::max(r.hi, s.mem->heap.committed);
}

template <typename F> void each_chunk(const Shadow &s, F &&f) {
    for (const Range &r : s.ranges) {
        for (u32 a = r.lo; a < r.hi;) {
            const u32 end = r.hi - a > k_chunk ? a + k_chunk : r.hi;
            f(a, end - a);
            a = end;
        }
    }
}

// The baseline's bytes of a chunk: the entry's, or zero (heap pages committed since).
const u8 *baseline_of(const Shadow &s, u32 a, u32 n, std::vector<u8> &zero) {
    const auto it = s.baseline.find(a);
    if (it != s.baseline.end() && it->second.size() == n) {
        return it->second.data();
    }
    zero.assign(n, 0);
    return zero.data();
}

const u8 *expected_of(const Shadow &s, const State &x, u32 a, u32 n, std::vector<u8> &zero) {
    const auto it = x.chunks.find(a);
    if (it != x.chunks.end() && it->second.size() == n) {
        return it->second.data();
    }
    return baseline_of(s, a, n, zero);
}

void take_baseline(Shadow &s) {
    s.baseline.clear();
    each_chunk(s, [&s](u32 a, u32 n) {
        s.baseline.emplace(a, std::vector<u8>(s.cpu->mem + a, s.cpu->mem + a + n));
    });
}

State capture(Shadow &s) {
    grow_heap(s);
    State x;
    std::vector<u8> zero;
    each_chunk(s, [&](u32 a, u32 n) {
        const u8 *live = s.cpu->mem + a;
        if (std::memcmp(live, baseline_of(s, a, n, zero), n) != 0) {
            x.chunks.emplace(a, std::vector<u8>(live, live + n));
        }
    });
    x.book = book_of(s.mem);
    x.regs = regs_of(s.cpu);
    return x;
}

// Guest memory and the heap's bookkeeping made `x`'s (registers apart).
void put_back(Shadow &s, const State &x) {
    grow_heap(s);
    std::vector<u8> zero;
    each_chunk(s, [&](u32 a, u32 n) {
        const u8 *want = expected_of(s, x, a, n, zero);
        if (std::memcmp(s.cpu->mem + a, want, n) != 0) {
            std::memcpy(s.cpu->mem + a, want, n);
        }
    });
    apply_book(s.mem, x.book);
}

// The word at `addr` & ~3 of a chunk's bytes [chunk, chunk + n), as far as it is in the chunk.
u32 word_at(const u8 *bytes, u32 chunk, u32 n, u32 addr) {
    u32 v = 0;
    for (u32 i = 0; i < 4; i++) {
        const u32 b = (addr & ~3u) + i;
        if (b >= chunk && b < chunk + n) {
            v |= static_cast<u32>(bytes[b - chunk]) << (8u * i);
        }
    }
    return v;
}

// The first byte where live memory is not `x`'s, then the heap's bookkeeping.
Difference memory_difference(Shadow &s, const State &x) {
    grow_heap(s);
    Difference d;
    std::vector<u8> zero;
    each_chunk(s, [&](u32 a, u32 n) {
        if (d.found) {
            return;
        }
        const u8 *want = expected_of(s, x, a, n, zero);
        const u8 *live = s.cpu->mem + a;
        if (std::memcmp(live, want, n) == 0) {
            return;
        }
        u32 i = 0;
        while (live[i] == want[i]) {
            i++;
        }
        d.found = true;
        d.what = "memory";
        d.address = a + i;
        d.original = word_at(want, a, n, a + i);
        d.rewrite = word_at(live, a, n, a + i);
    });
    if (!d.found && !(book_of(s.mem) == x.book)) {
        const Book now = book_of(s.mem);
        d.found = true;
        d.what = "heap bookkeeping (top, live blocks)";
        d.original = x.book.top;
        d.rewrite = now.top;
        d.address = static_cast<u32>(x.book.live.size());
    }
    return d;
}

Difference register_difference(const Regs &original, const Regs &rewrite,
                               std::initializer_list<int> which) {
    static const char *const names[16] = {"r0", "r1", "r2",  "r3",  "r4",  "r5", "r6", "r7",
                                          "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
    Difference d;
    for (const int r : which) {
        if (original.r[r] != rewrite.r[r]) {
            d.found = true;
            d.what = names[r];
            d.original = original.r[r];
            d.rewrite = rewrite.r[r];
            return d;
        }
    }
    return d;
}

// --- the report ------------------------------------------------------------------------------

void note_difference(Shadow &s, const Difference &d, u32 checkpoint, bool at_end) {
    if (s.first.found || !d.found) {
        return;
    }
    s.first = d;
    s.first_checkpoint = checkpoint;
    s.first_at_end = at_end;
}

void report(Shadow &s) {
    const Difference &d = s.first;
    std::string where = s.first_at_end ? "at the call's end"
                                       : "at native call " + std::to_string(s.first_checkpoint + 1);
    char line[320];
    std::snprintf(line, sizeof line,
                  "scriptvm shadow: run %llu differs %s (%u native calls recorded): %s",
                  static_cast<unsigned long long>(s.runs), where.c_str(),
                  static_cast<unsigned>(s.natives.size()), d.what.c_str());
    std::string text = line;
    if (d.what == "memory") {
        std::snprintf(line, sizeof line,
                      " at 0x%08X: the original's word 0x%08X, the rewrite's 0x%08X",
                      static_cast<unsigned>(d.address), static_cast<unsigned>(d.original),
                      static_cast<unsigned>(d.rewrite));
    } else {
        std::snprintf(line, sizeof line, ": the original's 0x%08X, the rewrite's 0x%08X",
                      static_cast<unsigned>(d.original), static_cast<unsigned>(d.rewrite));
    }
    text += line;
    text += "\n";
    for (std::size_t i = 0; i < s.natives.size(); i++) {
        const NativeCall &c = s.natives[i];
        std::snprintf(line, sizeof line, "  native call %zu: index %u, base %u\n", i + 1,
                      static_cast<unsigned>(c.index), static_cast<unsigned>(c.base));
        text += line;
    }
    const std::size_t from = s.ops.size() > k_report_ops ? s.ops.size() - k_report_ops : 0u;
    std::snprintf(line, sizeof line, "  the rewrite's last %zu of %zu opcode lines:\n",
                  s.ops.size() - from, s.ops.size());
    text += line;
    for (std::size_t i = from; i < s.ops.size(); i++) {
        text += "    " + s.ops[i] + "\n";
    }
    std::fputs(text.c_str(), stderr);
    std::fflush(stderr);

    s.differences++;
    if (!s.have_process_first) {
        s.have_process_first = true;
        mt_scriptvm_shadow_difference &p = s.process_first;
        p = mt_scriptvm_shadow_difference{};
        p.run = s.runs;
        p.checkpoint = s.first_checkpoint;
        p.at_end = s.first_at_end ? 1u : 0u;
        std::snprintf(p.what, sizeof p.what, "%s", d.what.c_str());
        p.address = d.address;
        p.original = d.original;
        p.rewrite = d.rewrite;
    }
}

// --- the handlers ----------------------------------------------------------------------------

int shadow_execute(mt_cpu *cpu, const mt_hook_call *call) {
    Shadow &s = g_shadow;
    if (s.mode == Mode::original || (s.mode == Mode::rewrite && s.in_native > 0)) {
        return MT_HOOK_PASS; // nested in run 1, or in a native that runs: the original's
    }
    const mt_scriptvm_execute_fn rewrite = s.rewrite != nullptr ? s.rewrite : mt_scriptvm_execute;
    if (s.mode == Mode::rewrite) {
        cpu->r[0] = rewrite(cpu, cpu->r[0], cpu->r[1], cpu->r[2]); // nested in run 2
        return MT_HOOK_HANDLED;
    }

    // The outermost call: the entry state.
    s.runs++;
    s.cpu = cpu;
    s.mem = mt_mem_from_cpu(cpu);
    s.natives.clear();
    s.next_native = 0;
    s.first = Difference{};
    s.ops.clear();
    const Regs entry = regs_of(cpu);
    set_ranges(s, cpu->r[MT_SP]);
    take_baseline(s);
    const Book entry_book = book_of(s.mem);

    // 1. The original.
    s.mode = Mode::original;
    call->original(cpu);
    s.mode = Mode::idle;
    const State end1 = capture(s);

    // 2. The rewrite from the same state.
    State start;
    start.book = entry_book;
    put_back(s, start);
    apply_regs(cpu, entry);
    s.mode = Mode::rewrite;
    mt::scriptvm::set_recorder(&s.ops);
    cpu->r[0] = rewrite(cpu, entry.r[0], entry.r[1], entry.r[2]);
    mt::scriptvm::set_recorder(nullptr);
    s.mode = Mode::idle;

    // 3. The comparison.
    const auto at_end = static_cast<u32>(s.natives.size());
    if (s.next_native != s.natives.size()) {
        Difference d;
        d.found = true;
        d.what = "the number of native calls";
        d.original = static_cast<u32>(s.natives.size());
        d.rewrite = static_cast<u32>(s.next_native);
        note_difference(s, d, at_end, true);
    }
    // r0, the callee-saved registers and sp. r9 is not one on iOS (Apple's armv7 ABI: a scratch
    // register), and the original's natives leave it changed.
    const Regs regs2 = regs_of(cpu);
    note_difference(s, register_difference(end1.regs, regs2, {0, 4, 5, 6, 7, 8, 10, 11, 13}),
                    at_end, true);
    note_difference(s, memory_difference(s, end1), at_end, true);
    if (s.first.found) {
        report(s);
    }

    // The game goes on from the original's end.
    put_back(s, end1);
    apply_regs(cpu, end1.regs);
    s.baseline.clear();
    s.natives.clear();
    return MT_HOOK_HANDLED;
}

int shadow_call_native(mt_cpu *cpu, const mt_hook_call *call) {
    Shadow &s = g_shadow;
    if (s.mode == Mode::idle || s.in_native > 0) {
        return MT_HOOK_PASS;
    }
    NativeCall here;
    here.lua = cpu->r[0];
    here.index = cpu->r[1];
    here.base = cpu->r[2];
    here.sp = cpu->r[MT_SP];
    here.lr = cpu->r[MT_LR];
    if (s.mode == Mode::original) {
        // Run 1: the native runs, between its two states.
        here.before = capture(s);
        s.in_native++;
        call->original(cpu);
        s.in_native--;
        here.after = capture(s);
        s.natives.push_back(std::move(here));
        s.native_count++;
        return MT_HOOK_HANDLED;
    }

    // Run 2: the same call in the same state, then run 1's state after it.
    const std::size_t k = s.next_native++;
    if (k >= s.natives.size()) {
        Difference d;
        d.found = true;
        d.what = "a native call the original did not make (index)";
        d.original = 0;
        d.rewrite = here.index;
        note_difference(s, d, static_cast<u32>(k), false);
        s.in_native++; // nothing to replay: it runs, which the original did not
        call->original(cpu);
        s.in_native--;
        return MT_HOOK_HANDLED;
    }
    const NativeCall &rec = s.natives[k];
    const struct {
        const char *what;
        u32 original, rewrite;
    } fields[] = {{"native call L", rec.lua, here.lua},
                  {"native call index", rec.index, here.index},
                  {"native call base", rec.base, here.base},
                  {"native call sp", rec.sp, here.sp},
                  {"native call lr", rec.lr, here.lr}};
    for (const auto &f : fields) {
        if (f.original != f.rewrite) {
            Difference d;
            d.found = true;
            d.what = f.what;
            d.original = f.original;
            d.rewrite = f.rewrite;
            note_difference(s, d, static_cast<u32>(k), false);
            break;
        }
    }
    note_difference(s, memory_difference(s, rec.before), static_cast<u32>(k), false);
    put_back(s, rec.after);
    for (int r = 0; r < 4; r++) {
        cpu->r[r] = rec.after.regs.r[r];
    }
    cpu->r[12] = rec.after.regs.r[12];
    return MT_HOOK_HANDLED;
}

} // namespace

extern "C" void mt_scriptvm_shadow_install(int enabled, mt_scriptvm_execute_fn rewrite) noexcept {
    g_shadow.rewrite = rewrite;
    mt_hook_register(MT_HOOK_FN_LUA_EXECUTE, shadow_execute, nullptr, enabled);
    mt_hook_register(MT_HOOK_FN_LUA_CALL_NATIVE, shadow_call_native, nullptr, enabled);
}

extern "C" mt_scriptvm_shadow_stats mt_scriptvm_shadow_get_stats(void) noexcept {
    mt_scriptvm_shadow_stats st{};
    st.runs = g_shadow.runs;
    st.natives = g_shadow.native_count;
    st.differences = g_shadow.differences;
    return st;
}

extern "C" int mt_scriptvm_shadow_first_difference(mt_scriptvm_shadow_difference *out) noexcept {
    if (!g_shadow.have_process_first) {
        return 0;
    }
    if (out != nullptr) {
        *out = g_shadow.process_first;
    }
    return 1;
}

extern "C" void mt_scriptvm_shadow_reset(void) noexcept {
    g_shadow.runs = 0;
    g_shadow.native_count = 0;
    g_shadow.differences = 0;
    g_shadow.have_process_first = false;
    g_shadow.process_first = mt_scriptvm_shadow_difference{};
}
