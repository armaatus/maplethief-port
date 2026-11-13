// Save states (#202, ADR 0105): the safe-point check, the sections of guest memory and of the
// runtime's core tables, saving, and the all-or-nothing restore. maplethief/snapshot.h is the
// interface; state_file.h the file; snapshot_internal.h the host state serializers.
//
// The sections this file writes (each a payload of Writer values):
//   cpu          r0-r15, cpsr, fpscr, d0-d31 of the main cpu
//   sjlj         the unwinder's chain (checked equal at a restore: at the turn hook it is the
//                run loop's, and its host jump buffers live on the same host stack every turn)
//   image        the writable image segments' pages
//   heap         the heap's pages up to its committed end (a lowered top leaves old bytes that a
//                later malloc hands out again, so they count too)
//   heap.book    top, committed, live blocks, free ranges, the small free lists in order
//   stacks       every guest stack's pages (only the main one at a safe point)
//   objc         retain counts, @synchronized depths, autorelease pools, +initialize flags, the
//                selector table (names, canonical SELs, every known guest copy)
//   objc.states  every live host state: its serializer's name, the object, the payload
//   process      the __cxa_atexit registrations
// and each Module's own section (CF, Foundation). Pages are 4 KiB; a page of zeros is left out.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "boot/process.h"
#include "hle/libc_internal.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/gcd.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/sjlj.h"
#include "maplethief/snapshot.h"
#include "maplethief/uikit.h"
#include "mem/mem_internal.h"
#include "objc/objc_internal.h"
#include "snapshot/snapshot_internal.h"
#include "snapshot/state_file.h"

#ifndef MT_VERSION
#define MT_VERSION "unknown"
#endif

