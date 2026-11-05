// The Objective-C runtime's services (ADR 0029) on a real guest address space (#42's heap
// and a guest stack) and synthetic image metadata: NSObject (alloc, retain counts, dealloc,
// the NSObject protocol methods), +initialize, autorelease pools, properties, @synchronized,
// the blocks ABI, the example host class of maplethief/objc/host.h, undeliverable messages
// and start-up through the import tables. Guest IMPs are test functions that do what the
// compiled code does (objc_msgSendSuper2 for [super ...], _Block_object_assign in a copy
// helper, ...), reached through objc_test.cpp's mt_dispatch.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "objc_test.h"

namespace {

using u32 = std::uint32_t;

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

std::vector<std::string> g_calls; // what the test IMPs saw, in order
u32 g_next_imp = 0x00F00001u;     // fake guest code addresses (Thumb bit set)

// Registers a test function as a guest IMP; returns its fake address.
u32 imp(mt_guest_fn fn) {
    const u32 at = g_next_imp;
    g_next_imp += 0x10u;
    objc_test::add_imp(at, fn);
    return at;
}

struct Methods {
    std::vector<std::pair<const char *, u32>> list; // selector, IMP
};

// A guest address space with the runtime's host classes and a builder for image metadata.
struct World {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 at = 0, end = 0; // metadata bump region (a heap block)
    u32 nsobject = 0, nsobject_meta = 0;
    std::vector<u32> classlist, catlist;

