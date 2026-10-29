// NSObject and NSAutoreleasePool as host classes, with the object services behind them
// (ADR 0026, ADR 0029): allocation on the guest heap, retain counts and host state in side
// tables keyed by the guest object, dealloc, autorelease pools, and the NSObject protocol
// methods answered from the runtime's tables.
//
// Retain counts are manual (the game predates ARC). An object without an entry in
// rt->retains has retain count 1; the entry holds the count - 1. -release at count 1 marks
// the object as deallocating and sends -dealloc, so the subclasses' -dealloc runs and ends in
// -[NSObject dealloc], which frees the guest memory and every side-table entry.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "objc/objc_internal.h"

namespace mt_objc_impl {

namespace {

constexpr u32 k_deallocating = 0x80000000u; // in rt->retains: -dealloc was sent
constexpr u32 k_count_mask = 0x7FFFFFFFu;
constexpr u32 k_max_protocol_depth = 16;

u32 sel1(mt_objc *rt, const char *name) {
    const long id = sel_id_of(rt, name);
    return id >= 0 ? static_cast<u32>(id) + 1 : 0;
}

const char *class_name_of(const mt_objc *rt, u32 obj) {
    const long ci = object_class(rt, obj);
    return ci >= 0 ? rt->classes[ci].name : "?";
}

// --- retain counts ---------------------------------------------------------------------------

u32 retain_object(mt_objc *rt, u32 obj) {
    if (obj == 0 || is_class_object(rt, obj)) {
        return obj; // class objects are never deallocated
    }
    u32 n = 0;
    map_get(rt->retains, obj, &n);
    if ((n & k_count_mask) != k_count_mask) {
        n++;
    }
    map_put(rt->retains, obj, n);
    return obj;
}

// Drops one reference; returns true when the object must now be deallocated.
bool release_object(mt_objc *rt, u32 obj) {
    if (obj == 0 || is_class_object(rt, obj)) {
        return false;
    }
    u32 n = 0;
    if (!map_get(rt->retains, obj, &n)) {
        map_put(rt->retains, obj, k_deallocating);
        return true;
    }
    if ((n & k_count_mask) == 0) {
        return false; // already deallocating: a release from its own -dealloc
    }
    if (n == 1) {
        map_erase(rt->retains, obj);
    } else {
        map_put(rt->retains, obj, n - 1);
    }
    return false;
}

// --- autorelease pools -----------------------------------------------------------------------

void autorelease_add(mt_objc *rt, u32 obj) {
    if (obj == 0) {
        return;
    }
    if (rt->mark_count == 0) {
        char line[200];
        std::snprintf(line, sizeof line,
                      "objc: 0x%08X of class %s autoreleased with no pool in place - just leaking",
                      obj, class_name_of(rt, obj));
        log_line(rt, line);
        return;
    }
    if (push(rt->pool_objects, rt->pool_count, rt->pool_cap, obj) < 0) {
        log_line(rt, "objc: out of memory");
    }
}

// Pops every pool from index `depth` up, releasing their objects newest first.
void pop_pools(mt_cpu *cpu, mt_objc *rt, u32 depth) {
    if (depth >= rt->mark_count) {
        return;
    }
    const u32 start = rt->marks[depth].start;
    // Inner pools end here; what the releases autorelease lands in this pool and is released
    // in this loop too.
    rt->mark_count = depth + 1;
    while (rt->pool_count > start) {
        const u32 obj = rt->pool_objects[--rt->pool_count];
        mt_objc_release(cpu, obj);
    }
    rt->mark_count = depth;
}

// --- protocols -------------------------------------------------------------------------------

// Whether guest protocol_list_t `list` names protocol `name` (directly or through the
// protocols a listed protocol adopts).
bool list_has(const mt_objc *rt, u32 list, const char *name, u32 depth) {
    u32 count;
    if (list == 0 || depth > k_max_protocol_depth || !rd32(rt, list, &count) || count > 0x1000) {
        return false;
    }
    for (u32 i = 0; i < count; i++) {
        u32 proto, proto_name, inherited;
        if (!rd32(rt, list + 4 + 4 * i, &proto) || !rd32(rt, proto + 4, &proto_name)) {
            continue;
        }
        const char *s = guest_string(rt, proto_name);
        if (s != nullptr && std::strcmp(s, name) == 0) {
            return true;
        }
        if (rd32(rt, proto + 8, &inherited) && list_has(rt, inherited, name, depth + 1)) {
            return true;
        }
    }
    return false;
}

// Whether class index `ci` (a class, not a metaclass) or a superclass adopts protocol_t
// `proto` (compared by name: protocols are not uniqued here).
bool conforms(mt_objc *rt, long ci, u32 proto) {
    u32 name_addr;
    const char *name =
        proto != 0 && rd32(rt, proto + 4, &name_addr) ? guest_string(rt, name_addr) : nullptr;
    if (name == nullptr) {
        return false;
    }
    for (long k = ci, steps = 0; k >= 0 && steps <= static_cast<long>(rt->class_count);
         k = static_cast<long>(rt->classes[k].super) - 1, steps++) {
        const Class &c = rt->classes[k];
        if (list_has(rt, c.protocols, name, 0)) {
            return true;
        }
        for (u32 i = 0; i < c.cat_protocol_count; i++) {
            if (list_has(rt, c.cat_protocols[i], name, 0)) {
                return true;
            }
        }
    }
    return false;
}

// Whether class or metaclass object `cls` is `target` or inherits from it.
bool inherits(const mt_objc *rt, u32 cls, u32 target) {
    for (u32 k = cls, steps = 0; k != 0 && steps <= rt->class_count;
         k = mt_objc_superclass(rt, k), steps++) {
        if (k == target) {
            return true;
        }
    }
    return false;
}

// --- NSObject ------------------------------------------------------------------------------

#define MT_RT mt_objc *rt = mt_objc_current()

void ns_self(mt_cpu *) {} // -init, -self: r0 is self already

void ns_retain(mt_cpu *cpu) {
    MT_RT;
    cpu->r[0] = retain_object(rt, cpu->r[0]);
}

void ns_release(mt_cpu *cpu) {
    MT_RT;
    const u32 self = cpu->r[0];
    if (release_object(rt, self)) {
        host_send(cpu, rt, self, -1, sel1(rt, "dealloc"), nullptr, 0);
    }
}

void ns_autorelease(mt_cpu *cpu) {
    MT_RT;
    if (!is_class_object(rt, cpu->r[0])) {
        autorelease_add(rt, cpu->r[0]);
    }
}

void ns_retain_count(mt_cpu *cpu) {
    cpu->r[0] = mt_objc_retain_count(mt_objc_current(), cpu->r[0]);
}

void ns_dealloc(mt_cpu *cpu) { free_object(cpu, mt_objc_current(), cpu->r[0]); }

void ns_class(mt_cpu *cpu) { cpu->r[0] = mt_objc_class_of(mt_objc_current(), cpu->r[0]); }

void ns_superclass(mt_cpu *cpu) {
    MT_RT;
    cpu->r[0] = mt_objc_superclass(rt, mt_objc_class_of(rt, cpu->r[0]));
}

void ns_is_kind_of(mt_cpu *cpu) {
    MT_RT;
    cpu->r[0] = static_cast<u32>(mt_objc_is_kind_of(rt, cpu->r[0], cpu->r[2]));
}

void ns_is_member_of(mt_cpu *cpu) {
    MT_RT;
    cpu->r[0] = cpu->r[2] != 0 && mt_objc_class_of(rt, cpu->r[0]) == cpu->r[2] ? 1u : 0u;
}

void ns_responds(mt_cpu *cpu) {
    MT_RT;
    mt_objc_method m;
    const u32 cls = mt_objc_class_of(rt, cpu->r[0]);
    cpu->r[0] = cpu->r[2] != 0 && mt_objc_lookup(rt, cls, cpu->r[2], &m) != 0 ? 1u : 0u;
}

// -conformsToProtocol: and, through the root metaclass, +conformsToProtocol: (a class
// receiver: its own protocols).
void ns_conforms(mt_cpu *cpu) {
    MT_RT;
    long ci = object_class(rt, cpu->r[0]);
    if (ci >= 0 && rt->classes[ci].is_meta) {
        ci = static_cast<long>(rt->classes[ci].meta) - 1;
    }
    cpu->r[0] = ci >= 0 && conforms(rt, ci, cpu->r[2]) ? 1u : 0u;
}

// -performSelector:, -performSelector:withObject:, -performSelector:withObject:withObject:
void perform(mt_cpu *cpu, u32 n) {
    mt_args a = mt_objc_args(cpu);
    const u32 self = mt_objc_self(cpu);
    const u32 sel = mt_arg_u32(&a);
    u32 objects[2] = {0, 0};
    for (u32 i = 0; i < n; i++) {
        objects[i] = mt_arg_u32(&a);
    }
    mt_objc_msg(cpu, self, sel, objects, n);
}
void ns_perform0(mt_cpu *cpu) { perform(cpu, 0); }
void ns_perform1(mt_cpu *cpu) { perform(cpu, 1); }
void ns_perform2(mt_cpu *cpu) { perform(cpu, 2); }

void ns_is_equal(mt_cpu *cpu) { cpu->r[0] = cpu->r[0] == cpu->r[2] ? 1u : 0u; }

void ns_hash(mt_cpu *) {} // the address: r0 is self already

void ns_copy(mt_cpu *cpu) { cpu->r[0] = mt_objc_send1(cpu, cpu->r[0], "copyWithZone:", 0); }

void ns_mutable_copy(mt_cpu *cpu) {
    cpu->r[0] = mt_objc_send1(cpu, cpu->r[0], "mutableCopyWithZone:", 0);
}

void ns_nil(mt_cpu *cpu) { // -zone, -isProxy: NULL / NO
    cpu->r[0] = 0;
}

// +alloc is [self allocWithZone:NULL], so classes overriding +allocWithZone:
// (EAGLView_Instance's singleton) see every allocation.
void ns_alloc(mt_cpu *cpu) {
    MT_RT;
    cpu->r[0] = host_send(cpu, rt, cpu->r[0], -1, sel1(rt, "allocWithZone:"), nullptr, 0);
}

void ns_alloc_with_zone(mt_cpu *cpu) {
    cpu->r[0] = mt_objc_create_instance(mt_objc_current(), cpu->r[0]);
}

void ns_new(mt_cpu *cpu) {
    MT_RT;
    const u32 obj = host_send(cpu, rt, cpu->r[0], -1, sel1(rt, "alloc"), nullptr, 0);
    cpu->r[0] = host_send(cpu, rt, obj, -1, sel1(rt, "init"), nullptr, 0);
}

void ns_class_class(mt_cpu *) {} // +class, +self: r0 is the class already

void ns_class_superclass(mt_cpu *cpu) {
    cpu->r[0] = mt_objc_superclass(mt_objc_current(), cpu->r[0]);
}

void ns_initialize(mt_cpu *) {}

void ns_instances_respond(mt_cpu *cpu) {
    MT_RT;
    mt_objc_method m;
    cpu->r[0] = cpu->r[2] != 0 && mt_objc_lookup(rt, cpu->r[0], cpu->r[2], &m) != 0 ? 1u : 0u;
}

void ns_is_subclass_of(mt_cpu *cpu) {
    cpu->r[0] = cpu->r[2] != 0 && inherits(mt_objc_current(), cpu->r[0], cpu->r[2]) ? 1u : 0u;
}

const mt_objc_host_method k_nsobject_instance[] = {
    {"init", ns_self},
    {"self", ns_self},
    {"retain", ns_retain},
    {"release", ns_release},
    {"autorelease", ns_autorelease},
    {"retainCount", ns_retain_count},
    {"dealloc", ns_dealloc},
    {"class", ns_class},
    {"superclass", ns_superclass},
    {"isKindOfClass:", ns_is_kind_of},
    {"isMemberOfClass:", ns_is_member_of},
    {"respondsToSelector:", ns_responds},
    {"conformsToProtocol:", ns_conforms},
    {"performSelector:", ns_perform0},
    {"performSelector:withObject:", ns_perform1},
    {"performSelector:withObject:withObject:", ns_perform2},
    {"isEqual:", ns_is_equal},
    {"hash", ns_hash},
    {"copy", ns_copy},
    {"mutableCopy", ns_mutable_copy},
    {"zone", ns_nil},
    {"isProxy", ns_nil},
};

const mt_objc_host_method k_nsobject_class[] = {
    {"alloc", ns_alloc},
    {"allocWithZone:", ns_alloc_with_zone},
    {"new", ns_new},
    {"class", ns_class_class},
    {"self", ns_class_class},
    {"superclass", ns_class_superclass},
    {"initialize", ns_initialize},
    {"instancesRespondToSelector:", ns_instances_respond},
    {"isSubclassOfClass:", ns_is_subclass_of},
};

// --- NSAutoreleasePool -----------------------------------------------------------------------

void pool_init(mt_cpu *cpu) {
    MT_RT;
    const PoolMark mark{cpu->r[0], rt->pool_count};
    if (push(rt->marks, rt->mark_count, rt->mark_cap, mark) < 0) {
        log_line(rt, "objc: out of memory");
    }
}

// -drain, -release: pops this pool (and the pools inside it), then frees it.
void pool_drain(mt_cpu *cpu) {
    MT_RT;
    const u32 self = cpu->r[0];
    for (u32 i = rt->mark_count; i-- > 0;) {
        if (rt->marks[i].pool == self) {
            pop_pools(cpu, rt, i);
            free_object(cpu, rt, self);
            return;
        }
    }
    char line[160];
    std::snprintf(line, sizeof line,
                  "objc: NSAutoreleasePool 0x%08X drained, but it is no pool in place", self);
    log_line(rt, line);
}

void pool_misuse(mt_cpu *cpu) {
    MT_RT;
    char line[160];
    std::snprintf(line, sizeof line, "objc: -[NSAutoreleasePool %s] of 0x%08X: pools cannot be %s",
                  mt_objc_sel_name(rt, cpu->r[1]), cpu->r[0], "retained or autoreleased (ignored)");
    log_line(rt, line);
}

// -addObject: and +addObject: add to the innermost pool.
void pool_add(mt_cpu *cpu) { autorelease_add(mt_objc_current(), cpu->r[2]); }

const mt_objc_host_method k_pool_instance[] = {
    {"init", pool_init},     {"drain", pool_drain},        {"release", pool_drain},
    {"retain", pool_misuse}, {"autorelease", pool_misuse}, {"addObject:", pool_add},
};
const mt_objc_host_method k_pool_class[] = {{"addObject:", pool_add}};

#undef MT_RT

} // namespace

const mt_objc_host_class k_nsobject = {
    "NSObject",
    nullptr,
    4,
    k_nsobject_instance,
    sizeof k_nsobject_instance / sizeof k_nsobject_instance[0],
    k_nsobject_class,
    sizeof k_nsobject_class / sizeof k_nsobject_class[0],
};

const mt_objc_host_class k_nsautoreleasepool = {
    "NSAutoreleasePool",
    "NSObject",
    4,
    k_pool_instance,
    sizeof k_pool_instance / sizeof k_pool_instance[0],
    k_pool_class,
    sizeof k_pool_class / sizeof k_pool_class[0],
};

void free_object(mt_cpu *cpu, mt_objc *rt, u32 obj) {
    if (obj == 0 || is_class_object(rt, obj)) {
        return;
    }
    // Host states, newest first; each entry leaves the table before its destructor runs.
    u32 head;
    while (map_get(rt->state_heads, obj, &head)) {
        const u32 index = head - 1;
        const State s = rt->states[index];
        if (s.next != 0) {
            map_put(rt->state_heads, obj, s.next);
        } else {
            map_erase(rt->state_heads, obj);
        }
        rt->states[index] = State{0, nullptr, nullptr, rt->state_free};
        rt->state_free = index + 1;
        if (s.type->destroy != nullptr) {
            s.type->destroy(cpu, obj, s.data);
        }
        std::free(s.data);
    }
    map_erase(rt->retains, obj);
    map_erase(rt->syncs, obj);
    char line[200];
    std::snprintf(line, sizeof line, "objc: dealloc of 0x%08X (class %s): no heap block", obj,
                  class_name_of(rt, obj));
    wr32(rt, obj, 0); // a message to a freed object is one to no known class
    if (rt->free_fn != nullptr && rt->free_fn(rt->alloc_user, obj) != 0) {
        log_line(rt, line);
    }
}

void free_services(mt_objc *rt) {
    for (u32 i = 0; i < rt->state_count; i++) {
        std::free(rt->states[i].data);
    }
    std::free(rt->states);
    std::free(rt->pool_objects);
    std::free(rt->marks);
    map_free(rt->state_heads);
    map_free(rt->retains);
    map_free(rt->syncs);
}

} // namespace mt_objc_impl

