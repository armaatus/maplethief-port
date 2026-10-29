// Internals shared by the Objective-C runtime's translation units (ADR 0026, ADR 0029):
// objc.cpp (metadata, lookup, sending, start-up), nsobject.cpp (NSObject, autorelease pools,
// host state), libobjc.cpp (properties, @synchronized) and blocks.cpp (the blocks ABI).
//
// Plain C++ without exceptions, `new` or function-local statics (ADR 0019): the recompiler's
// C test harness links these files into a C program without a C++ runtime library.
// Containers are open-addressing hash tables and arrays over malloc'd memory.
#ifndef MAPLETHIEF_RUNTIME_OBJC_OBJC_INTERNAL_H
#define MAPLETHIEF_RUNTIME_OBJC_OBJC_INTERNAL_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"

namespace mt_objc_impl {

using u32 = std::uint32_t;

// --- containers ----------------------------------------------------------------------------

inline u32 hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// u32 -> u32 with linear probing; key 0 is reserved for empty slots (guest addresses, SELs and
// the ids stored here are never 0).
struct U32Map {
    u32 *slots = nullptr; // cap pairs {key, value}
    u32 cap = 0;
    u32 size = 0;
};

bool map_get(const U32Map &m, u32 key, u32 *value);
bool map_put(U32Map &m, u32 key, u32 value);
// Removes `key` (backward-shift deletion, so lookups never need tombstones). Returns whether
// it was there.
bool map_erase(U32Map &m, u32 key);
void map_clear(U32Map &m);
void map_free(U32Map &m);

// Appends one element to a malloc'd array; returns its index or -1.
template <typename T> long push(T *&items, u32 &count, u32 &cap, const T &item) {
    if (count == cap) {
        const u32 grown = cap != 0 ? cap * 2 : 16;
        void *p = std::realloc(items, sizeof(T) * grown);
        if (p == nullptr) {
            return -1;
        }
        items = static_cast<T *>(p);
        cap = grown;
    }
    items[count] = item;
    return static_cast<long>(count++);
}

// --- runtime state ---------------------------------------------------------------------------

struct Method {
    u32 sel; // selector id + 1
    u32 guest_imp;
    mt_guest_fn fn;
    u32 origin;
    u32 owner;
};

struct Class {
    u32 addr;  // guest class_t (or metaclass)
    u32 super; // class index + 1, 0 for none
    u32 meta;  // classes: metaclass index + 1; metaclasses: their class's index + 1
    bool is_meta;
    bool is_host;
    bool placeholder; // an imported class no host code implements yet (ADR 0029)
    bool initialized; // +initialize was sent (set on the class and its metaclass)
    char *name;
    u32 instance_start;
    u32 instance_size;
    u32 protocols;      // guest protocol_list_t of class_ro_t.baseProtocols, 0 if none
    u32 *cat_protocols; // protocol lists of categories on this class
    u32 cat_protocol_count, cat_protocol_cap;
    U32Map own;   // selector id + 1 -> method index: this class's methods
    U32Map cache; // guest SEL value -> method index: resolved lookups, inherited ones included
};

// An imported class object (or block isa object) placed by mt_imports_place.
struct ImportedClass {
    char *name; // class name, e.g. "NSObject", "__NSStackBlock__"
    u32 cls, meta;
};

struct Undeliverable {
    long ci;    // class or metaclass index, -1 for a receiver of no known class
    u32 sel_id; // selector id + 1, 0 if the SEL was no string
    u32 sel;
    u32 receiver;
    u32 first_caller;
    u32 count;
};

// One host state of one object (mt_objc_state).
struct State {
    u32 obj;
    const mt_objc_state_type *type;
    void *data;
    u32 next; // index + 1 of the object's next state, 0 for none; free list link when free
};

struct PoolMark {
    u32 pool;  // the NSAutoreleasePool object, 0 for one pushed by mt_objc_pool_push
    u32 start; // first index into pool_objects that belongs to this pool
};

} // namespace mt_objc_impl

struct mt_objc {
    mt_cpu *cpu;
    mt_guest_alloc_fn alloc;
    void *alloc_user;
    mt_guest_free_fn free_fn; // NULL: freed objects are never reused
    mt_import_config cfg;

    mt_objc_impl::Class *classes;
    std::uint32_t class_count, class_cap;
    mt_objc_impl::U32Map class_by_addr; // class or metaclass object -> class index + 1