    explicit World(u32 policy = MT_UNIMPLEMENTED_STOP, bool host_classes = true) {
        g_log.clear();
        g_calls.clear();
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        at = mt_heap_malloc(mem, 0x10000u);
        REQUIRE(at != 0u);
        end = at + 0x10000u;
        mt_import_config cfg = mt_import_config_default();
        cfg.unimplemented = policy;
        cfg.log = capture;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        if (host_classes) {
            REQUIRE(mt_objc_add_host_classes(rt) == 0u);
            nsobject = mt_objc_class_named(rt, "NSObject");
            nsobject_meta = mt_objc_metaclass(rt, nsobject);
        }
    }
    ~World() {
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    World(const World &) = delete;
    World &operator=(const World &) = delete;

    void put32(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    u32 bytes(u32 n) {
        const u32 p = (at + 3u) & ~3u;
        at = p + n;
        REQUIRE(at <= end);
        return p;
    }
    u32 str(const char *s) {
        const u32 n = static_cast<u32>(std::strlen(s)) + 1u;
        const u32 p = bytes(n);
        std::memcpy(cpu.mem + p, s, n);
        return p;
    }
    u32 words(std::initializer_list<u32> ws) {
        const u32 p = bytes(4u * static_cast<u32>(ws.size()));
        u32 i = 0;
        for (u32 w : ws) {
            put32(p + 4u * i++, w);
        }
        return p;
    }
    u32 methods(const Methods &ms) {
        if (ms.list.empty()) {
            return 0;
        }
        std::vector<u32> names; // the strings first: the list must be contiguous
        for (const auto &m : ms.list) {
            names.push_back(str(m.first));
        }
        const u32 types = str("v8@0:4");
        const u32 p = words({12u | 3u, static_cast<u32>(ms.list.size())});
        for (std::size_t i = 0; i < ms.list.size(); i++) {
            words({names[i], types, ms.list[i].second});
        }
        return p;
    }
    // protocol_t { isa, name, protocols, ... } and protocol_list_t { count, list... }
    u32 protocol(const char *name, u32 adopted = 0) {
        return words({0u, str(name), adopted, 0u, 0u, 0u, 0u, 0u});
    }
    u32 protocol_list(std::initializer_list<u32> ps) {
        const u32 p = words({static_cast<u32>(ps.size())});
        for (u32 x : ps) {
            words({x});
        }
        return p;
    }
    // An image class and its metaclass; `super` is the (class, metaclass) of its superclass.
    std::pair<u32, u32> klass(const char *name, std::pair<u32, u32> super, u32 size,
                              const Methods &inst = {}, const Methods &cls = {},
                              u32 protocols = 0) {
        const u32 name_at = str(name);
        const u32 mro = words({1u, 20u, 20u, 0u, name_at, methods(cls), 0u, 0u, 0u, 0u});
        const u32 ro = words({0u, 4u, size, 0u, name_at, methods(inst), protocols, 0u, 0u, 0u});
        const u32 meta = words({nsobject_meta, super.second, 0u, 0u, mro});
        const u32 c = words({meta, super.first, 0u, 0u, ro});
        classlist.push_back(c);
        return {c, meta};
    }
    std::pair<u32, u32> root() const { return {nsobject, nsobject_meta}; }
    void category(const char *name, u32 cls, const Methods &inst = {}, u32 protocols = 0) {
        catlist.push_back(words({str(name), cls, methods(inst), 0u, protocols, 0u}));
    }
    u32 load() {
        mt_objc_image img{};
        img.classlist = words({});
        for (u32 c : classlist) {
            words({c});
        }
        img.classlist_count = static_cast<u32>(classlist.size());
        img.catlist = at;
        for (u32 c : catlist) {
            words({c});
        }
        img.catlist_count = static_cast<u32>(catlist.size());
        return mt_objc_load_image(rt, &img);
    }

    u32 sel(const char *name) { return mt_objc_sel(rt, name); }
    // [obj name args...] from host code.
    u32 send(u32 obj, const char *name, std::initializer_list<u32> args = {}) {
        return mt_objc_msg(&cpu, obj, sel(name), args.begin(), static_cast<u32>(args.size()));
    }
    u32 live_size(u32 obj) { return mt_heap_usable_size(mem, obj); }
};

World *g_world = nullptr; // for the test IMPs

// What compiled code does for [super name a2 a3] in a method of class `cls`.
u32 guest_super(mt_cpu *cpu, u32 self, u32 cls, const char *name, u32 a2 = 0, u32 a3 = 0) {
    const u32 lr = cpu->r[MT_LR], sp = cpu->r[MT_SP];
    cpu->r[MT_SP] = sp - 8u;
    g_world->put32(sp - 8u, self);
    g_world->put32(sp - 4u, cls);
    cpu->r[0] = sp - 8u;
    cpu->r[1] = mt_objc_sel(mt_objc_current(), name);
    cpu->r[2] = a2;
    cpu->r[3] = a3;
    cpu->r[MT_LR] = 0x7001u; // a return address inside the guest method
    mt_hle_objc_msgSendSuper2(cpu, 0);
    cpu->r[MT_LR] = lr;
    cpu->r[MT_SP] = sp;
    return cpu->r[0];
}

// Calls an HLE import with word arguments, those past r3 on the guest stack.
u32 call_hle(mt_cpu *cpu, mt_import_fn fn, std::initializer_list<u32> args) {
    const u32 lr = cpu->r[MT_LR], sp = cpu->r[MT_SP];
    u32 i = 0;
    const u32 stacked = args.size() > 4 ? static_cast<u32>(args.size()) - 4u : 0u;
    cpu->r[MT_SP] = sp - 8u * ((stacked + 1u) / 2u);
    for (u32 a : args) {
        if (i < 4) {
            cpu->r[i] = a;
        } else {
            g_world->put32(cpu->r[MT_SP] + 4u * (i - 4u), a);
        }
        i++;
    }
    cpu->r[MT_LR] = 0x7003u;
    fn(cpu, 0);
    cpu->r[MT_LR] = lr;
    cpu->r[MT_SP] = sp;
    return cpu->r[0];
}

// --- test IMPs ---------------------------------------------------------------------------

u32 g_thing = 0, g_sub = 0, g_sub_meta = 0;

void thing_dealloc(mt_cpu *cpu) {
    g_calls.push_back("-[Thing dealloc]");
    guest_super(cpu, cpu->r[0], g_thing, "dealloc");
}

void thing_copy_with_zone(mt_cpu *cpu) {
    g_calls.push_back("-[Thing copyWithZone:]");
    cpu->r[0] = g_world->send(g_thing, "new"); // a new object, retain count 1
}

void record_initialize(mt_cpu *cpu) {
    g_calls.push_back(std::string("+initialize ") +
                      mt_objc_class_name(mt_objc_current(), cpu->r[0]));
}

void sub_alloc_with_zone(mt_cpu *cpu) {
    g_calls.push_back("+[Sub allocWithZone:]");
    cpu->r[0] = guest_super(cpu, cpu->r[0], g_sub_meta, "allocWithZone:", cpu->r[2]);
}

void record_args(mt_cpu *cpu) {
    g_calls.push_back("make: r2=" + std::to_string(cpu->r[2]) + " r3=" + std::to_string(cpu->r[3]));
}

// -[Leaky dealloc]: autoreleases another object while a pool is being drained.
u32 g_leaky_extra = 0, g_leaky = 0;
void leaky_dealloc(mt_cpu *cpu) {
    g_calls.push_back("-[Leaky dealloc]");
    const u32 self = cpu->r[0];
    mt_objc_autorelease(cpu, g_leaky_extra);
    guest_super(cpu, self, g_leaky, "dealloc");
}

void two_args(mt_cpu *cpu) {
    g_calls.push_back("two:and: " + std::to_string(cpu->r[2]) + " " + std::to_string(cpu->r[3]));
    cpu->r[0] = cpu->r[2] + cpu->r[3];
}

// Blocks: copy and dispose helpers as clang writes them for a block capturing an object (at
// +20) and a __block variable (at +24), and an invoke function adding its argument to the
// __block int.
void block_copy_helper(mt_cpu *cpu) {
    const u32 dst = cpu->r[0], src = cpu->r[1];
    g_calls.push_back("copy helper");
    call_hle(cpu, mt_hle_Block_object_assign, {dst + 20u, g_world->get32(src + 20u), 3u});
    call_hle(cpu, mt_hle_Block_object_assign, {dst + 24u, g_world->get32(src + 24u), 8u});
}
void block_dispose_helper(mt_cpu *cpu) {
    const u32 b = cpu->r[0];
    g_calls.push_back("dispose helper");
    call_hle(cpu, mt_hle_Block_object_dispose, {g_world->get32(b + 20u), 3u});
    call_hle(cpu, mt_hle_Block_object_dispose, {g_world->get32(b + 24u), 8u});
}
void block_invoke(mt_cpu *cpu) {
    const u32 byref = g_world->get32(cpu->r[0] + 24u);
    const u32 shared = g_world->get32(byref + 4u); // forwarding
    cpu->r[0] = g_world->get32(shared + 16u) + cpu->r[1];
}
// A __block id variable's keep / destroy helpers (BLOCK_BYREF_CALLER | object).
void byref_keep(mt_cpu *cpu) {
    g_calls.push_back("byref keep");
    call_hle(cpu, mt_hle_Block_object_assign,
             {cpu->r[0] + 24u, g_world->get32(cpu->r[1] + 24u), 128u | 3u});
}
void byref_destroy(mt_cpu *cpu) {
    g_calls.push_back("byref destroy");
    call_hle(cpu, mt_hle_Block_object_dispose, {g_world->get32(cpu->r[0] + 24u), 128u | 3u});
}

// --- the example host class of maplethief/objc/host.h -----------------------------------------

struct CounterState {
    std::int32_t total;
    u32 delegate; // a retained guest object
};

u32 g_destroyed = 0;
void counter_destroy(mt_cpu *cpu, u32 obj, void *state) {
    auto *s = static_cast<CounterState *>(state);
    g_destroyed = obj;
    mt_objc_release(cpu, s->delegate);
}
const mt_objc_state_type k_counter_state = {"Counter", sizeof(CounterState), counter_destroy};

// -addValue:(int)v times:(int)n -> int
void counter_add(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::int32_t v = mt_arg_i32(&a), n = mt_arg_i32(&a);
    auto *s = static_cast<CounterState *>(
        mt_objc_state(mt_objc_current(), mt_objc_self(cpu), &k_counter_state));
    s->total += v * n;
    mt_ret_u32(cpu, static_cast<u32>(s->total));
}

// -sumA:b:c:d: -> int: two arguments in r2-r3, two on the stack.
void counter_sum4(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    u32 sum = 0;
    for (int i = 0; i < 4; i++) {
        sum += mt_arg_u32(&a);
    }
    mt_ret_u32(cpu, sum);
}

// -frame -> CGRect (16 bytes, objc_msgSend_stret)
void counter_frame(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    const float rect[4] = {0.0f, 0.0f, 320.0f, static_cast<float>(self & 0xFFu)};
    mt_ret_struct(cpu, result, rect, sizeof rect);
}

// -setDelegate:(id) keeps it retained in the host state; -poke sends -two:and: to it from host
// code and returns the answer.
void counter_set_delegate(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 self = mt_objc_self(cpu);
    const u32 delegate = mt_arg_u32(&a);
    auto *s = static_cast<CounterState *>(mt_objc_state(mt_objc_current(), self, &k_counter_state));
    mt_objc_retain(cpu, delegate);
    mt_objc_release(cpu, s->delegate);
    s->delegate = delegate;
}
void counter_poke(mt_cpu *cpu) {
    const auto *s = static_cast<const CounterState *>(
        mt_objc_find_state(mt_objc_current(), mt_objc_self(cpu), &k_counter_state));
    mt_ret_u32(cpu, s != nullptr ? mt_objc_send2(cpu, s->delegate, "two:and:", 40u, 2u) : 0u);
}

const mt_objc_host_method k_counter_methods[] = {
    {"addValue:times:", counter_add},       {"sumA:b:c:d:", counter_sum4}, {"frame", counter_frame},
    {"setDelegate:", counter_set_delegate}, {"poke", counter_poke},
};
const mt_objc_host_class k_counter_class = {"Counter",
                                            "NSObject",
                                            4,
                                            k_counter_methods,
                                            sizeof k_counter_methods / sizeof k_counter_methods[0],
                                            nullptr,
                                            0};

} // namespace

TEST_CASE("NSObject: +alloc on the guest heap, retain counts, -release -> -dealloc frees") {
    World w;
    g_world = &w;
    const auto thing = w.klass("Thing", w.root(), 12, {{{"dealloc", imp(thing_dealloc)}}});
    g_thing = thing.first;
    REQUIRE(w.load() == 0u);

    const u32 obj = w.send(thing.first, "alloc");
    REQUIRE(obj != 0u);
    CHECK(obj % MT_HEAP_ALIGN == 0u);
    CHECK(w.live_size(obj) >= 12u); // the class's instance size, on #42's heap
    CHECK(w.get32(obj) == thing.first);
    CHECK(w.get32(obj + 4) == 0u);
    CHECK(w.get32(obj + 8) == 0u);
    CHECK(w.send(obj, "init") == obj);
    CHECK(w.send(obj, "retainCount") == 1u);
    CHECK(w.send(obj, "retain") == obj);
    CHECK(mt_objc_retain_count(w.rt, obj) == 2u);
    w.send(obj, "release");
    CHECK(mt_objc_retain_count(w.rt, obj) == 1u);
    CHECK(g_calls.empty());
    w.send(obj, "release"); // the last reference: -[Thing dealloc] -> -[NSObject dealloc]
    REQUIRE(g_calls.size() == 1u);
    CHECK(g_calls[0] == "-[Thing dealloc]");
    CHECK(w.live_size(obj) == 0u); // freed
    CHECK(w.get32(obj) == 0u);     // a later message to it is one to no known class

    // +new is [[self alloc] init]; class objects are never retained or released.
    const u32 other = w.send(thing.first, "new");
    CHECK(w.get32(other) == thing.first);
    CHECK(w.send(thing.first, "retain") == thing.first);
    w.send(thing.first, "release");
    CHECK(mt_objc_retain_count(w.rt, thing.first) == 0xFFFFFFFFu);
    CHECK(w.send(thing.first, "class") == thing.first);
    CHECK(g_log.empty());
}

TEST_CASE("+alloc goes through +allocWithZone:; +initialize runs once, superclass first") {
    World w;
    g_world = &w;
    const u32 init = imp(record_initialize);
    const auto base = w.klass("Base", w.root(), 4, {}, {{{"initialize", init}}});
    const auto sub = w.klass(
        "Sub", base, 8, {}, {{{"initialize", init}, {"allocWithZone:", imp(sub_alloc_with_zone)}}});
    const auto other = w.klass("Other", base, 4, {}, {{{"make:", imp(record_args)}}});
    g_sub = sub.first;
    g_sub_meta = sub.second;
    REQUIRE(w.load() == 0u);

    const u32 obj = w.send(sub.first, "alloc");
    CHECK(w.get32(obj) == sub.first);
    REQUIRE(g_calls.size() == 3u);
    CHECK(g_calls[0] == "+initialize Base");
    CHECK(g_calls[1] == "+initialize Sub");
    CHECK(g_calls[2] == "+[Sub allocWithZone:]");
    w.send(obj, "init");
    w.send(sub.first, "alloc");
    CHECK(g_calls.size() == 4u); // no second +initialize

    // A class without its own +initialize gets the inherited one, with itself as self (as in
    // libobjc), and the message that triggered it keeps its arguments.
    g_calls.clear();
    w.send(other.first, "make:", {0x77u, 0x88u});
    REQUIRE(g_calls.size() == 2u);
    CHECK(g_calls[0] == "+initialize Other");
    CHECK(g_calls[1] == "make: r2=119 r3=136");
}

TEST_CASE("autorelease pools: the run loop's push/pop, NSAutoreleasePool objects, nesting") {
    World w;
    g_world = &w;
    const auto thing = w.klass("Thing", w.root(), 8, {{{"dealloc", imp(thing_dealloc)}}});
    g_thing = thing.first;
    const auto leaky = w.klass("Leaky", w.root(), 8, {{{"dealloc", imp(leaky_dealloc)}}});
    g_leaky = leaky.first;
    REQUIRE(w.load() == 0u);

    // No pool: logged, leaked.
    const u32 lost = w.send(thing.first, "new");
    CHECK(w.send(lost, "autorelease") == lost);
    REQUIRE(g_log.size() == 1u);
    CHECK(g_log[0].find("autoreleased with no pool in place - just leaking") != std::string::npos);

    // A run-loop turn.
    const u32 turn = mt_objc_pool_push(w.rt);
    const u32 a = w.send(thing.first, "new");
    w.send(a, "autorelease");
    w.send(w.send(a, "retain"), "autorelease"); // twice in the pool, retain count 2
    CHECK(mt_objc_pool_pending(w.rt) == 2u);
    // A guest pool inside it, drained by the guest.
    const u32 pool =
        w.send(w.send(mt_objc_class_named(w.rt, "NSAutoreleasePool"), "alloc"), "init");
    const u32 b = w.send(thing.first, "new");
    w.send(b, "autorelease");
    w.send(pool, "drain");
    CHECK(w.live_size(b) == 0u);
    CHECK(w.live_size(pool) == 0u);
    CHECK(w.live_size(a) != 0u);
    // A guest pool never drained, and an object whose -dealloc autoreleases another one: the
    // outer pop releases them all.
    w.send(w.send(mt_objc_class_named(w.rt, "NSAutoreleasePool"), "alloc"), "init");
    g_leaky_extra = w.send(thing.first, "new");
    const u32 l = w.send(leaky.first, "new");
    w.send(l, "autorelease");
    g_calls.clear();
    mt_objc_pool_pop(w.rt, turn);
    CHECK(mt_objc_pool_pending(w.rt) == 0u);
    CHECK(w.live_size(a) == 0u);
    CHECK(w.live_size(l) == 0u);
    CHECK(w.live_size(g_leaky_extra) == 0u);
    // Newest first; the object autoreleased by -[Leaky dealloc] goes in the same pop, and `a`
    // (in the pool twice) is deallocated at its second release.
    REQUIRE(g_calls.size() == 3u);
    CHECK(g_calls[0] == "-[Leaky dealloc]");
    CHECK(g_calls[1] == "-[Thing dealloc]");
    CHECK(g_calls[2] == "-[Thing dealloc]");
    CHECK(g_log.size() == 1u);
    mt_objc_pool_pop(w.rt, turn); // popped already
    CHECK(g_log.back().find("not in place") != std::string::npos);
}

TEST_CASE("NSObject protocol methods are answered from the runtime's tables") {
    World w;
    g_world = &w;
    const u32 p0 = w.protocol("P0");
    const u32 p1 = w.protocol("P1", w.protocol_list({p0}));
    const u32 p2 = w.protocol("P2");
    const u32 p3 = w.protocol("P3");
    const auto thing =
        w.klass("Thing", w.root(), 8,
                {{{"copyWithZone:", imp(thing_copy_with_zone)}, {"two:and:", imp(two_args)}}}, {},
                w.protocol_list({p1}));
    g_thing = thing.first;
    const auto sub = w.klass("Sub", thing, 8);
    w.category("Extra", thing.first, {}, w.protocol_list({p2}));
    REQUIRE(w.load() == 0u);
    const u32 obj = w.send(sub.first, "new");

    CHECK(w.send(obj, "class") == sub.first);
    CHECK(w.send(obj, "superclass") == thing.first);
    CHECK(w.send(sub.first, "superclass") == thing.first);
    CHECK(w.send(obj, "isKindOfClass:", {thing.first}) == 1u);
    CHECK(w.send(obj, "isKindOfClass:", {w.nsobject}) == 1u);
    CHECK(w.send(obj, "isKindOfClass:", {mt_objc_class_named(w.rt, "NSAutoreleasePool")}) == 0u);
    CHECK(w.send(sub.first, "isKindOfClass:", {w.nsobject}) == 1u); // via the root metaclass
    CHECK(w.send(obj, "isMemberOfClass:", {sub.first}) == 1u);
    CHECK(w.send(obj, "isMemberOfClass:", {thing.first}) == 0u);
    CHECK(w.send(obj, "respondsToSelector:", {w.sel("two:and:")}) == 1u);
    CHECK(w.send(obj, "respondsToSelector:", {w.sel("retain")}) == 1u);
    CHECK(w.send(obj, "respondsToSelector:", {w.sel("nope")}) == 0u);
    CHECK(w.send(sub.first, "respondsToSelector:", {w.sel("alloc")}) == 1u);
    CHECK(w.send(sub.first, "respondsToSelector:", {w.sel("two:and:")}) == 0u);
    CHECK(w.send(sub.first, "instancesRespondToSelector:", {w.sel("two:and:")}) == 1u);
    CHECK(w.send(sub.first, "isSubclassOfClass:", {thing.first}) == 1u);
    CHECK(w.send(thing.first, "isSubclassOfClass:", {sub.first}) == 0u);
    // Protocols: adopted, inherited through a protocol and a superclass, from a category.
    CHECK(w.send(obj, "conformsToProtocol:", {p1}) == 1u);
    CHECK(w.send(obj, "conformsToProtocol:", {p0}) == 1u);
    CHECK(w.send(obj, "conformsToProtocol:", {p2}) == 1u);
    CHECK(w.send(obj, "conformsToProtocol:", {p3}) == 0u);
    CHECK(w.send(sub.first, "conformsToProtocol:", {p0}) == 1u);
    CHECK(w.send(w.send(w.nsobject, "new"), "conformsToProtocol:", {p0}) == 0u);
    // -performSelector:withObject:withObject: (the second object on the stack).
    CHECK(w.send(obj, "performSelector:withObject:withObject:", {w.sel("two:and:"), 5u, 6u}) ==
          11u);
    CHECK(g_calls.back() == "two:and: 5 6");
    CHECK(w.send(obj, "performSelector:withObject:", {w.sel("two:and:"), 7u}) == 7u);
    CHECK(w.send(obj, "isEqual:", {obj}) == 1u);
    CHECK(w.send(obj, "isEqual:", {obj + 16u}) == 0u);
    CHECK(w.send(obj, "hash") == obj);
    // -copy is -copyWithZone:.
    const u32 copy = w.send(obj, "copy");
    CHECK(g_calls.back() == "-[Thing copyWithZone:]");
    CHECK(w.get32(copy) == thing.first);
    CHECK(g_log.empty());
}

TEST_CASE("objc_getProperty / objc_setProperty: retain, copy, atomic") {
    World w;
    g_world = &w;
    const auto thing =
        w.klass("Thing", w.root(), 12, {{{"copyWithZone:", imp(thing_copy_with_zone)}}});
    g_thing = thing.first;
    REQUIRE(w.load() == 0u);
    const u32 owner = w.send(thing.first, "new");
    const u32 v1 = w.send(thing.first, "new");
    const u32 v2 = w.send(thing.first, "new");
    const u32 cmd = w.sel("setValue:");

    // A nonatomic retain setter: objc_setProperty(self, _cmd, 8, value, NO, NO).
    call_hle(&w.cpu, mt_hle_objc_setProperty, {owner, cmd, 8u, v1, 0u, 0u});
    CHECK(w.get32(owner + 8) == v1);
    CHECK(mt_objc_retain_count(w.rt, v1) == 2u);
    call_hle(&w.cpu, mt_hle_objc_setProperty, {owner, cmd, 8u, v2, 0u, 0u});
    CHECK(w.get32(owner + 8) == v2);
    CHECK(mt_objc_retain_count(w.rt, v1) == 1u);
    CHECK(mt_objc_retain_count(w.rt, v2) == 2u);
    // Setting the same value again keeps the count.
    call_hle(&w.cpu, mt_hle_objc_setProperty, {owner, cmd, 8u, v2, 1u, 0u});
    CHECK(mt_objc_retain_count(w.rt, v2) == 2u);
    // A copy setter stores [value copyWithZone:NULL].
    call_hle(&w.cpu, mt_hle_objc_setProperty, {owner, cmd, 8u, v1, 0u, 1u});
    const u32 copied = w.get32(owner + 8);
    CHECK(copied != v1);
    CHECK(w.get32(copied) == thing.first);
    CHECK(g_calls.back() == "-[Thing copyWithZone:]");
    CHECK(mt_objc_retain_count(w.rt, v2) == 1u);
    CHECK(mt_objc_retain_count(w.rt, v1) == 1u);
    // nil.
    call_hle(&w.cpu, mt_hle_objc_setProperty, {owner, cmd, 8u, 0u, 0u, 0u});
    CHECK(w.get32(owner + 8) == 0u);
    CHECK(w.live_size(copied) == 0u); // released to 0

    // Getters: nonatomic returns the ivar; atomic returns [[value retain] autorelease].
    call_hle(&w.cpu, mt_hle_objc_setProperty, {owner, cmd, 4u, v1, 0u, 0u});
    CHECK(call_hle(&w.cpu, mt_hle_objc_getProperty, {owner, cmd, 4u, 0u}) == v1);
    CHECK(mt_objc_retain_count(w.rt, v1) == 2u);
    const u32 turn = mt_objc_pool_push(w.rt);
    CHECK(call_hle(&w.cpu, mt_hle_objc_getProperty, {owner, cmd, 4u, 1u}) == v1);
    CHECK(mt_objc_retain_count(w.rt, v1) == 3u);
    CHECK(mt_objc_pool_pending(w.rt) == 1u);
    mt_objc_pool_pop(w.rt, turn);
    CHECK(mt_objc_retain_count(w.rt, v1) == 2u);
    CHECK(g_log.empty());
}

TEST_CASE("@synchronized: a recursive count per object; objc_enumerationMutation stops") {
    World w;
    g_world = &w;
    const u32 obj = w.send(w.nsobject, "new");
    const u32 other = w.send(w.nsobject, "new");
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_enter, {obj}) == 0u);
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_enter, {obj}) == 0u); // recursive
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_enter, {other}) == 0u);
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_exit, {obj}) == 0u);
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_exit, {obj}) == 0u);
    CHECK(g_log.empty());
    // Exit without enter: OBJC_SYNC_NOT_OWNING_THREAD_ERROR, logged.
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_exit, {obj}) == 0xFFFFFFFFu);
    REQUIRE(g_log.size() == 1u);
    CHECK(g_log[0].find("objc_sync_exit(0x") != std::string::npos);
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_exit, {other}) == 0u);
    // nil: nothing.
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_enter, {0u}) == 0u);
    CHECK(call_hle(&w.cpu, mt_hle_objc_sync_exit, {0u}) == 0u);

    struct Arg {
        World *w;
        u32 obj;
    } arg{&w, obj};
    CHECK(objc_test::stops(
        [](void *p) {
            auto *a = static_cast<Arg *>(p);
            a->w->cpu.r[0] = a->obj;
            a->w->cpu.r[MT_LR] = 0x4321u;
            mt_hle_objc_enumerationMutation(&a->w->cpu, 0);
        },
        &arg));
    CHECK(objc_test::g_trap[0] == MT_TRAP_OBJC);
    CHECK(objc_test::g_trap[1] == 0x4321u);
    CHECK(objc_test::g_trap[2] == obj);
    CHECK(g_log.back().find("mutated while being enumerated") != std::string::npos);
    w.cpu.r[MT_LR] = 0x2345u;
}