namespace mt_snap {

namespace {

using mt_objc_impl::State;

constexpr u32 k_page = 4096u;

mt_snapshot_config g_config = {0x11AA00u, 0x108C64u, 1};

void set_why(char *why, std::size_t size, const char *text) {
    if (why != nullptr && size != 0) {
        std::snprintf(why, size, "%s", text);
    }
}
void set_why(char *why, std::size_t size, const std::string &text) {
    set_why(why, size, text.c_str());
}

// The guest's clock now, Unix microseconds (what gettimeofday and CFAbsoluteTime read).
u64 guest_unix_us() {
    const mt_libc_config &c = mt_libc_cfg();
    if (c.clock != nullptr) {
        return c.clock(c.clock_user);
    }
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

std::string hex32(u32 v) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", static_cast<unsigned>(v));
    return text;
}

bool read_i32(const mt_cpu *cpu, u32 addr, std::int32_t *v) {
    if (addr < MT_GUEST_PAGE_ZERO_END || static_cast<u64>(addr) + 4u > cpu->mem_size) {
        return false;
    }
    std::memcpy(v, cpu->mem + addr, 4);
    return true;
}

// --- pages ---------------------------------------------------------------------------------

bool zero_page(const std::uint8_t *p, u32 n) {
    static const std::uint8_t k_zero[k_page] = {};
    return std::memcmp(p, k_zero, n) == 0;
}

void save_pages(Writer &w, const std::uint8_t *base, u32 lo, u32 hi) {
    std::vector<u32> nonzero;
    for (u32 a = lo; a < hi; a += k_page) {
        const u32 n = std::min(k_page, hi - a);
        if (!zero_page(base + a, n)) {
            nonzero.push_back(a);
        }
    }
    w.u32(lo);
    w.u32(hi);
    w.u32(static_cast<u32>(nonzero.size()));
    for (const u32 a : nonzero) {
        w.u32(a);
        w.raw(base + a, std::min(k_page, hi - a));
    }
}

struct Pages {
    u32 lo = 0, hi = 0;
    std::vector<std::pair<u32, const std::uint8_t *>> pages; // address, bytes
};

bool read_pages(Reader &r, Pages *p) {
    u32 n = 0;
    if (!r.u32(&p->lo) || !r.u32(&p->hi) || !r.u32(&n)) {
        return false;
    }
    if (p->hi < p->lo || p->hi > MT_GUEST_ARENA_SIZE || p->lo < MT_GUEST_PAGE_ZERO_END) {
        return r.fail("a page range outside guest memory");
    }
    if (!r.count_fits(n, 4)) {
        return false;
    }
    u32 last = 0;
    for (u32 i = 0; i < n; i++) {
        u32 a = 0;
        if (!r.u32(&a)) {
            return false;
        }
        if (a < p->lo || a >= p->hi || (a - p->lo) % k_page != 0 || (i != 0 && a <= last)) {
            return r.fail("a page outside its range or out of order");
        }
        const std::uint8_t *bytes = r.raw(std::min(k_page, p->hi - a));
        if (bytes == nullptr) {
            return false;
        }
        p->pages.emplace_back(a, bytes);
        last = a;
    }
    return true;
}

void apply_pages(std::uint8_t *base, const Pages &p) {
    std::memset(base + p.lo, 0, p.hi - p.lo);
    for (const auto &[a, bytes] : p.pages) {
        std::memcpy(base + a, bytes, std::min(k_page, p.hi - a));
    }
}

// --- cpu and sjlj ----------------------------------------------------------------------------

void save_cpu(const mt_cpu *cpu, Writer &w) {
    for (const u32 r : cpu->r) {
        w.u32(r);
    }
    w.u32(cpu->cpsr);
    w.u32(cpu->fpscr);
    for (const u64 d : cpu->d) {
        w.u64(d);
    }
}

struct CpuRegs {
    u32 r[16];
    u32 cpsr, fpscr;
    u64 d[32];
};

bool read_cpu(Reader &r, CpuRegs *c) {
    for (u32 &v : c->r) {
        if (!r.u32(&v)) {
            return false;
        }
    }
    if (!r.u32(&c->cpsr) || !r.u32(&c->fpscr)) {
        return false;
    }
    for (u64 &d : c->d) {
        if (!r.u64(&d)) {
            return false;
        }
    }
    return r.done();
}

// The unwinder's chain as a value: its top, the exceptions being handled and the guest jump
// buffers that have a host jump buffer.
struct SjljChain {
    u32 top = 0, caught = 0;
    std::vector<u32> frames;
    bool operator==(const SjljChain &) const = default;
};

SjljChain sjlj_chain(mt_cpu *cpu) {
    SjljChain c;
    const mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    c.top = s->top;
    c.caught = s->caught_count;
    for (const mt_sjlj_frame &f : s->frames) {
        if (f.jbuf != 0) {
            c.frames.push_back(f.jbuf);
        }
    }
    std::sort(c.frames.begin(), c.frames.end());
    return c;
}

// --- the heap's bookkeeping --------------------------------------------------------------------

void save_heap_book(mt_mem *mem, Writer &w) {
    const mt::mem::Heap &h = mem->heap;
    w.u32(h.top);
    w.u32(h.committed);
    w.u32(static_cast<u32>(h.live.size()));
    for (const auto &[a, n] : h.live) {
        w.u32(a);
        w.u32(n);
    }
    w.u32(static_cast<u32>(h.free_by_addr.size()));
    for (const auto &[a, n] : h.free_by_addr) {
        w.u32(a);
        w.u32(n);
    }
    for (const std::vector<u32> &list : h.small) {
        w.words(list);
    }
}

struct HeapBook {
    u32 top = 0, committed = 0;
    std::vector<std::pair<u32, u32>> live, free;
    std::vector<u32> small[mt::mem::k_small_classes];
};

bool read_heap_book(Reader &r, HeapBook *b) {
    if (!r.u32(&b->top) || !r.u32(&b->committed)) {
        return false;
    }
    if (b->top < MT_GUEST_HEAP_BASE || b->committed < b->top || b->committed > MT_GUEST_HEAP_END ||
        (b->committed - MT_GUEST_HEAP_BASE) % mt::mem::k_heap_commit_step != 0) {
        return r.fail("the heap's top or committed end is out of range");
    }
    const auto in_heap = [b](u32 a, u32 n) {
        return a >= MT_GUEST_HEAP_BASE && n != 0 && n % MT_HEAP_ALIGN == 0 &&
               a % MT_HEAP_ALIGN == 0 && static_cast<u64>(a) + n <= b->top;
    };
    for (auto *list : {&b->live, &b->free}) {
        u32 n = 0;
        if (!r.u32(&n) || !r.count_fits(n, 8)) {
            return false;
        }
        list->resize(n);
        for (auto &[a, size] : *list) {
            r.u32(&a);
            r.u32(&size);
            if (!in_heap(a, size)) {
                return r.fail("a heap block outside the heap");
            }
        }
    }
    for (u32 c = 0; c < mt::mem::k_small_classes; c++) {
        if (!r.words(&b->small[c])) {
            return false;
        }
        for (const u32 a : b->small[c]) {
            if (!in_heap(a, (c + 1) * MT_HEAP_ALIGN)) {
                return r.fail("a free small block outside the heap");
            }
        }
    }
    return r.done();
}

void apply_heap_book(mt_mem *mem, const HeapBook &b) {
    mt::mem::Heap &h = mem->heap;
    h.top = b.top;
    h.committed = b.committed;
    h.live.clear();
    h.live_bytes = 0;
    for (const auto &[a, n] : b.live) {
        h.live.emplace(a, n);
        h.live_bytes += n;
    }
    h.free_by_addr.clear();
    h.free_by_size.clear();
    for (const auto &[a, n] : b.free) {
        h.free_by_addr.emplace(a, n);
        h.free_by_size.emplace(n, a);
    }
    for (u32 c = 0; c < mt::mem::k_small_classes; c++) {
        h.small[c] = b.small[c];
    }
}

// --- the ObjC runtime's tables -------------------------------------------------------------

void save_map(Writer &w, const mt_objc_impl::U32Map &m) {
    std::vector<std::pair<u32, u32>> pairs;
    for (u32 i = 0; i < m.cap; i++) {
        if (m.slots[2 * i] != 0) {
            pairs.emplace_back(m.slots[2 * i], m.slots[2 * i + 1]);
        }
    }
    std::sort(pairs.begin(), pairs.end());
    w.u32(static_cast<u32>(pairs.size()));
    for (const auto &[k, v] : pairs) {
        w.u32(k);
        w.u32(v);
    }
}

bool read_map(Reader &r, std::vector<std::pair<u32, u32>> *pairs) {
    u32 n = 0;
    if (!r.u32(&n) || !r.count_fits(n, 8)) {
        return false;
    }
    pairs->resize(n);
    for (auto &[k, v] : *pairs) {
        r.u32(&k);
        r.u32(&v);
        if (k == 0) {
            return r.fail("a table entry with key 0");
        }
    }
    return true;
}

bool fill_map(mt_objc_impl::U32Map &m, const std::vector<std::pair<u32, u32>> &pairs) {
    mt_objc_impl::map_clear(m);
    for (const auto &[k, v] : pairs) {
        if (!mt_objc_impl::map_put(m, k, v)) {
            return false;
        }
    }
    return true;
}

void save_objc(const mt_objc *rt, Writer &w) {
    save_map(w, rt->retains);
    save_map(w, rt->syncs);
    w.words(std::vector<u32>(rt->pool_objects, rt->pool_objects + rt->pool_count));
    w.u32(rt->mark_count);
    for (u32 i = 0; i < rt->mark_count; i++) {
        w.u32(rt->marks[i].pool);
        w.u32(rt->marks[i].start);
    }
    std::vector<std::string> initialized;
    for (u32 i = 0; i < rt->class_count; i++) {
        const mt_objc_impl::Class &c = rt->classes[i];
        if (!c.is_meta && c.initialized && c.name != nullptr) {
            initialized.emplace_back(c.name);
        }
    }
    w.u32(static_cast<u32>(initialized.size()));
    for (const std::string &n : initialized) {
        w.str(n);
    }
    w.u32(rt->sel_count);
    for (u32 i = 0; i < rt->sel_count; i++) {
        w.str(rt->sel_names[i]);
        w.u32(rt->sel_canon[i]);
    }
    save_map(w, rt->sel_ids);
}

struct ObjcTables {
    std::vector<std::pair<u32, u32>> retains, syncs, sel_ids;
    std::vector<u32> pool_objects;
    std::vector<mt_objc_impl::PoolMark> marks;
    std::vector<long> initialized; // class indices (not metaclasses)
    std::vector<std::string> sel_names;
    std::vector<u32> sel_canon;
};

bool read_objc(mt_objc *rt, Reader &r, ObjcTables *t) {
    u32 n = 0;
    if (!read_map(r, &t->retains) || !read_map(r, &t->syncs) || !r.words(&t->pool_objects) ||
        !r.u32(&n) || !r.count_fits(n, 8)) {
        return false;
    }
    t->marks.resize(n);
    for (mt_objc_impl::PoolMark &m : t->marks) {
        r.u32(&m.pool);
        r.u32(&m.start);
        if (m.start > t->pool_objects.size()) {
            return r.fail("an autorelease pool past its objects");
        }
    }
    if (!r.u32(&n) || !r.count_fits(n, 4)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        std::string name;
        if (!r.str(&name)) {
            return false;
        }
        const long ci = mt_objc_impl::class_index(rt, mt_objc_class_named(rt, name.c_str()));
        if (ci < 0 || rt->classes[ci].is_meta) {
            return r.fail("class " + name + " is not in this process");
        }
        t->initialized.push_back(ci);
    }
    if (!r.u32(&n) || !r.count_fits(n, 8)) {
        return false;
    }
    t->sel_names.resize(n);
    t->sel_canon.resize(n);
    std::unordered_set<std::string> names;
    for (u32 i = 0; i < n; i++) {
        if (!r.str(&t->sel_names[i]) || !r.u32(&t->sel_canon[i])) {
            return false;
        }
        if (t->sel_names[i].empty() || t->sel_names[i].find('\0') != std::string::npos ||
            !names.insert(t->sel_names[i]).second) {
            return r.fail("a malformed or repeated selector");
        }
    }
    if (!read_map(r, &t->sel_ids) || !r.done()) {
        return false;
    }
    for (const auto &[at, id1] : t->sel_ids) {
        if (id1 == 0 || id1 > n) {
            return r.fail("a selector copy of no selector");
        }
    }
    // The selectors both tables have must be the same ones (one binary, one boot), and no
    // method may use a selector the state does not have.
    const u32 common = std::min(n, rt->sel_count);
    for (u32 i = 0; i < common; i++) {
        if (t->sel_names[i] != rt->sel_names[i]) {
            return r.fail("the selector table differs from this process's (selector " +
                          std::to_string(i) + ")");
        }
    }
    for (u32 i = 0; i < rt->method_count; i++) {
        if (rt->methods[i].sel > n) {
            return r.fail("a method uses a selector the state does not have");
        }
    }
    return true;
}

bool apply_objc(mt_objc *rt, const ObjcTables &t) {
    bool ok = fill_map(rt->retains, t.retains) && fill_map(rt->syncs, t.syncs);
    rt->pool_count = 0;
    for (const u32 o : t.pool_objects) {
        ok = ok && mt_objc_impl::push(rt->pool_objects, rt->pool_count, rt->pool_cap, o) >= 0;
    }
    rt->mark_count = 0;
    for (const mt_objc_impl::PoolMark &m : t.marks) {
        ok = ok && mt_objc_impl::push(rt->marks, rt->mark_count, rt->mark_cap, m) >= 0;
    }
    for (u32 i = 0; i < rt->class_count; i++) {
        rt->classes[i].initialized = false;
        mt_objc_impl::map_clear(rt->classes[i].cache);
    }
    for (const long ci : t.initialized) {
        rt->classes[ci].initialized = true;
        rt->classes[rt->classes[ci].meta - 1].initialized = true;
    }
    const u32 n = static_cast<u32>(t.sel_names.size());
    if (n < rt->sel_count) {
        ok = ok && mt_objc_impl::sel_truncate(rt, n);
    }
    for (u32 i = rt->sel_count; i < n && ok; i++) {
        ok = mt_objc_impl::sel_id_of(rt, t.sel_names[i].c_str()) == static_cast<long>(i);
    }
    for (u32 i = 0; i < n && i < rt->sel_count; i++) {
        rt->sel_canon[i] = t.sel_canon[i];
    }
    for (u32 i = 0; i < rt->undeliverable_count; i++) {
        if (rt->undeliverable[i].sel_id > n) {
            rt->undeliverable[i].sel_id = 0; // a selector the state does not have
        }
    }
    return fill_map(rt->sel_ids, t.sel_ids) && ok;
}

// --- host states -----------------------------------------------------------------------------

// Every live state's index, object by object (ascending), each object's oldest state first: the
// order that rebuilds each object's chain as it was.
std::vector<u32> states_in_order(const mt_objc *rt) {
    std::vector<u32> objects;
    for (u32 i = 0; i < rt->state_count; i++) {
        if (rt->states[i].type != nullptr) {
            objects.push_back(rt->states[i].obj);
        }
    }
    std::sort(objects.begin(), objects.end());
    objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
    std::vector<u32> order, chain;
    for (const u32 obj : objects) {
        chain.clear();
        u32 at = 0;
        for (bool ok = mt_objc_impl::map_get(rt->state_heads, obj, &at); ok && at != 0;
             at = rt->states[at - 1].next) {
            chain.push_back(at - 1);
        }
        order.insert(order.end(), chain.rbegin(), chain.rend());
    }
    return order;
}

bool save_states(mt_cpu *cpu, const mt_objc *rt, Writer &w, std::string *why) {
    Writer body;
    u32 count = 0;
    for (const u32 i : states_in_order(rt)) {
        const State &s = rt->states[i];
        const StateOps *ops = ops_for(s.type);
        if (ops == nullptr) {
            continue; // a partial state (the caller checked the census)
        }
        Writer payload;
        if (!ops->save(cpu, s.obj, s.data, payload)) {
            *why =
                std::string("the ") + ops->name + " state of " + hex32(s.obj) + " cannot be saved";
            return false;
        }
        body.str(ops->name);
        body.u32(s.obj);
        body.u32(static_cast<u32>(payload.bytes().size()));
        body.raw(payload.bytes().data(), payload.bytes().size());
        count++;
    }
    w.u32(count);
    w.raw(body.bytes().data(), body.bytes().size());
    return true;
}

struct LoadedState {
    const StateOps *ops;
    u32 obj;
    void *data;
};

void free_loaded(std::vector<LoadedState> &states) {
    for (LoadedState &s : states) {
        if (s.ops->drop != nullptr) {
            s.ops->drop(s.data);
        }
        std::free(s.data);
    }
    states.clear();
}

bool read_states(mt_cpu *cpu, Reader &r, std::vector<LoadedState> *out) {
    u32 n = 0;
    if (!r.u32(&n) || !r.count_fits(n, 12)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        std::string name;
        u32 obj = 0, len = 0;
        if (!r.str(&name) || !r.u32(&obj) || !r.u32(&len)) {
            return false;
        }
        const StateOps *ops = ops_named(name.c_str());
        if (ops == nullptr) {
            return r.fail("host state type " + name + " has no serializer in this port");
        }
        if (obj == 0) {
            return r.fail("a " + name + " state of nil");
        }
        const std::uint8_t *payload = r.raw(len);
        if (payload == nullptr) {
            return false;
        }
        void *data = std::calloc(1, ops->type->size != 0 ? ops->type->size : 1);
        if (data == nullptr) {
            return r.fail("out of memory");
        }
        Reader pr(payload, len, name + " state of " + hex32(obj));
        if (!ops->load(cpu, obj, data, pr) || !pr.done()) {
            // A load that failed half-way may hold host memory: drop frees what it set.
            if (ops->drop != nullptr) {
                ops->drop(data);
            }
            std::free(data);
            return r.fail(pr.error().empty() ? name + " state malformed" : pr.error());
        }
        out->push_back(LoadedState{ops, obj, data});
    }
    return r.done();
}

void drop_live_states(mt_objc *rt) {
    for (u32 i = 0; i < rt->state_count; i++) {
        State &s = rt->states[i];
        if (s.type != nullptr) {
            const StateOps *ops = ops_for(s.type);
            if (ops != nullptr && ops->drop != nullptr) {
                ops->drop(s.data);
            }
            std::free(s.data);
        }
        s = State{0, nullptr, nullptr, 0};
    }
    rt->state_count = 0;
    rt->state_free = 0;
    mt_objc_impl::map_clear(rt->state_heads);
}

bool attach_state(mt_objc *rt, u32 obj, const mt_objc_state_type *type, void *data) {
    u32 head = 0;
    mt_objc_impl::map_get(rt->state_heads, obj, &head);
    const long index = mt_objc_impl::push(rt->states, rt->state_count, rt->state_cap,
                                          State{obj, type, data, head});
    return index >= 0 && mt_objc_impl::map_put(rt->state_heads, obj, static_cast<u32>(index) + 1);
}

// --- the process ---------------------------------------------------------------------------

void save_process(Writer &w) {
    u32 n = 0;
    const mt_atexit_entry *e = mt_process_atexit_entries(mt_process_current(), &n);
    w.u32(n);
    for (u32 i = 0; i < n; i++) {
        w.u32(e[i].func);
        w.u32(e[i].arg);
        w.u32(e[i].dso);
        w.u32(e[i].done);
    }
}

bool read_process(Reader &r, std::vector<mt_atexit_entry> *out) {
    u32 n = 0;
    if (!r.u32(&n) || !r.count_fits(n, 16)) {
        return false;
    }
    out->resize(n);
    for (mt_atexit_entry &e : *out) {
        r.u32(&e.func);
        r.u32(&e.arg);
        r.u32(&e.dso);
        r.u32(&e.done);
    }
    return r.done();
}

// --- the census ----------------------------------------------------------------------------

std::vector<const char *> missing_types(const mt_objc *rt) {
    std::vector<const mt_objc_state_type *> seen;
    std::vector<const char *> names;
    for (u32 i = 0; i < rt->state_count; i++) {
        const mt_objc_state_type *t = rt->states[i].type;
        if (t == nullptr || ops_for(t) != nullptr ||
            std::find(seen.begin(), seen.end(), t) != seen.end()) {
            continue;
        }
        seen.push_back(t);
        if (std::find_if(names.begin(), names.end(), [t](const char *n) {
                return std::strcmp(n, t->name) == 0;
            }) == names.end()) {
            names.push_back(t->name);
        }
    }
    return names;
}

std::string join(const std::vector<const char *> &names) {
    std::string s;
    for (const char *n : names) {
        s += (s.empty() ? "" : ", ") + std::string(n);
    }
    return s;
}

bool open_file_handle(const mt_objc *rt) {
    for (u32 i = 0; i < rt->state_count; i++) {
        if (rt->states[i].type != nullptr &&
            std::strcmp(rt->states[i].type->name, "NSFileHandle") == 0) {
            return true;
        }
    }
    return false;
}

// --- the staged restore ----------------------------------------------------------------------

struct Staged {
    CpuRegs cpu{};
    Pages heap_pages;
    HeapBook book;
    std::vector<Pages> image, stacks;
    ObjcTables objc;
    std::vector<LoadedState> states;
    std::vector<std::function<void(mt_cpu *)>> modules;
    std::vector<mt_atexit_entry> atexit;
    ~Staged() { free_loaded(states); }
};

const Section *need(const std::vector<Section> &sections, const char *name, std::string *why) {
    const Section *s = find_section(sections, name);
    if (s == nullptr) {
        *why = std::string("the state has no ") + name + " section";
    }
    return s;
}

std::vector<const mt_segment *> writable_segments(mt_mem *mem) {
    u32 n = 0;
    const mt_segment *segs = mt_mem_segments(mem, &n);
    std::vector<const mt_segment *> out;
    for (u32 i = 0; i < n; i++) {
        if ((segs[i].initprot & MT_PROT_WRITE) != 0 && segs[i].vmsize != 0) {
            out.push_back(&segs[i]);
        }
    }
    return out;
}

// Reads every section into `st`, checking it against this process. Touches nothing live.
bool stage(mt_cpu *cpu, mt_objc *rt, const std::vector<Section> &sections, Staged *st,
           std::string *why) {
    mt_mem *mem = mt_mem_from_cpu(cpu);
    const auto reader = [](const Section &s) {
        return Reader(s.bytes.data(), s.bytes.size(), "section " + s.name);
    };
    const Section *s = need(sections, "cpu", why);
    if (s == nullptr) {
        return false;
    }
    Reader rc = reader(*s);
    if (!read_cpu(rc, &st->cpu)) {
        *why = rc.error();
        return false;
    }
    if (st->cpu.r[MT_SP] != cpu->r[MT_SP]) {
        *why = "the state's stack pointer differs from the turn hook's (not taken at a turn hook)";
        return false;
    }
    if ((s = need(sections, "sjlj", why)) == nullptr) {
        return false;
    }
    Reader rj = reader(*s);
    SjljChain saved;
    if (!rj.u32(&saved.top) || !rj.u32(&saved.caught) || !rj.words(&saved.frames) || !rj.done()) {
        *why = rj.error();
        return false;
    }
    if (!(saved == sjlj_chain(cpu))) {
        *why = "the unwinder's chain differs from this turn's";
        return false;
    }
    if ((s = need(sections, "image", why)) == nullptr) {
        return false;
    }
    Reader ri = reader(*s);
    const std::vector<const mt_segment *> segs = writable_segments(mem);
    u32 n = 0;
    if (!ri.u32(&n) || n != segs.size()) {
        *why = ri.error().empty() ? "the state's image segments differ from this process's"
                                  : ri.error();
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        Pages p;
        if (!read_pages(ri, &p)) {
            *why = ri.error();
            return false;
        }
        if (p.lo != segs[i]->vmaddr || p.hi != segs[i]->vmaddr + segs[i]->vmsize) {
            *why = "the state's image segments differ from this process's";
            return false;
        }
        st->image.push_back(std::move(p));
    }
    if (!ri.done()) {
        *why = ri.error();
        return false;
    }
    if ((s = need(sections, "heap.book", why)) == nullptr) {
        return false;
    }
    Reader rb = reader(*s);
    if (!read_heap_book(rb, &st->book)) {
        *why = rb.error();
        return false;
    }
    if ((s = need(sections, "heap", why)) == nullptr) {
        return false;
    }
    Reader rh = reader(*s);
    if (!read_pages(rh, &st->heap_pages) || !rh.done()) {
        *why = rh.error();
        return false;
    }
    if (st->heap_pages.lo != MT_GUEST_HEAP_BASE || st->heap_pages.hi != st->book.committed) {
        *why = "the heap's pages do not match its bookkeeping";
        return false;
    }
    if ((s = need(sections, "stacks", why)) == nullptr) {
        return false;
    }
    Reader rs = reader(*s);
    {
        std::lock_guard<std::mutex> lock(mem->stacks_mu);
        if (!rs.u32(&n) || n != mem->stacks.size()) {
            *why = rs.error().empty() ? "the state's guest stacks differ from this process's"
                                      : rs.error();
            return false;
        }
        for (u32 i = 0; i < n; i++) {
            Pages p;
            u32 guard = 0;
            if (!rs.u32(&guard) || !read_pages(rs, &p)) {
                *why = rs.error();
                return false;
            }
            const mt::mem::Stack &k = mem->stacks[i];
            if (guard != k.guard || p.lo != k.base || p.hi != k.top) {
                *why = "the state's guest stacks differ from this process's";
                return false;
            }
            st->stacks.push_back(std::move(p));
        }
    }
    if (!rs.done()) {
        *why = rs.error();
        return false;
    }
    if ((s = need(sections, "objc", why)) == nullptr) {
        return false;
    }
    Reader ro = reader(*s);
    if (!read_objc(rt, ro, &st->objc)) {
        *why = ro.error();
        return false;
    }
    const std::vector<const char *> missing = missing_types(rt);
    if (!missing.empty()) {
        *why = "live host states without a serializer: " + join(missing);
        return false;
    }
    if ((s = need(sections, "objc.states", why)) == nullptr) {
        return false;
    }
    Reader rt_states = reader(*s);
    if (!read_states(cpu, rt_states, &st->states)) {
        *why = rt_states.error();
        return false;
    }
    std::size_t nmod = 0;
    const Module *const *mods = all_modules(&nmod);
    for (std::size_t i = 0; i < nmod; i++) {
        if ((s = need(sections, mods[i]->section, why)) == nullptr) {
            return false;
        }
        Reader rm = reader(*s);
        std::function<void(mt_cpu *)> apply;
        if (!mods[i]->load(cpu, rm, &apply) || !rm.done()) {
            *why = rm.error().empty() ? "section " + s->name + " malformed" : rm.error();
            return false;
        }
        st->modules.push_back(std::move(apply));
    }
    if ((s = need(sections, "process", why)) == nullptr) {
        return false;
    }
    Reader rp = reader(*s);
    if (!read_process(rp, &st->atexit)) {
        *why = rp.error();
        return false;
    }
    return true;
}

// Puts the staged state in place. Only fails when the host runs out of memory half-way.
bool apply(mt_cpu *cpu, mt_objc *rt, Staged &st) {
    mt_mem *mem = mt_mem_from_cpu(cpu);
    bool ok = true;
    drop_live_states(rt);
    for (const Pages &p : st.image) {
        apply_pages(cpu->mem, p);
    }
    {
        std::lock_guard<std::mutex> lock(mem->heap.mu);
        const u32 committed = mem->heap.committed;
        if (st.book.committed > committed) {
            ok = mt::mem::commit_range(mem, committed, st.book.committed);
        } else if (committed > st.book.committed) {
            // Never committed in the saved run: zero, as a fresh commit would be.
            std::memset(cpu->mem + st.book.committed, 0, committed - st.book.committed);
        }
        if (ok) {
            apply_heap_book(mem, st.book);
            apply_pages(cpu->mem, st.heap_pages);
        }
    }
    for (const Pages &p : st.stacks) {
        apply_pages(cpu->mem, p);
    }
    ok = ok && apply_objc(rt, st.objc);
    for (LoadedState &s : st.states) {
        if (ok && attach_state(rt, s.obj, s.ops->type, s.data)) {
            s.data = nullptr;
        } else {
            ok = false;
        }
    }
    st.states.erase(std::remove_if(st.states.begin(), st.states.end(),
                                   [](const LoadedState &s) { return s.data == nullptr; }),
                    st.states.end());
    for (const auto &f : st.modules) {
        f(cpu);
    }
    if (mt_process *p = mt_process_current(); p != nullptr) {
        ok = mt_process_set_atexit_entries(p, st.atexit.data(),
                                           static_cast<u32>(st.atexit.size())) == 0 &&
             ok;
    }
    std::memcpy(cpu->r, st.cpu.r, sizeof cpu->r);
    cpu->cpsr = st.cpu.cpsr;
    cpu->fpscr = st.cpu.fpscr;
    std::memcpy(cpu->d, st.cpu.d, sizeof cpu->d);
    // Links between host states, now that every state and guest byte is back (#203).
    for (u32 i = 0; i < rt->state_count; i++) {
        const State &s = rt->states[i];
        const StateOps *ops = s.type != nullptr ? ops_for(s.type) : nullptr;
        if (ops != nullptr && ops->link != nullptr) {
            ops->link(cpu, s.obj, s.data);
        }
    }
    return ok;
}

} // namespace

} // namespace mt_snap