    mt_objc_impl::Method *methods;
    std::uint32_t method_count, method_cap;

    // Selectors: id = index into these arrays.
    char **sel_names;         // host copies of the names
    std::uint32_t *sel_canon; // canonical SEL (guest string), 0 until one exists
    std::uint32_t sel_count, sel_cap;
    std::uint32_t *sel_slots; // open addressing over id + 1, by string hash
    std::uint32_t sel_slot_cap;
    mt_objc_impl::U32Map sel_ids; // every known guest copy of a selector string -> id + 1

    mt_objc_impl::U32Map pending; // __objc_classlist entries: addr -> k_pending or k_failed

    // Undeliverable messages, one record per (class, selector).
    mt_objc_impl::Undeliverable *undeliverable;
    std::uint32_t undeliverable_count, undeliverable_cap;
    mt_objc_undeliverable_fn undeliverable_hook;
    void *undeliverable_user;
    std::uint32_t host_caller; // lr of the guest call that led to a host send (for logs)

    mt_objc_impl::ImportedClass *imported;
    std::uint32_t imported_count, imported_cap;

    // NSObject services (nsobject.cpp, libobjc.cpp).
    mt_objc_impl::U32Map retains; // object -> retain count - 1 (absent: 1)
    mt_objc_impl::U32Map syncs;   // object -> @synchronized depth
    std::uint32_t *pool_objects;  // autoreleased objects, oldest first
    std::uint32_t pool_count, pool_cap;
    mt_objc_impl::PoolMark *marks; // autorelease pools, outermost first
    std::uint32_t mark_count, mark_cap;
    mt_objc_impl::State *states;
    std::uint32_t state_count, state_cap, state_free; // state_free: index + 1 of a free entry
    mt_objc_impl::U32Map state_heads;                 // object -> index + 1 of its newest state
    std::uint32_t malloc_block_class;                 // __NSMallocBlock__, found on first use

    mt_objc_stats stats;
};

namespace mt_objc_impl {

// --- shared helpers (objc.cpp) -------------------------------------------------------------

void log_line(const mt_objc *rt, const char *line);
bool rd32(const mt_objc *rt, u32 addr, u32 *value);
bool wr32(const mt_objc *rt, u32 addr, u32 value);
const char *guest_string(const mt_objc *rt, u32 addr);
long class_index(const mt_objc *rt, u32 addr);
// The selector id of `name` (interned now if new), or -1 if out of memory.
long sel_id_of(mt_objc *rt, const char *name);
// The method index for selector id + 1 `sel` in class `ci` and its superclasses, or -1.
long find_method(mt_objc *rt, long ci, u32 sel);
// The class index of an object's class (its isa), or -1. Class objects give their metaclass.
long object_class(const mt_objc *rt, u32 obj);
// Whether `obj` is a class or metaclass object.
bool is_class_object(const mt_objc *rt, u32 obj);
// Sends `sel` (selector id + 1) to `self` from host code with the given extra arguments; the
// lookup starts at class index `start` (-1: the receiver's isa). Returns r0.
u32 host_send(mt_cpu *cpu, mt_objc *rt, u32 self, long start, u32 sel_id1, const u32 *args,
              u32 argc);
// Whether `obj`'s class implements selector id + 1 `sel`.
bool responds(mt_objc *rt, u32 obj, u32 sel_id1);
// Calls guest (or import) function pointer `fn` with up to 4 word arguments from host code.
u32 call_guest(mt_cpu *cpu, mt_objc *rt, u32 fn, const u32 *args, u32 argc);

// --- nsobject.cpp ------------------------------------------------------------------------

// Frees an object's guest memory and every host-side record of it (retain count, states,
// @synchronized depth): the end of -[NSObject dealloc].
void free_object(mt_cpu *cpu, mt_objc *rt, u32 obj);
void free_services(mt_objc *rt);

// --- the runtime's host classes ------------------------------------------------------------

extern const mt_objc_host_class k_nsobject;
extern const mt_objc_host_class k_nsautoreleasepool;
extern const mt_objc_host_class k_nsblock;
extern const mt_objc_host_class k_stack_block;
extern const mt_objc_host_class k_malloc_block;
extern const mt_objc_host_class k_global_block;

} // namespace mt_objc_impl

#endif // MAPLETHIEF_RUNTIME_OBJC_OBJC_INTERNAL_H