TEST_CASE("blocks: _Block_copy with copy/dispose helpers, objects, __block variables") {
    World w;
    g_world = &w;
    const u32 stack_cls = mt_objc_class_named(w.rt, "__NSStackBlock__");
    const u32 malloc_cls = mt_objc_class_named(w.rt, "__NSMallocBlock__");
    const u32 global_cls = mt_objc_class_named(w.rt, "__NSGlobalBlock__");
    REQUIRE((stack_cls != 0u && malloc_cls != 0u && global_cls != 0u));
    constexpr u32 has_copy_dispose = 1u << 25, needs_free = 1u << 24, is_global = 1u << 28;

    const u32 captured = w.send(w.nsobject, "new");
    // A __block int (no helpers) and a __block id (keep/destroy helpers), on the stack.
    const u32 byref = w.words({0u, 0u, 0u, 20u, 42u});
    w.put32(byref + 4, byref); // forwarding
    const u32 byref_obj =
        w.words({0u, 0u, has_copy_dispose, 28u, imp(byref_keep), imp(byref_destroy), captured});
    w.put32(byref_obj + 4, byref_obj);
    // ^(int x){ return captured, n + x; } on the stack: isa, flags, reserved, invoke,
    // descriptor, captured object, byref.
    const u32 descriptor = w.words({0u, 28u, imp(block_copy_helper), imp(block_dispose_helper)});
    const u32 block =
        w.words({stack_cls, has_copy_dispose, 0u, imp(block_invoke), descriptor, captured, byref});

    // -retain of a stack block does not copy it.
    CHECK(w.send(block, "retain") == block);
    const u32 heap = mt_block_copy(&w.cpu, block);
    REQUIRE(heap != 0u);
    CHECK(heap != block);
    CHECK(w.live_size(heap) >= 28u);
    CHECK(w.get32(heap) == malloc_cls);
    CHECK(w.get32(heap + 4) == (has_copy_dispose | needs_free | 1u));
    CHECK(w.get32(heap + 12) == w.get32(block + 12));
    CHECK(mt_objc_retain_count(w.rt, captured) == 2u); // retained by the copy helper
    const u32 shared = w.get32(heap + 24);
    CHECK(shared != byref);              // the __block int moved to the heap
    CHECK(w.get32(byref + 4) == shared); // the stack copy forwards to it
    CHECK(w.get32(shared + 4) == shared);
    CHECK(w.get32(shared + 16) == 42u);              // its value copied
    CHECK(w.get32(shared + 8) == (needs_free | 2u)); // the stack frame's and the block's

    // Messages: -copy and -retain add a reference, -release drops one.
    CHECK(w.send(heap, "copy") == heap);
    CHECK(w.send(heap, "retain") == heap);
    CHECK((w.get32(heap + 4) & 0xFFFFu) == 3u);
    w.send(heap, "release");
    w.send(heap, "release");
    // Invoke: n (through forwarding) + x.
    w.put32(shared + 16, 50u);
    const u32 x = 8u;
    CHECK(mt_block_invoke(&w.cpu, heap, &x, 1) == 58u);

    // The last release runs the dispose helper and frees the block.
    g_calls.clear();
    mt_block_release(&w.cpu, heap);
    REQUIRE(g_calls.size() == 1u);
    CHECK(g_calls[0] == "dispose helper");
    CHECK(w.live_size(heap) == 0u);
    CHECK(mt_objc_retain_count(w.rt, captured) == 1u);
    CHECK(w.live_size(shared) != 0u); // the stack frame still holds the __block variable
    // ... until its scope ends: the compiler disposes of it.
    call_hle(&w.cpu, mt_hle_Block_object_dispose, {byref, 8u});
    CHECK(w.live_size(shared) == 0u);

    // A __block id with keep/destroy helpers: kept by a plain assignment (BYREF_CALLER).
    const u32 slot = w.words({0u});
    call_hle(&w.cpu, mt_hle_Block_object_assign, {slot, byref_obj, 8u});
    const u32 shared_obj = w.get32(slot);
    CHECK(shared_obj != byref_obj);
    CHECK(w.get32(shared_obj + 24) == captured);
    CHECK(w.get32(shared_obj + 16) == w.get32(byref_obj + 16));
    CHECK(mt_objc_retain_count(w.rt, captured) == 1u);
    call_hle(&w.cpu, mt_hle_Block_object_assign, {slot, byref_obj, 8u}); // a second block
    CHECK(w.get32(slot) == shared_obj);
    g_calls.clear();
    call_hle(&w.cpu, mt_hle_Block_object_dispose, {shared_obj, 8u});
    call_hle(&w.cpu, mt_hle_Block_object_dispose, {shared_obj, 8u});
    CHECK(g_calls.empty());
    call_hle(&w.cpu, mt_hle_Block_object_dispose, {byref_obj, 8u}); // the stack frame's
    REQUIRE(g_calls.size() == 1u);
    CHECK(g_calls[0] == "byref destroy");
    CHECK(w.live_size(shared_obj) == 0u);

    // A block field (flags 7) is copied; _Block_copy / _Block_release as imports. (A new
    // stack frame: the __block int is on the stack again.)
    w.put32(byref + 4, byref);
    const u32 slot2 = w.words({0u});
    call_hle(&w.cpu, mt_hle_Block_object_assign, {slot2, block, 7u});
    const u32 heap2 = w.get32(slot2);
    CHECK(w.get32(heap2) == malloc_cls);
    CHECK(call_hle(&w.cpu, mt_hle_Block_copy, {heap2}) == heap2);
    call_hle(&w.cpu, mt_hle_Block_release, {heap2});
    call_hle(&w.cpu, mt_hle_Block_object_dispose, {heap2, 7u});
    CHECK(w.live_size(heap2) == 0u);

    // A global block is never copied or freed.
    const u32 global = w.words({global_cls, is_global, 0u, imp(block_invoke), descriptor});
    CHECK(mt_block_copy(&w.cpu, global) == global);
    CHECK(w.send(global, "copy") == global);
    w.send(global, "release");
    CHECK(w.get32(global + 4) == is_global);
    CHECK(mt_block_copy(&w.cpu, 0u) == 0u);
    CHECK(g_log.empty());
}