using namespace mt_snap;

extern "C" mt_snapshot_config mt_snapshot_config_default(void) noexcept {
    return mt_snapshot_config{0x11AA00u, 0x108C64u, 1};
}

extern "C" void mt_snapshot_configure(const mt_snapshot_config *config) noexcept {
    g_config = config != nullptr ? *config : mt_snapshot_config_default();
}

namespace {

// The safe-point check. A restore (`restoring`) skips the input conditions: the state it puts
// back has no touch down, no key held and nothing queued (a save refuses otherwise), and its
// input and touch modules replace the present's, releasing what the player holds now (#203).
int safe_point(mt_cpu *cpu, char *why, std::size_t why_size, bool restoring) {
    const auto refuse = [why, why_size](const std::string &reason) {
        set_why(why, why_size, reason);
        return 0;
    };
    mt_objc *rt = mt_objc_current();
    if (cpu == nullptr || rt == nullptr) {
        return refuse("no game is running");
    }
    if (mt_runloop_in_turn() != 0) {
        return refuse("inside a run-loop turn");
    }
    std::int32_t v = 0;
    if (g_config.game_mode != 0) {
        if (!read_i32(cpu, g_config.game_mode, &v)) {
            return refuse("the game mode cannot be read");
        }
        if (v == 9 || v == 10) {
            return refuse("during a scene");
        }
        if (v != 0) {
            return refuse("not in the field (game mode " + std::to_string(v) + ")");
        }
    }
    if (g_config.popup != 0) {
        if (!read_i32(cpu, g_config.popup, &v)) {
            return refuse("the popup cannot be read");
        }
        if (v != -1) {
            return refuse("a popup is open");
        }
    }
    if (g_config.check_panel != 0 && mt_input_panel() != 0) {
        return refuse("not in the field (the key pad shows another panel)");
    }
    if (!restoring && mt_uikit_touches_active() != 0) {
        return refuse("a touch is down");
    }
    for (u32 a = 1; !restoring && a < MT_ACTION_FIRST_PORT; a++) {
        if (mt_input_held(a) != 0) {
            return refuse("a key is held");
        }
    }
    if (!restoring && mt_input_get_stats().queued != 0) {
        return refuse("input is waiting");
    }
    if (mt_uikit_animation_count(mt_cf_absolute_time()) != 0) {
        return refuse("an animation is running");
    }
    if (mt_gcd_idle() == 0) {
        return refuse("background work is running (GCD)");
    }
    if (open_file_handle(rt)) {
        return refuse("a file is open");
    }
    if (rt->pool_count != 0) {
        return refuse("autoreleased objects are waiting");
    }
    return 1;
}

} // namespace