using namespace mt_objc_impl;

// --- objects ---------------------------------------------------------------------------------

extern "C" u32 mt_objc_retain(mt_cpu *cpu, u32 obj) noexcept {
    mt_objc *rt = mt_objc_current();
    return rt != nullptr ? host_send(cpu, rt, obj, -1, sel1(rt, "retain"), nullptr, 0) : 0;
}

extern "C" void mt_objc_release(mt_cpu *cpu, u32 obj) noexcept {
    mt_objc *rt = mt_objc_current();
    if (rt != nullptr) {
        host_send(cpu, rt, obj, -1, sel1(rt, "release"), nullptr, 0);
    }
}

extern "C" u32 mt_objc_autorelease(mt_cpu *cpu, u32 obj) noexcept {
    mt_objc *rt = mt_objc_current();
    return rt != nullptr ? host_send(cpu, rt, obj, -1, sel1(rt, "autorelease"), nullptr, 0) : 0;
}

extern "C" u32 mt_objc_create_instance(mt_objc *rt, u32 cls) noexcept {
    const long ci = class_index(rt, cls);
    if (ci < 0 || rt->classes[ci].is_meta || rt->alloc == nullptr) {
        return 0;
    }
    const u32 size = rt->classes[ci].instance_size < 4 ? 4u : rt->classes[ci].instance_size;
    const u32 obj = rt->alloc(rt->alloc_user, size, 16);
    if (obj == 0 || static_cast<std::uint64_t>(obj) + size > rt->cpu->mem_size) {
        char line[160];
        std::snprintf(line, sizeof line, "objc: no guest memory for an instance of %s (%u bytes)",
                      rt->classes[ci].name, size);
        log_line(rt, line);
        return 0;
    }
    wr32(rt, obj, cls);
    return obj;
}