TEST_CASE("the example host class: arguments, stret, host state, sending from host code") {
    World w;
    g_world = &w;
    REQUIRE(mt_objc_add_host_class(w.rt, &k_counter_class, 0, 0) == 0);
    const u32 counter_cls = mt_objc_class_named(w.rt, "Counter");
    // An image subclass of the host class, and an image class as its delegate.
    const auto tally = w.klass("Tally", {counter_cls, mt_objc_metaclass(w.rt, counter_cls)}, 8);
    const auto thing = w.klass("Thing", w.root(), 8, {{{"two:and:", imp(two_args)}}});
    REQUIRE(w.load() == 0u);

    const u32 c = w.send(tally.first, "new");
    CHECK(w.send(c, "addValue:times:", {3u, 4u}) == 12u);
    CHECK(w.send(c, "addValue:times:", {static_cast<u32>(-1), 2u}) == 10u);
    // Four arguments: two in r2-r3, two on the guest stack (mt_objc_msg puts them below sp).
    const u32 sp = w.cpu.r[MT_SP];
    CHECK(w.send(c, "sumA:b:c:d:", {1u, 2u, 3u, 4u}) == 10u);
    CHECK(w.cpu.r[MT_SP] == sp);
    CHECK(w.cpu.r[MT_LR] == 0x2345u);
    // A structure result through objc_msgSend_stret.
    const u32 rect = w.words({0u, 0u, 0u, 0u});
    mt_objc_msg_stret(&w.cpu, rect, c, w.sel("frame"), nullptr, 0);
    float out[4];
    std::memcpy(out, w.cpu.mem + rect, sizeof out);
    CHECK(out[2] == 320.0f);
    CHECK(out[3] == static_cast<float>(c & 0xFFu));
    // The guest's own objc_msgSend_stret reaches it the same way.
    const u32 rect2 = w.words({0u, 0u, 0u, 0u});
    w.cpu.r[0] = rect2;
    w.cpu.r[1] = c;
    w.cpu.r[2] = w.sel("frame");
    mt_hle_objc_msgSend_stret(&w.cpu, 0);
    CHECK(w.get32(rect2 + 8) == w.get32(rect + 8));
    CHECK(w.get32(rect2 + 8) != 0u);
    // Host state holding a guest object; the host method sends to it.
    const u32 delegate = w.send(thing.first, "new");
    w.send(c, "setDelegate:", {delegate});
    CHECK(mt_objc_retain_count(w.rt, delegate) == 2u);
    CHECK(w.send(c, "poke") == 42u);
    CHECK(g_calls.back() == "two:and: 40 2");
    REQUIRE(mt_objc_find_state(w.rt, c, &k_counter_state) != nullptr);
    CHECK(static_cast<CounterState *>(mt_objc_find_state(w.rt, c, &k_counter_state))->total == 10);
    // The census of host states and reference tables (#190, ADR 0105): one Counter state, on c;
    // the delegate retained twice.
    struct Seen {
        u32 counters = 0, obj = 0;
    } seen;
    const auto census = [](void *user, u32 obj, const mt_objc_state_type *type, const void *st) {
        auto *s = static_cast<Seen *>(user);
        if (type == &k_counter_state) {
            s->counters++;
            s->obj = obj;
            CHECK(static_cast<const CounterState *>(st)->total == 10);
        }
    };
    mt_objc_each_state(w.rt, census, &seen);
    CHECK(seen.counters == 1u);
    CHECK(seen.obj == c);
    u32 retained = 0, autoreleased = 7;
    mt_objc_ref_counts(w.rt, &retained, &autoreleased);
    CHECK(retained >= 1u);
    CHECK(autoreleased == 0u);
    // dealloc destroys the state (which releases the delegate) and frees the object.
    g_destroyed = 0;
    w.send(c, "release");
    CHECK(g_destroyed == c);
    CHECK(mt_objc_find_state(w.rt, c, &k_counter_state) == nullptr);
    seen = Seen{};
    mt_objc_each_state(w.rt, census, &seen); // a freed state is not listed
    CHECK(seen.counters == 0u);
    CHECK(mt_objc_retain_count(w.rt, delegate) == 1u);
    CHECK(w.live_size(c) == 0u);
    CHECK(g_log.empty());
}