extern "C" int mt_snapshot_safe(mt_cpu *cpu, char *why, std::size_t why_size) noexcept {
    return safe_point(cpu, why, why_size, false);
}

extern "C" mt_snapshot_meta mt_snapshot_meta_default(void) noexcept {
    mt_snapshot_meta m;
    std::memset(&m, 0, sizeof m);
    m.slot = MT_SNAPSHOT_ANY_SLOT;
    return m;
}

extern "C" mt_snapshot_expect mt_snapshot_expect_default(void) noexcept {
    mt_snapshot_expect e;
    std::memset(&e, 0, sizeof e);
    e.slot = MT_SNAPSHOT_ANY_SLOT;
    return e;
}

// mt_snapshot_save, which may throw std::bad_alloc.
static int save_state(mt_cpu *cpu, const mt_snapshot_meta *meta, std::uint8_t **out,
                      std::size_t *size, char *why, std::size_t why_size) {
    const mt_snapshot_meta m = meta != nullptr ? *meta : mt_snapshot_meta_default();
    *out = nullptr;
    *size = 0;
    if (mt_snapshot_safe(cpu, why, why_size) == 0) {
        return -1;
    }
    mt_objc *rt = mt_objc_current();
    const std::vector<const char *> missing = missing_types(rt);
    if (!missing.empty() && (m.flags & MT_SNAPSHOT_PARTIAL) == 0) {
        set_why(why, why_size, "live host states without a serializer: " + join(missing));
        return -1;
    }
    mt_mem *mem = mt_mem_from_cpu(cpu);
    std::vector<Section> sections;
    const auto add = [&sections](const char *name, Writer &w) {
        sections.push_back(Section{name, std::move(w.bytes())});
    };
    Writer w_cpu, w_sjlj, w_image, w_heap, w_book, w_stacks, w_objc, w_states, w_process;
    save_cpu(cpu, w_cpu);
    const SjljChain chain = sjlj_chain(cpu);
    w_sjlj.u32(chain.top);
    w_sjlj.u32(chain.caught);
    w_sjlj.words(chain.frames);
    const std::vector<const mt_segment *> segs = writable_segments(mem);
    w_image.u32(static_cast<u32>(segs.size()));
    for (const mt_segment *s : segs) {
        save_pages(w_image, cpu->mem, s->vmaddr, s->vmaddr + s->vmsize);
    }
    {
        std::lock_guard<std::mutex> lock(mem->heap.mu);
        save_pages(w_heap, cpu->mem, MT_GUEST_HEAP_BASE, mem->heap.committed);
        save_heap_book(mem, w_book);
    }
    {
        std::lock_guard<std::mutex> lock(mem->stacks_mu);
        w_stacks.u32(static_cast<u32>(mem->stacks.size()));
        for (const mt::mem::Stack &k : mem->stacks) {
            w_stacks.u32(k.guard);
            save_pages(w_stacks, cpu->mem, k.base, k.top);
        }
    }
    save_objc(rt, w_objc);
    std::string reason;
    if (!save_states(cpu, rt, w_states, &reason)) {
        set_why(why, why_size, reason);
        return -1;
    }
    save_process(w_process);
    add("cpu", w_cpu);
    add("sjlj", w_sjlj);
    add("image", w_image);
    add("heap", w_heap);
    add("heap.book", w_book);
    add("stacks", w_stacks);
    add("objc", w_objc);
    add("objc.states", w_states);
    add("process", w_process);
    std::size_t nmod = 0;
    const Module *const *mods = all_modules(&nmod);
    for (std::size_t i = 0; i < nmod; i++) {
        Writer w;
        if (!mods[i]->save(cpu, w, &reason)) {
            set_why(why, why_size, reason);
            return -1;
        }
        add(mods[i]->section, w);
    }
    mt_snapshot_header h;
    std::memset(&h, 0, sizeof h);
    h.format = MT_SNAPSHOT_FORMAT;
    std::memcpy(h.binary_sha256, m.binary_sha256, 32);
    std::snprintf(h.port_version, sizeof h.port_version, "%s", MT_VERSION);
    h.slot = m.slot;
    h.flags = m.flags & (MT_SNAPSHOT_STORE | MT_SNAPSHOT_PARTIAL);
    h.guest_unix_us = guest_unix_us();
    std::vector<std::uint8_t> file;
    if (!write_state_file(h, sections, (m.flags & MT_SNAPSHOT_STORE) != 0, &file, &reason)) {
        set_why(why, why_size, reason);
        return -1;
    }
    auto *bytes = static_cast<std::uint8_t *>(std::malloc(file.size()));
    if (bytes == nullptr) {
        set_why(why, why_size, "out of memory");
        return -1;
    }
    std::memcpy(bytes, file.data(), file.size());
    *out = bytes;
    *size = file.size();
    return 0;
}