extern "C" u32 mt_objc_class_of(const mt_objc *rt, u32 obj) noexcept {
    u32 isa = 0;
    return obj != 0 && rd32(rt, obj, &isa) ? isa : 0;
}

extern "C" int mt_objc_is_kind_of(const mt_objc *rt, u32 obj, u32 cls) noexcept {
    return cls != 0 && inherits(rt, mt_objc_class_of(rt, obj), cls) ? 1 : 0;
}

extern "C" u32 mt_objc_retain_count(const mt_objc *rt, u32 obj) noexcept {
    if (obj == 0) {
        return 0;
    }
    if (is_class_object(rt, obj)) {
        return 0xFFFFFFFFu; // UINT_MAX, as for any object that is never deallocated
    }
    u32 n = 0;
    map_get(rt->retains, obj, &n);
    return (n & k_count_mask) + 1;
}

// --- host state --------------------------------------------------------------------------------

extern "C" void *mt_objc_find_state(const mt_objc *rt, u32 obj,
                                    const mt_objc_state_type *type) noexcept {
    u32 at;
    for (bool ok = map_get(rt->state_heads, obj, &at); ok && at != 0;
         at = rt->states[at - 1].next) {
        if (rt->states[at - 1].type == type) {
            return rt->states[at - 1].data;
        }
    }
    return nullptr;
}