TEST_CASE("undeliverable messages are recorded per (class, selector) and reported to a hook") {
    World w(MT_UNIMPLEMENTED_CONTINUE);
    g_world = &w;
    std::vector<std::string> hooked;
    mt_objc_set_undeliverable_hook(
        w.rt,
        [](void *user, const mt_objc_undeliverable *m) {
            static_cast<std::vector<std::string> *>(user)->push_back(
                std::string(m->class_method ? "+" : "-") +
                (m->class_name != nullptr ? m->class_name : "?") + " " + m->selector + " " +
                std::to_string(m->count));
        },
        &hooked);
    const u32 obj = w.send(w.nsobject, "new");
    w.cpu.r[MT_LR] = 0x5555u;
    for (int i = 0; i < 3; i++) {
        w.cpu.r[0] = obj;
        w.cpu.r[1] = w.sel("frobnicate");
        mt_hle_objc_msgSend(&w.cpu, 0);
        CHECK(w.cpu.r[0] == 0u);
    }
    w.cpu.r[0] = w.nsobject;
    w.cpu.r[1] = w.sel("frobnicate");
    mt_hle_objc_msgSend(&w.cpu, 0);
    // Sent by host code on behalf of the guest caller 0x5555.
    w.send(obj, "twiddle");
    w.cpu.r[MT_LR] = 0x2345u;
    REQUIRE(mt_objc_undeliverable_count(w.rt) == 3u);
    mt_objc_undeliverable u{};
    REQUIRE(mt_objc_undeliverable_at(w.rt, 0, &u) == 0);
    CHECK(std::string(u.class_name) == "NSObject");
    CHECK(u.class_method == 0u);
    CHECK(std::string(u.selector) == "frobnicate");
    CHECK(u.count == 3u);
    CHECK(u.receiver == obj);
    CHECK(u.first_caller == 0x5555u);
    REQUIRE(mt_objc_undeliverable_at(w.rt, 1, &u) == 0);
    CHECK(u.class_method == 1u);
    REQUIRE(mt_objc_undeliverable_at(w.rt, 2, &u) == 0);
    CHECK(std::string(u.selector) == "twiddle");
    CHECK(u.first_caller == 0x5555u);
    CHECK(mt_objc_undeliverable_at(w.rt, 3, &u) == -1);
    CHECK(hooked == std::vector<std::string>{"-NSObject frobnicate 1", "-NSObject frobnicate 2",
                                             "-NSObject frobnicate 3", "+NSObject frobnicate 1",
                                             "-NSObject twiddle 1"});
    CHECK(g_log.size() == 3u); // "continue": each pair logged once
}