extern "C" int mt_snapshot_save(mt_cpu *cpu, const mt_snapshot_meta *meta, std::uint8_t **out,
                                std::size_t *size, char *why, std::size_t why_size) noexcept {
    *out = nullptr;
    *size = 0;
    try {
        return save_state(cpu, meta, out, size, why, why_size);
    } catch (const std::bad_alloc &) {
        set_why(why, why_size, "out of memory");
        return -1;
    }
}

extern "C" int mt_snapshot_read_header(const std::uint8_t *bytes, std::size_t size,
                                       const mt_snapshot_expect *expect, mt_snapshot_header *header,
                                       char *why, std::size_t why_size) noexcept {
    std::string reason;
    mt_snapshot_header h{};
    try {
        if (!read_state_file(bytes, size, expect, &h, nullptr, &reason)) {
            set_why(why, why_size, reason);
            return -1;
        }
    } catch (const std::bad_alloc &) {
        set_why(why, why_size, "out of memory while reading the state");
        return -1;
    }
    if (header != nullptr) {
        *header = h;
    }
    return 0;
}

extern "C" int mt_snapshot_restore(mt_cpu *cpu, const std::uint8_t *bytes, std::size_t size,
                                   const mt_snapshot_expect *expect, mt_snapshot_header *header,
                                   char *why, std::size_t why_size) noexcept {
    if (safe_point(cpu, why, why_size, true) == 0) {
        return -1;
    }
    mt_objc *rt = mt_objc_current();
    std::string reason;
    mt_snapshot_header h{};
    std::vector<Section> sections;
    Staged st;
    // Reading and staging change nothing, so an allocation that fails there is one more refusal
    // (#413, ADR 0154): std::bad_alloc must not leave this noexcept function.
    try {
        if (!read_state_file(bytes, size, expect, &h, &sections, &reason)) {
            set_why(why, why_size, reason);
            return -1;
        }
        if (!stage(cpu, rt, sections, &st, &reason)) {
            set_why(why, why_size, reason);
            return -1;
        }
    } catch (const std::bad_alloc &) {
        set_why(why, why_size, "out of memory while reading the state: nothing changed");
        return -1;
    }
    bool applied = false;
    try {
        applied = apply(cpu, rt, st);
    } catch (const std::bad_alloc &) {
        applied = false;
    }
    if (!applied) {
        set_why(why, why_size, "out of memory while restoring: the game is in an unknown state");
        return -1;
    }
    if (expect != nullptr && expect->rebase_clock != nullptr) {
        expect->rebase_clock(expect->user, h.guest_unix_us);
    }
    if (header != nullptr) {
        *header = h;
    }
    return 0;
}

extern "C" std::uint32_t mt_snapshot_missing(const mt_objc *rt, const char **names,
                                             std::uint32_t max) noexcept {
    if (rt == nullptr) {
        return 0;
    }
    const std::vector<const char *> missing = missing_types(rt);
    for (std::size_t i = 0; i < missing.size() && i < max; i++) {
        names[i] = missing[i];
    }
    return static_cast<std::uint32_t>(missing.size());
}

extern "C" int mt_snapshot_has_serializer(const char *name) noexcept {
    return name != nullptr && ops_named(name) != nullptr ? 1 : 0;
}