extern "C" void *mt_objc_state(mt_objc *rt, u32 obj, const mt_objc_state_type *type) noexcept {
    void *found = mt_objc_find_state(rt, obj, type);
    if (found != nullptr || obj == 0) {
        return found;
    }
    void *data = std::calloc(1, type->size != 0 ? type->size : 1);
    if (data == nullptr) {
        return nullptr;
    }
    u32 head = 0;
    map_get(rt->state_heads, obj, &head);
    const State s{obj, type, data, head};
    long index;
    if (rt->state_free != 0) {
        index = static_cast<long>(rt->state_free) - 1;
        rt->state_free = rt->states[index].next;
        rt->states[index] = s;
    } else {
        index = push(rt->states, rt->state_count, rt->state_cap, s);
    }
    if (index < 0 || !map_put(rt->state_heads, obj, static_cast<u32>(index) + 1)) {
        std::free(data);
        return nullptr;
    }
    return data;
}

// --- autorelease pools ---------------------------------------------------------------------

extern "C" u32 mt_objc_pool_push(mt_objc *rt) noexcept {
    const PoolMark mark{0, rt->pool_count};
    if (push(rt->marks, rt->mark_count, rt->mark_cap, mark) < 0) {
        log_line(rt, "objc: out of memory");
        return 0;
    }
    return rt->mark_count;
}