TEST_CASE("start-up: imported class objects, placeholders, overrides, block isa objects") {
    World w(MT_UNIMPLEMENTED_STOP, false);
    g_world = &w;
    // The data imports as mt_imports_place places them (zeroed objects), and a function.
    const char *names[] = {"_OBJC_CLASS_$_NSObject",
                           "_OBJC_METACLASS_$_NSObject",
                           "_OBJC_CLASS_$_MPMoviePlayerController",
                           "_OBJC_METACLASS_$_MPMoviePlayerController",
                           "_OBJC_CLASS_$_AVAudioRecorder",
                           "__NSConcreteStackBlock",
                           "_objc_msgSend"};
    constexpr u32 n = sizeof names / sizeof names[0];
    mt_import_desc descs[n];
    u32 addrs[n];
    for (u32 i = 0; i < n; i++) {
        const bool fn = i == n - 1;
        descs[i] = mt_import_desc{
            names[i],          "lib",         fn ? MT_IMPORT_FUNCTION : MT_IMPORT_DATA,
            fn ? 0x1000u : 0u, fn ? 0u : 20u, fn ? 0u : 4u};
        addrs[i] = fn ? 0x1000u : w.words({0u, 0u, 0u, 0u, 0u});
    }
    // The image: Widget : MPMoviePlayerController (a placeholder: no host class; ivars from 48),
    // Chartboost : NSObject with +sharedChartboost.
    w.nsobject = addrs[0];
    w.nsobject_meta = addrs[1];
    const auto widget = w.klass("Widget", {addrs[2], addrs[3]}, 56);
    w.put32(w.get32(widget.first + 16) + 4, 48u); // class_ro_t.instanceStart
    const auto cb =
        w.klass("Chartboost", w.root(), 8, {}, {{{"sharedChartboost", imp(thing_copy_with_zone)}}});
    mt_objc_image img{};
    img.classlist = w.words({widget.first, cb.first});
    img.classlist_count = 2;

    CHECK(mt_objc_start(w.rt, descs, n, addrs, &img) == 0u);
    const mt_objc_stats s = mt_objc_get_stats(w.rt);
    CHECK(s.image_classes == 2u);
    CHECK(s.placeholders == 2u); // MPMoviePlayerController, AVAudioRecorder
    CHECK(s.overrides == 1u);
    CHECK(s.instance_start_problems == 0u);
    // Host classes live at the imported objects, with libobjc's isa and superclass words.
    CHECK(mt_objc_class_named(w.rt, "NSObject") == addrs[0]);
    CHECK(mt_objc_metaclass(w.rt, addrs[0]) == addrs[1]);
    CHECK(w.get32(addrs[0]) == addrs[1]);
    CHECK(w.get32(addrs[1]) == addrs[1]);
    CHECK(w.get32(addrs[2]) == addrs[3]);
    CHECK(w.get32(addrs[2] + 4) == addrs[0]);
    CHECK(mt_objc_class_named(w.rt, "__NSStackBlock__") == addrs[5]);
    CHECK(mt_objc_class_is_placeholder(w.rt, addrs[2]) == 1);
    CHECK(mt_objc_class_is_placeholder(w.rt, addrs[0]) == 0);
    CHECK(mt_objc_instance_size(w.rt, addrs[2]) == 4u); // a placeholder of NSObject's size
    // UIView is UIKit's host class (#54) with the SDK's size.
    CHECK(mt_objc_class_is_placeholder(w.rt, mt_objc_class_named(w.rt, "UIView")) == 0);
    CHECK(mt_objc_instance_size(w.rt, mt_objc_class_named(w.rt, "UIView")) == 46u);
    CHECK(mt_objc_superclass(w.rt, widget.first) == addrs[2]);
    // A placeholder: alloc/init work (NSObject), anything else is unrecognized.
    const u32 view = w.send(widget.first, "new");
    CHECK(w.get32(view) == widget.first);
    // The override: no Chartboost code runs. StoreKit is a host class of the registry
    // (#52, ADR 0034), with a class object of its own when the image does not import it.
    g_calls.clear();
    CHECK(w.send(cb.first, "sharedChartboost") == 0u);
    CHECK(g_calls.empty());
    CHECK(mt_objc_class_is_placeholder(w.rt, addrs[4]) == 1);
    CHECK(w.send(mt_objc_class_named(w.rt, "SKPaymentQueue"), "canMakePayments") == 1u);
    REQUIRE(!g_log.empty());
    CHECK(g_log[0].find("2 imported classes have no host implementation yet") != std::string::npos);

    struct Arg {
        World *w;
        u32 view;
    } arg{&w, view};
    CHECK(objc_test::stops(
        [](void *p) {
            auto *a = static_cast<Arg *>(p);
            a->w->send(a->view, "setNeedsDisplay");
        },
        &arg));
    CHECK(objc_test::g_trap[0] == MT_TRAP_OBJC);
    CHECK(g_log.back().find("unrecognized selector -[Widget setNeedsDisplay]") !=
          std::string::npos);
}