extern "C" void mt_objc_pool_pop(mt_objc *rt, u32 token) noexcept {
    if (token == 0 || token > rt->mark_count) {
        char line[120];
        std::snprintf(line, sizeof line, "objc: autorelease pool %u popped, but it is not in place",
                      token);
        log_line(rt, line);
        return;
    }
    pop_pools(rt->cpu, rt, token - 1);
}

extern "C" void mt_objc_pool_pop_on(mt_cpu *cpu, mt_objc *rt, u32 token) noexcept {
    if (token == 0 || token > rt->mark_count) {
        char line[120];
        std::snprintf(line, sizeof line, "objc: autorelease pool %u popped, but it is not in place",
                      token);
        log_line(rt, line);
        return;
    }
    pop_pools(cpu, rt, token - 1);
}

extern "C" u32 mt_objc_pool_pending(const mt_objc *rt) noexcept { return rt->pool_count; }

// --- per-thread pool stacks (GCD worker threads, #50, ADR 0033) -------------------------------

struct mt_objc_pools {
    u32 *objects;
    u32 count, cap;
    PoolMark *marks;
    u32 mark_count, mark_cap;
};

extern "C" mt_objc_pools *mt_objc_pools_create(void) noexcept {
    return static_cast<mt_objc_pools *>(std::calloc(1, sizeof(mt_objc_pools)));
}

extern "C" void mt_objc_pools_destroy(mt_objc_pools *pools) noexcept {
    if (pools != nullptr) {
        std::free(pools->objects);
        std::free(pools->marks);
        std::free(pools);
    }
}

extern "C" void mt_objc_pools_swap(mt_objc *rt, mt_objc_pools *pools) noexcept {
    std::swap(rt->pool_objects, pools->objects);
    std::swap(rt->pool_count, pools->count);
    std::swap(rt->pool_cap, pools->cap);
    std::swap(rt->marks, pools->marks);
    std::swap(rt->mark_count, pools->mark_count);
    std::swap(rt->mark_cap, pools->mark_cap);
}
