// The Objective-C runtime over the guest's own metadata (ADR 0026, ADR 0029): metadata,
// selectors, lookup and caches, message sending (from guest and host code), +initialize,
// undeliverable messages and start-up. See maplethief/objc/objc.h; NSObject and the other
// services are in nsobject.cpp, libobjc.cpp and blocks.cpp, the host class registry in
// classes.cpp.
//
// Plain C++ without exceptions, allocation through new or function-local statics (like the
// import code, ADR 0019): the recompiler's C test harness compiles it with -fno-exceptions and
// links it into a C program (tests/test_objc_runtime.py). Containers are open-addressing hash
// tables over malloc'd arrays.
//
// Guest layouts (32-bit, objc4 objc-runtime-new.h; tools/mtport/objc.py parses the same):
//
//     class_t       { isa, superclass, cache, vtable, data }    data & ~3 -> class_ro_t
//     class_ro_t    { flags, instanceStart, instanceSize, ivarLayout, name, baseMethods,
//                     baseProtocols, ... }
//     method_list_t { entsizeAndFlags, count } then count x method_t { name, types, imp }
//     category_t    { name, cls, instanceMethods, classMethods, protocols, properties }
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "maplethief/diag.h" // the unimplemented report (#53)
#include "objc/objc_internal.h"

namespace mt_objc_impl {

// --- containers --------------------------------------------------------------------------

bool map_get(const U32Map &m, u32 key, u32 *value) {
    if (m.cap == 0 || key == 0) {
        return false;
    }
    for (u32 i = hash32(key) & (m.cap - 1);; i = (i + 1) & (m.cap - 1)) {
        const u32 k = m.slots[2 * i];
        if (k == key) {
            *value = m.slots[2 * i + 1];
            return true;
        }
        if (k == 0) {
            return false;
        }
    }
}

namespace {

void map_insert_slot(U32Map &m, u32 key, u32 value) {
    u32 i = hash32(key) & (m.cap - 1);
    while (m.slots[2 * i] != 0 && m.slots[2 * i] != key) {
        i = (i + 1) & (m.cap - 1);
    }
    if (m.slots[2 * i] == 0) {
        m.size++;
    }
    m.slots[2 * i] = key;
    m.slots[2 * i + 1] = value;
}

} // namespace

bool map_put(U32Map &m, u32 key, u32 value) {
    if (key == 0) {
        return false;
    }
    if ((m.size + 1) * 4 > m.cap * 3) {
        const u32 cap = m.cap != 0 ? m.cap * 2 : 8;
        auto *slots = static_cast<u32 *>(std::calloc(2 * static_cast<std::size_t>(cap), 4));
        if (slots == nullptr) {
            return false;
        }
        U32Map grown{slots, cap, 0};
        for (u32 i = 0; i < m.cap; i++) {
            if (m.slots[2 * i] != 0) {
                map_insert_slot(grown, m.slots[2 * i], m.slots[2 * i + 1]);
            }
        }
        std::free(m.slots);
        m = grown;
    }
    map_insert_slot(m, key, value);
    return true;
}

bool map_erase(U32Map &m, u32 key) {
    if (m.cap == 0 || key == 0) {
        return false;
    }
    const u32 mask = m.cap - 1;
    u32 i = hash32(key) & mask;
    while (m.slots[2 * i] != key) {
        if (m.slots[2 * i] == 0) {
            return false;
        }
        i = (i + 1) & mask;
    }
    // Backward shift: move later entries of the probe run into the hole when their home
    // slot does not lie (cyclically) between the hole and their position.
    for (u32 j = (i + 1) & mask;; j = (j + 1) & mask) {
        const u32 k = m.slots[2 * j];
        if (k == 0) {
            break;
        }
        const u32 home = hash32(k) & mask;
        const bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
        if (!stays) {
            m.slots[2 * i] = k;
            m.slots[2 * i + 1] = m.slots[2 * j + 1];
            i = j;
        }
    }
    m.slots[2 * i] = 0;
    m.slots[2 * i + 1] = 0;
    m.size--;
    return true;
}

void map_clear(U32Map &m) {
    if (m.slots != nullptr) {
        std::memset(m.slots, 0, 2 * static_cast<std::size_t>(m.cap) * 4);
    }
    m.size = 0;
}

void map_free(U32Map &m) {
    std::free(m.slots);
    m = U32Map{};
}

namespace {

char *copy_string(const char *s) {
    const std::size_t n = std::strlen(s) + 1;
    auto *out = static_cast<char *>(std::malloc(n));
    if (out != nullptr) {
        std::memcpy(out, s, n);
    }
    return out;
}

constexpr u32 k_ro_meta = 0x1;
constexpr u32 k_max_list = 0x10000;  // a list header claiming more entries is corrupt
constexpr u32 k_max_string = 0x1000; // longest selector or class name read from the guest
constexpr u32 k_class_object_size = 20;
constexpr u32 k_pending = 1; // in __objc_classlist, not realized yet
constexpr u32 k_failed = 2;  // in __objc_classlist, could not be realized

mt_objc *g_current = nullptr; // the guest is single-threaded (ADR 0014)

void problem(mt_objc *rt, const char *line) {
    rt->stats.problems++;
    log_line(rt, line);
}

} // namespace

void log_line(const mt_objc *rt, const char *line) {
    if (rt->cfg.log != nullptr) {
        rt->cfg.log(rt->cfg.log_user, line);
        return;
    }
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// --- guest memory (untrusted metadata: every read is bounds-checked) --------------------

bool rd32(const mt_objc *rt, u32 addr, u32 *value) {
    if (static_cast<std::uint64_t>(addr) + 4 > rt->cpu->mem_size) {
        return false;
    }
    std::memcpy(value, rt->cpu->mem + addr, 4);
    return true;
}

bool wr32(const mt_objc *rt, u32 addr, u32 value) {
    if (static_cast<std::uint64_t>(addr) + 4 > rt->cpu->mem_size) {
        return false;
    }
    std::memcpy(rt->cpu->mem + addr, &value, 4);
    return true;
}

// A NUL-terminated guest string of at most k_max_string bytes, or nullptr.
const char *guest_string(const mt_objc *rt, u32 addr) {
    if (addr == 0 || addr >= rt->cpu->mem_size) {
        return nullptr;
    }
    const std::uint64_t room = rt->cpu->mem_size - addr;
    const auto *s = reinterpret_cast<const char *>(rt->cpu->mem + addr);
    const std::size_t limit = room < k_max_string ? static_cast<std::size_t>(room) : k_max_string;
    return std::memchr(s, 0, limit) != nullptr ? s : nullptr;
}

// --- selectors ---------------------------------------------------------------------------

namespace {

u32 fnv1a(const char *s) {
    u32 h = 2166136261u;
    for (; *s != 0; s++) {
        h = (h ^ static_cast<unsigned char>(*s)) * 16777619u;
    }
    return h;
}

// Index of the selector string `name`, or -1.
long sel_find(const mt_objc *rt, const char *name) {
    if (rt->sel_slot_cap == 0) {
        return -1;
    }
    for (u32 i = fnv1a(name) & (rt->sel_slot_cap - 1);; i = (i + 1) & (rt->sel_slot_cap - 1)) {
        const u32 v = rt->sel_slots[i];
        if (v == 0) {
            return -1;
        }
        if (std::strcmp(rt->sel_names[v - 1], name) == 0) {
            return static_cast<long>(v - 1);
        }
    }
}

bool sel_slots_grow(mt_objc *rt) {
    const u32 cap = rt->sel_slot_cap != 0 ? rt->sel_slot_cap * 2 : 256;
    auto *slots = static_cast<u32 *>(std::calloc(cap, 4));
    if (slots == nullptr) {
        return false;
    }
    for (u32 n = 0; n < rt->sel_count; n++) {
        u32 i = fnv1a(rt->sel_names[n]) & (cap - 1);
        while (slots[i] != 0) {
            i = (i + 1) & (cap - 1);
        }
        slots[i] = n + 1;
    }
    std::free(rt->sel_slots);
    rt->sel_slots = slots;
    rt->sel_slot_cap = cap;
    return true;
}

// The id (index) of selector `name`, or -1 if out of memory. `at` is a guest copy of the string
// (0: none). Methods are keyed by id, so which guest copy is the canonical SEL (sel_value) can
// be decided late: the first copy registered with an address, normally the image's.
long intern(mt_objc *rt, const char *name, u32 at) {
    long index = sel_find(rt, name);
    if (index < 0) {
        if ((rt->sel_count + 1) * 2 > rt->sel_slot_cap && !sel_slots_grow(rt)) {
            return -1;
        }
        char *copy = copy_string(name);
        if (copy == nullptr) {
            return -1;
        }
        if (rt->sel_count == rt->sel_cap) {
            const u32 cap = rt->sel_cap != 0 ? rt->sel_cap * 2 : 256;
            void *names = std::realloc(rt->sel_names, sizeof(char *) * cap);
            if (names != nullptr) {
                rt->sel_names = static_cast<char **>(names);
            }
            void *canon =
                names != nullptr ? std::realloc(rt->sel_canon, sizeof(u32) * cap) : nullptr;
            if (canon == nullptr) {
                std::free(copy);
                return -1;
            }
            rt->sel_canon = static_cast<u32 *>(canon);
            rt->sel_cap = cap;
        }
        index = static_cast<long>(rt->sel_count++);
        rt->sel_names[index] = copy;
        rt->sel_canon[index] = 0;
        u32 i = fnv1a(copy) & (rt->sel_slot_cap - 1);
        while (rt->sel_slots[i] != 0) {
            i = (i + 1) & (rt->sel_slot_cap - 1);
        }
        rt->sel_slots[i] = static_cast<u32>(index) + 1;
        rt->stats.selectors++;
    }
    if (at != 0) {
        if (!map_put(rt->sel_ids, at, static_cast<u32>(index) + 1)) {
            return -1;
        }
        if (rt->sel_canon[index] == 0) {
            rt->sel_canon[index] = at;
        }
    }
    return index;
}

// The canonical SEL of selector `index`: its first guest copy, placed now if it has none yet
// (a name only host code uses). 0 if out of memory.
u32 sel_value(mt_objc *rt, long index) {
    if (index < 0) {
        return 0;
    }
    if (rt->sel_canon[index] == 0) {
        const char *name = rt->sel_names[index];
        const std::size_t n = std::strlen(name) + 1;
        const u32 at = rt->alloc != nullptr ? rt->alloc(rt->alloc_user, static_cast<u32>(n), 1) : 0;
        if (at == 0 || static_cast<std::uint64_t>(at) + n > rt->cpu->mem_size ||
            !map_put(rt->sel_ids, at, static_cast<u32>(index) + 1)) {
            return 0;
        }
        std::memcpy(rt->cpu->mem + at, name, n);
        rt->sel_canon[index] = at;
    }
    return rt->sel_canon[index];
}

// The selector id of a guest SEL value (any copy of the string), or -1 if it is none.
long sel_id(mt_objc *rt, u32 sel) {
    u32 v;
    if (map_get(rt->sel_ids, sel, &v)) {
        return static_cast<long>(v - 1);
    }
    const char *name = guest_string(rt, sel);
    return name != nullptr ? intern(rt, name, sel) : -1;
}

} // namespace

long sel_id_of(mt_objc *rt, const char *name) { return intern(rt, name, 0); }

// --- classes -----------------------------------------------------------------------------

long class_index(const mt_objc *rt, u32 addr) {
    u32 v;
    return map_get(rt->class_by_addr, addr, &v) ? static_cast<long>(v - 1) : -1;
}

long object_class(const mt_objc *rt, u32 obj) {
    u32 isa;
    return obj != 0 && rd32(rt, obj, &isa) ? class_index(rt, isa) : -1;
}

bool is_class_object(const mt_objc *rt, u32 obj) { return class_index(rt, obj) >= 0; }

namespace {

long new_class(mt_objc *rt, u32 addr, const char *name, bool is_meta, bool is_host) {
    Class c{};
    c.addr = addr;
    c.is_meta = is_meta;
    c.is_host = is_host;
    c.name = copy_string(name);
    if (c.name == nullptr) {
        return -1;
    }
    const long index = push(rt->classes, rt->class_count, rt->class_cap, c);
    if (index < 0 || !map_put(rt->class_by_addr, addr, static_cast<u32>(index) + 1)) {
        std::free(c.name);
        return -1;
    }
    return index;
}

void clear_caches(mt_objc *rt) {
    for (u32 i = 0; i < rt->class_count; i++) {
        map_clear(rt->classes[i].cache);
    }
}

// Adds (or replaces) a method of class `ci`.
bool add_method(mt_objc *rt, long ci, u32 sel, u32 guest_imp, mt_guest_fn fn, u32 origin) {
    const Method m{sel, guest_imp, fn, origin, rt->classes[ci].addr};
    u32 existing;
    if (map_get(rt->classes[ci].own, sel, &existing)) {
        rt->methods[existing] = m;
        return true;
    }
    const long mi = push(rt->methods, rt->method_count, rt->method_cap, m);
    if (mi < 0 || !map_put(rt->classes[ci].own, sel, static_cast<u32>(mi))) {
        return false;
    }
    rt->stats.methods++;
    return true;
}

// Adds the methods of a guest method_list_t to class `ci`. Returns false on malformed data.
bool add_method_list(mt_objc *rt, long ci, u32 list, u32 origin) {
    if (list == 0) {
        return true;
    }
    u32 entsize_flags, count;
    if (!rd32(rt, list, &entsize_flags) || !rd32(rt, list + 4, &count)) {
        return false;
    }
    const u32 entsize = entsize_flags & 0xFFFCu;
    if (entsize < 12 || count > k_max_list) {
        return false;
    }
    for (u32 i = 0; i < count; i++) {
        const u32 at = list + 8 + i * entsize;
        u32 name, imp;
        if (!rd32(rt, at, &name) || !rd32(rt, at + 8, &imp)) {
            return false;
        }
        const char *text = guest_string(rt, name);
        const long id = text != nullptr ? intern(rt, text, name) : -1;
        if (id < 0 || !add_method(rt, ci, static_cast<u32>(id) + 1, imp, nullptr, origin)) {
            return false;
        }
    }
    return true;
}

struct ClassRo {
    u32 flags, start, size, name, methods, protocols;
};

bool read_ro(const mt_objc *rt, u32 cls, ClassRo *ro) {
    u32 data;
    if (!rd32(rt, cls + 16, &data)) {
        return false;
    }
    data &= ~3u;
    return rd32(rt, data, &ro->flags) && rd32(rt, data + 4, &ro->start) &&
           rd32(rt, data + 8, &ro->size) && rd32(rt, data + 16, &ro->name) &&
           rd32(rt, data + 20, &ro->methods) && rd32(rt, data + 24, &ro->protocols);
}

long realize(mt_objc *rt, u32 addr, u32 depth);

constexpr long k_root = -2;    // resolve_super: the word is 0
constexpr long k_unknown = -1; // resolve_super: no image or host class there
constexpr long k_broken = -3;  // resolve_super: an image class that could not be realized

// The class a superclass (or category target) word points to: an image class, realized first,
// or a host class.
long resolve_super(mt_objc *rt, u32 super, u32 depth) {
    if (super == 0) {
        return k_root;
    }
    const long known = class_index(rt, super);
    if (known >= 0) {
        return known;
    }
    u32 v;
    if (map_get(rt->pending, super, &v)) {
        return v == k_pending && realize(rt, super, depth + 1) >= 0 ? class_index(rt, super)
                                                                    : k_broken;
    }
    return k_unknown;
}

long realize_class(mt_objc *rt, u32 addr, u32 depth);

// Realizes the image class at `addr` and its metaclass, superclasses first. Logs a problem
// and returns -1 if it can't (and never tries that class again).
long realize(mt_objc *rt, u32 addr, u32 depth) {
    const long known = class_index(rt, addr);
    if (known >= 0) {
        return known;
    }
    u32 v;
    if (map_get(rt->pending, addr, &v) && v == k_failed) {
        return -1;
    }
    const long ci = realize_class(rt, addr, depth);
    if (ci < 0) {
        map_put(rt->pending, addr, k_failed);
    }
    return ci;
}

long realize_class(mt_objc *rt, u32 addr, u32 depth) {
    char line[256];
    if (depth > 64) {
        std::snprintf(line, sizeof line, "objc: superclass chain of 0x%08X is a loop", addr);
        problem(rt, line);
        return -1;
    }
    u32 isa, super_addr;
    ClassRo ro{}, mro{};
    if (!rd32(rt, addr, &isa) || !rd32(rt, addr + 4, &super_addr) || !read_ro(rt, addr, &ro) ||
        (ro.flags & k_ro_meta) != 0 || !read_ro(rt, isa, &mro) || (mro.flags & k_ro_meta) == 0) {
        std::snprintf(line, sizeof line, "objc: class 0x%08X: malformed class_t/class_ro_t", addr);
        problem(rt, line);
        return -1;
    }
    const char *name = guest_string(rt, ro.name);
    if (name == nullptr) {
        name = "?";
    }
    const long super = resolve_super(rt, super_addr, depth);
    if (super == k_unknown || super == k_broken) {
        std::snprintf(line, sizeof line, "objc: superclass 0x%08X of %s %s", super_addr, name,
                      super == k_unknown ? "is neither an image class nor a host class"
                                         : "could not be realized");
        problem(rt, line);
        return -1;
    }
    u32 meta_super_addr;
    if (!rd32(rt, isa + 4, &meta_super_addr)) {
        meta_super_addr = 0;
    }
    const long ci = new_class(rt, addr, name, false, false);
    const long mi = ci >= 0 ? new_class(rt, isa, name, true, false) : -1;
    if (mi < 0) {
        problem(rt, "objc: out of memory");
        return -1;
    }
    Class &c = rt->classes[ci];
    Class &m = rt->classes[mi];
    c.meta = static_cast<u32>(mi) + 1;
    m.meta = static_cast<u32>(ci) + 1;
    c.instance_start = ro.start;
    c.instance_size = ro.size;
    c.protocols = ro.protocols;
    m.instance_start = mro.start;
    m.instance_size = mro.size;
    if (super >= 0) {
        c.super = static_cast<u32>(super) + 1;
        const u32 super_meta = rt->classes[super].meta; // index + 1
        m.super = super_meta;
        if (super_meta == 0 || rt->classes[super_meta - 1].addr != meta_super_addr) {
            std::snprintf(line, sizeof line,
                          "objc: metaclass of %s: superclass 0x%08X is not the metaclass of %s",
                          name, meta_super_addr, rt->classes[super].name);
            problem(rt, line);
        }
        const Class &s = rt->classes[super];
        if (s.is_host && s.instance_size > ro.start) {
            rt->stats.instance_start_problems++;
            std::snprintf(line, sizeof line,
                          "objc: %s starts its ivars at %u, inside host superclass %s (%u bytes)",
                          name, ro.start, s.name, s.instance_size);
            problem(rt, line);
        }
    } else {
        m.super = static_cast<u32>(ci) + 1; // a root metaclass inherits from its class
    }
    if (!add_method_list(rt, ci, ro.methods, MT_OBJC_IMAGE) ||
        !add_method_list(rt, mi, mro.methods, MT_OBJC_IMAGE)) {
        std::snprintf(line, sizeof line, "objc: %s: malformed method list", name);
        problem(rt, line);
    }
    rt->stats.classes++;
    rt->stats.image_classes++;
    return ci;
}

void attach_category(mt_objc *rt, u32 cat) {
    char line[256];
    u32 name_addr, cls, inst, cls_methods, protocols;
    if (!rd32(rt, cat, &name_addr) || !rd32(rt, cat + 4, &cls) || !rd32(rt, cat + 8, &inst) ||
        !rd32(rt, cat + 12, &cls_methods) || !rd32(rt, cat + 16, &protocols)) {
        std::snprintf(line, sizeof line, "objc: category 0x%08X: malformed category_t", cat);
        problem(rt, line);
        return;
    }
    const char *name = guest_string(rt, name_addr);
    const long ci = resolve_super(rt, cls, 0);
    if (ci < 0 || rt->classes[ci].is_meta) {
        std::snprintf(line, sizeof line, "objc: category %s is on %s class 0x%08X",
                      name != nullptr ? name : "?", ci == k_broken ? "unrealized" : "unknown", cls);
        problem(rt, line);
        return;
    }
    const long mi = static_cast<long>(rt->classes[ci].meta) - 1;
    if (!add_method_list(rt, ci, inst, MT_OBJC_CATEGORY) ||
        !add_method_list(rt, mi, cls_methods, MT_OBJC_CATEGORY)) {
        std::snprintf(line, sizeof line, "objc: category %s: malformed method list",
                      name != nullptr ? name : "?");
        problem(rt, line);
        return;
    }
    if (protocols != 0) {
        Class &c = rt->classes[ci];
        push(c.cat_protocols, c.cat_protocol_count, c.cat_protocol_cap, protocols);
    }
    rt->stats.categories++;
}

} // namespace

// Finds selector id + 1 `sel` from class `ci` up; returns the method index or -1.
long find_method(mt_objc *rt, long ci, u32 sel) {
    u32 mi;
    for (long k = ci, steps = 0; k >= 0 && steps <= static_cast<long>(rt->class_count);
         k = static_cast<long>(rt->classes[k].super) - 1, steps++) {
        if (map_get(rt->classes[k].own, sel, &mi)) {
            return static_cast<long>(mi);
        }
    }
    return -1;
}

namespace {

// The method for `sel` (any guest SEL value) in class `ci` and its superclasses, through the
// class's cache; -1 if none.
long lookup_index(mt_objc *rt, long ci, u32 sel) {
    u32 mi;
    if (map_get(rt->classes[ci].cache, sel, &mi)) {
        return static_cast<long>(mi);
    }
    const long id = sel_id(rt, sel);
    if (id < 0) {
        return -1;
    }
    const long found = find_method(rt, ci, static_cast<u32>(id) + 1);
    if (found >= 0) {
        map_put(rt->classes[ci].cache, sel, static_cast<u32>(found));
    }
    return found;
}

// --- message sending ---------------------------------------------------------------------

// The guest caller to report for a send with return address `lr`: lr itself, or for a message
// host code sent (lr = MT_HLE_RETURN), the guest caller of that host code.
u32 reported_caller(const mt_objc *rt, u32 lr) {
    return lr == MT_HLE_RETURN && rt != nullptr && rt->host_caller != 0 ? rt->host_caller : lr;
}

// Records an undeliverable message; returns its record (or nullptr if out of memory) and
// whether it is new.
Undeliverable *record_undeliverable(mt_objc *rt, u32 self, long ci, u32 sel, u32 caller,
                                    bool *first) {
    const long id = sel_id(rt, sel);
    const u32 id1 = id >= 0 ? static_cast<u32>(id) + 1 : 0;
    for (u32 i = 0; i < rt->undeliverable_count; i++) {
        Undeliverable &u = rt->undeliverable[i];
        if (u.ci == ci && u.sel_id == id1 && (id1 != 0 || u.sel == sel)) {
            u.count++;
            *first = false;
            return &u;
        }
    }
    *first = true;
    const Undeliverable u{ci, id1, sel, self, caller, 1};
    const long at = push(rt->undeliverable, rt->undeliverable_count, rt->undeliverable_cap, u);
    return at >= 0 ? &rt->undeliverable[at] : nullptr;
}

mt_objc_undeliverable describe(const mt_objc *rt, const Undeliverable &u) {
    mt_objc_undeliverable out{};
    out.class_name = u.ci >= 0 ? rt->classes[u.ci].name : nullptr;
    out.class_method = u.ci >= 0 && rt->classes[u.ci].is_meta ? 1u : 0u;
    out.selector = u.sel_id != 0 ? rt->sel_names[u.sel_id - 1] : "?";
    out.sel = u.sel;
    out.receiver = u.receiver;
    out.first_caller = u.first_caller;
    out.count = u.count;
    return out;
}

// The message cannot be delivered: record and log it, then stop or return 0 per the policy.
void undeliverable(mt_cpu *cpu, mt_objc *rt, u32 self, long ci, u32 sel, bool stret) {
    const u32 lr = cpu->r[MT_LR];
    const u32 caller = reported_caller(rt, lr);
    const u32 policy =
        rt != nullptr ? rt->cfg.unimplemented : static_cast<u32>(MT_UNIMPLEMENTED_STOP);
    bool first = true;
    const Undeliverable *record =
        rt != nullptr ? record_undeliverable(rt, self, ci, sel, caller, &first) : nullptr;
    const char *name = rt != nullptr ? mt_objc_sel_name(rt, sel) : nullptr;
    char line[320];
    const char *outcome = policy == MT_UNIMPLEMENTED_STOP ? "stopping" : "returning 0";
    if (rt == nullptr) {
        std::snprintf(line, sizeof line,
                      "objc: message sent before the runtime exists (called from 0x%08X)", lr);
    } else if (ci < 0) {
        std::snprintf(line, sizeof line,
                      "objc: %s sent to 0x%08X, which is no object of a known class "
                      "(called from 0x%08X): %s",
                      name != nullptr ? name : "?", self, caller, outcome);
    } else {
        const Class &c = rt->classes[ci];
        std::snprintf(line, sizeof line,
                      "objc: unrecognized selector %c[%s%s %s] sent to 0x%08X (called from "
                      "0x%08X): %s",
                      c.is_meta ? '+' : '-', c.name, c.placeholder ? " (placeholder)" : "",
                      name != nullptr ? name : "?", self, caller, outcome);
    }
    // The unimplemented report (#53, ADR 0030) lists every (class, selector) pair.
    const Class *known = rt != nullptr && ci >= 0 ? &rt->classes[ci] : nullptr;
    mt_report_unimplemented_selector(known != nullptr ? known->name : nullptr,
                                     known != nullptr && known->is_meta ? 1 : 0,
                                     name != nullptr ? name : "?", caller);
    if (first || policy == MT_UNIMPLEMENTED_TRACE || policy == MT_UNIMPLEMENTED_STOP) {
        if (rt != nullptr) {
            log_line(rt, line);
        } else {
            std::fputs(line, stderr);
            std::fputc('\n', stderr);
        }
    }
    if (rt != nullptr && record != nullptr && rt->undeliverable_hook != nullptr) {
        const mt_objc_undeliverable u = describe(rt, *record);
        rt->undeliverable_hook(rt->undeliverable_user, &u);
    }
    if (policy == MT_UNIMPLEMENTED_STOP) {
        mt_trap(cpu, MT_TRAP_OBJC, caller, sel);
    }
    if (!stret) {
        cpu->r[0] = 0;
        cpu->r[1] = 0;
    }
}

void initialize_class(mt_cpu *cpu, mt_objc *rt, long ci);

// +initialize of the class of class index `ci` (a class or a metaclass), once.
inline void ensure_initialized(mt_cpu *cpu, mt_objc *rt, long ci) {
    if (!rt->classes[ci].initialized) {
        initialize_class(
            cpu, rt, rt->classes[ci].is_meta ? static_cast<long>(rt->classes[ci].meta) - 1 : ci);
    }
}

// Looks up `sel` from class index `ci` and enters the method with the registers as they are.
void deliver(mt_cpu *cpu, mt_objc *rt, u32 self, long ci, u32 sel, bool stret) {
    if (ci >= 0) {
        ensure_initialized(cpu, rt, ci);
    }
    const long mi = ci >= 0 ? lookup_index(rt, ci, sel) : -1;
    if (mi < 0) {
        undeliverable(cpu, rt, self, ci, sel, stret);
        return;
    }
    Method &m = rt->methods[mi];
    if (m.fn == nullptr) {
        m.fn = mt_dispatch(cpu, m.guest_imp, reported_caller(rt, cpu->r[MT_LR]));
    }
    m.fn(cpu);
}

// Registers a host call needs to give back: lr, sp, and the caller host sends report.
struct HostFrame {
    u32 lr, sp, caller;
};

// Starts a call from host code: stores the words of `stack` (if any) below sp, sets lr to
// MT_HLE_RETURN. The registers r0-r3 are the caller's to set.
HostFrame host_enter(mt_cpu *cpu, mt_objc *rt, const u32 *stack, u32 n) {
    const HostFrame f{cpu->r[MT_LR], cpu->r[MT_SP], rt != nullptr ? rt->host_caller : 0u};
    if (rt != nullptr && f.lr != MT_HLE_RETURN) {
        rt->host_caller = f.lr;
    }
    if (n > 0) {
        const u32 sp = (f.sp - 4u * n) & ~7u;
        for (u32 i = 0; i < n; i++) {
            mt_st32(cpu, sp + 4u * i, stack[i], f.lr);
        }
        cpu->r[MT_SP] = sp;
    }
    cpu->r[MT_LR] = MT_HLE_RETURN;
    return f;
}

void host_leave(mt_cpu *cpu, mt_objc *rt, const HostFrame &f) {
    cpu->r[MT_LR] = f.lr;
    cpu->r[MT_SP] = f.sp;
    if (rt != nullptr) {
        rt->host_caller = f.caller;
    }
}

// A host send: r0 = self, r1 = sel, then the arguments; the lookup starts at class index
// `start`, or at the receiver's isa if `start` is -1.
u32 send_from_host(mt_cpu *cpu, mt_objc *rt, u32 self, long start, u32 sel, const u32 *args,
                   u32 argc) {
    if (self == 0) {
        cpu->r[0] = 0;
        cpu->r[1] = 0;
        return 0;
    }
    const HostFrame f = host_enter(cpu, rt, argc > 2 ? args + 2 : nullptr, argc > 2 ? argc - 2 : 0);
    cpu->r[0] = self;
    cpu->r[1] = sel;
    cpu->r[2] = argc > 0 ? args[0] : 0u;
    cpu->r[3] = argc > 1 ? args[1] : 0u;
    deliver(cpu, rt, self, start >= 0 ? start : (rt != nullptr ? object_class(rt, self) : -1), sel,
            false);
    host_leave(cpu, rt, f);
    return cpu->r[0];
}

void initialize_class(mt_cpu *cpu, mt_objc *rt, long ci) {
    if (ci < 0 || rt->classes[ci].initialized) {
        return;
    }
    rt->classes[ci].initialized = true;
    rt->classes[rt->classes[ci].meta - 1].initialized = true;
    if (rt->classes[ci].super != 0) {
        initialize_class(cpu, rt, static_cast<long>(rt->classes[ci].super) - 1);
    }
    // Only if some class up the metaclass chain implements it (libobjc sends it anyway and
    // NSObject's does nothing; skipping it keeps runtimes without NSObject quiet).
    const long id = intern(rt, "initialize", 0);
    const long meta = static_cast<long>(rt->classes[ci].meta) - 1;
    if (id < 0 || find_method(rt, meta, static_cast<u32>(id) + 1) < 0) {
        return;
    }
    // The message being sent keeps its registers: r0-r3 hold its arguments.
    u32 saved[5];
    std::memcpy(saved, cpu->r, sizeof(u32) * 4);
    saved[4] = cpu->r[12];
    send_from_host(cpu, rt, rt->classes[ci].addr, meta, sel_value(rt, id), nullptr, 0);
    std::memcpy(cpu->r, saved, sizeof(u32) * 4);
    cpu->r[12] = saved[4];
}

} // namespace

u32 host_send(mt_cpu *cpu, mt_objc *rt, u32 self, long start, u32 sel_id1, const u32 *args,
              u32 argc) {
    const u32 sel = sel_value(rt, static_cast<long>(sel_id1) - 1);
    return send_from_host(cpu, rt, self, start, sel, args, argc);
}

bool responds(mt_objc *rt, u32 obj, u32 sel_id1) {
    const long ci = object_class(rt, obj);
    return ci >= 0 && find_method(rt, ci, sel_id1) >= 0;
}

u32 call_guest(mt_cpu *cpu, mt_objc *rt, u32 fn, const u32 *args, u32 argc) {
    const HostFrame f = host_enter(cpu, rt, nullptr, 0);
    for (u32 i = 0; i < 4; i++) {
        cpu->r[i] = i < argc ? args[i] : 0u;
    }
    mt_dispatch(cpu, fn, reported_caller(rt, MT_HLE_RETURN))(cpu);
    host_leave(cpu, rt, f);
    return cpu->r[0];
}

} // namespace mt_objc_impl

using namespace mt_objc_impl;

// --- creation and loading ------------------------------------------------------------------

extern "C" mt_objc *mt_objc_create(mt_cpu *cpu, mt_guest_alloc_fn alloc, void *alloc_user,
                                   const mt_import_config *cfg) noexcept {
    auto *rt = static_cast<mt_objc *>(std::calloc(1, sizeof(mt_objc)));
    if (rt == nullptr) {
        return nullptr;
    }
    rt->cpu = cpu;
    rt->alloc = alloc;
    rt->alloc_user = alloc_user;
    rt->cfg = cfg != nullptr ? *cfg : mt_import_config_default();
    return rt;
}

extern "C" void mt_objc_destroy(mt_objc *rt) noexcept {
    if (rt == nullptr) {
        return;
    }
    if (g_current == rt) {
        g_current = nullptr;
    }
    free_services(rt);
    for (u32 i = 0; i < rt->class_count; i++) {
        std::free(rt->classes[i].name);
        std::free(rt->classes[i].cat_protocols);
        map_free(rt->classes[i].own);
        map_free(rt->classes[i].cache);
    }
    for (u32 i = 0; i < rt->sel_count; i++) {
        std::free(rt->sel_names[i]);
    }
    for (u32 i = 0; i < rt->imported_count; i++) {
        std::free(rt->imported[i].name);
    }
    std::free(rt->classes);
    std::free(rt->methods);
    std::free(rt->sel_names);
    std::free(rt->sel_canon);
    std::free(rt->sel_slots);
    std::free(rt->undeliverable);
    std::free(rt->imported);
    map_free(rt->class_by_addr);
    map_free(rt->sel_ids);
    map_free(rt->pending);
    std::free(rt);
}

extern "C" void mt_objc_set_free(mt_objc *rt, mt_guest_free_fn free_fn) noexcept {
    rt->free_fn = free_fn;
}

extern "C" void mt_objc_set_current(mt_objc *rt) noexcept { g_current = rt; }

extern "C" mt_objc *mt_objc_current(void) noexcept { return g_current; }

namespace {

const ImportedClass *imported_named(const mt_objc *rt, const char *name) {
    for (u32 i = 0; i < rt->imported_count; i++) {
        if (std::strcmp(rt->imported[i].name, name) == 0) {
            return &rt->imported[i];
        }
    }
    return nullptr;
}

// Records `addr` as the class (or metaclass) object of `name`.
bool note_import(mt_objc *rt, const char *name, bool meta, u32 addr) {
    ImportedClass *known = const_cast<ImportedClass *>(imported_named(rt, name));
    if (known == nullptr) {
        ImportedClass c{copy_string(name), 0, 0};
        if (c.name == nullptr || push(rt->imported, rt->imported_count, rt->imported_cap, c) < 0) {
            std::free(c.name);
            return false;
        }
        known = &rt->imported[rt->imported_count - 1];
    }
    (meta ? known->meta : known->cls) = addr;
    return true;
}

} // namespace

extern "C" int mt_objc_add_host_class(mt_objc *rt, const mt_objc_host_class *def, u32 cls,
                                      u32 meta) noexcept {
    char line[256];
    if (mt_objc_class_named(rt, def->name) != 0) {
        std::snprintf(line, sizeof line, "objc: host class %s already exists", def->name);
        log_line(rt, line);
        return -1;
    }
    long super = -1;
    if (def->superclass != nullptr) {
        super = class_index(rt, mt_objc_class_named(rt, def->superclass));
        if (super < 0) {
            std::snprintf(line, sizeof line, "objc: host class %s: unknown superclass %s",
                          def->name, def->superclass);
            log_line(rt, line);
            return -1;
        }
    }
    if (const ImportedClass *imp = imported_named(rt, def->name)) {
        cls = cls != 0 ? cls : imp->cls;
        meta = meta != 0 ? meta : imp->meta;
    }
    u32 *const objects[2] = {&cls, &meta};
    for (u32 *addr : objects) {
        if (*addr == 0 && rt->alloc != nullptr) {
            *addr = rt->alloc(rt->alloc_user, k_class_object_size, 4);
        }
        if (*addr == 0 ||
            static_cast<std::uint64_t>(*addr) + k_class_object_size > rt->cpu->mem_size) {
            std::snprintf(line, sizeof line, "objc: host class %s: no class object", def->name);
            log_line(rt, line);
            return -1;
        }
    }
    const long ci = new_class(rt, cls, def->name, false, true);
    const long mi = ci >= 0 ? new_class(rt, meta, def->name, true, true) : -1;
    if (mi < 0) {
        log_line(rt, "objc: out of memory");
        return -1;
    }
    rt->classes[ci].meta = static_cast<u32>(mi) + 1;
    rt->classes[mi].meta = static_cast<u32>(ci) + 1;
    rt->classes[ci].instance_size = def->instance_size;
    rt->classes[ci].instance_start = def->instance_size;
    // isa and superclass words of both objects, as libobjc's would be. A root metaclass's isa
    // is itself; every other metaclass's isa is the root metaclass.
    long root_meta = mi;
    if (super >= 0) {
        rt->classes[ci].super = static_cast<u32>(super) + 1;
        rt->classes[mi].super = rt->classes[super].meta;
        long k = super;
        while (rt->classes[k].super != 0) {
            k = static_cast<long>(rt->classes[k].super) - 1;
        }
        root_meta = static_cast<long>(rt->classes[k].meta) - 1;
    } else {
        rt->classes[mi].super = static_cast<u32>(ci) + 1;
    }
    wr32(rt, cls, meta);
    wr32(rt, cls + 4, super >= 0 ? rt->classes[super].addr : 0u);
    wr32(rt, meta, rt->classes[root_meta].addr);
    wr32(rt, meta + 4, rt->classes[rt->classes[mi].super - 1].addr);
    for (u32 i = 0; i < def->instance_method_count + def->class_method_count; i++) {
        const bool inst = i < def->instance_method_count;
        const mt_objc_host_method &hm =
            inst ? def->instance_methods[i] : def->class_methods[i - def->instance_method_count];
        const long id = intern(rt, hm.selector, 0);
        if (id < 0 ||
            !add_method(rt, inst ? ci : mi, static_cast<u32>(id) + 1, 0, hm.fn, MT_OBJC_HOST)) {
            return -1;
        }
    }
    rt->stats.classes++;
    clear_caches(rt);
    return 0;
}

extern "C" u32 mt_objc_load_image(mt_objc *rt, const mt_objc_image *image) noexcept {
    char line[256];
    const u32 before = rt->stats.problems;
    // Selector references first, so that the method tables and the guest agree on one SEL per
    // name (libobjc's sel fixups; the linker already coalesces __objc_methname).
    for (u32 i = 0; i < image->selrefs_count; i++) {
        const u32 slot = image->selrefs + 4 * i;
        u32 sel;
        const char *name = rd32(rt, slot, &sel) ? guest_string(rt, sel) : nullptr;
        const u32 canon = name != nullptr ? sel_value(rt, intern(rt, name, sel)) : 0;
        if (canon == 0) {
            std::snprintf(line, sizeof line, "objc: selref 0x%08X: no selector string", slot);
            problem(rt, line);
            continue;
        }
        if (canon != sel) {
            wr32(rt, slot, canon);
            rt->stats.selrefs_changed++;
        }
    }
    u32 entry;
    for (u32 i = 0; i < image->classlist_count; i++) {
        if (rd32(rt, image->classlist + 4 * i, &entry)) {
            map_put(rt->pending, entry, k_pending);
        }
    }
    for (u32 i = 0; i < image->classlist_count; i++) {
        if (!rd32(rt, image->classlist + 4 * i, &entry)) {
            std::snprintf(line, sizeof line, "objc: class list entry %u is outside memory", i);
            problem(rt, line);
            continue;
        }
        realize(rt, entry, 0);
    }
    for (u32 i = 0; i < image->catlist_count; i++) {
        if (!rd32(rt, image->catlist + 4 * i, &entry)) {
            std::snprintf(line, sizeof line, "objc: category list entry %u is outside memory", i);
            problem(rt, line);
            continue;
        }
        attach_category(rt, entry);
    }
    clear_caches(rt);
    return rt->stats.problems - before;
}

extern "C" int mt_objc_override(mt_objc *rt, const char *class_name, int class_method,
                                const char *selector, mt_guest_fn fn) noexcept {
    long ci = class_index(rt, mt_objc_class_named(rt, class_name));
    if (ci < 0) {
        return -1;
    }
    if (class_method != 0) {
        ci = static_cast<long>(rt->classes[ci].meta) - 1;
    }
    const long id = intern(rt, selector, 0);
    if (id < 0 || !add_method(rt, ci, static_cast<u32>(id) + 1, 0, fn, MT_OBJC_OVERRIDE)) {
        return -1;
    }
    rt->stats.overrides++;
    clear_caches(rt);
    return 0;
}

extern "C" mt_objc_stats mt_objc_get_stats(const mt_objc *rt) noexcept { return rt->stats; }

// --- start-up --------------------------------------------------------------------------------

extern "C" int mt_objc_bind_imports(mt_objc *rt, const mt_import_desc *descs, u32 count,
                                    const u32 *addrs) noexcept {
    static constexpr char k_class[] = "_OBJC_CLASS_$_";
    static constexpr char k_meta[] = "_OBJC_METACLASS_$_";
    // The block isa objects of libSystem are the class objects of the block classes, and
    // CoreFoundation's constant-string isa is the class object of __NSCFConstantString.
    static constexpr struct {
        const char *symbol, *cls;
    } k_blocks[] = {
        {"__NSConcreteStackBlock", "__NSStackBlock__"},
        {"__NSConcreteGlobalBlock", "__NSGlobalBlock__"},
        {"__NSConcreteMallocBlock", "__NSMallocBlock__"},
        // The isa of the image's CFString literals (#48, ADR 0032).
        {"___CFConstantStringClassReference", "__NSCFConstantString"},
    };
    const u32 before = rt->imported_count;
    for (u32 id = 0; id < count; id++) {
        const char *name = descs[id].name;
        if ((descs[id].flags & MT_IMPORT_DATA) == 0 || name == nullptr || addrs[id] == 0) {
            continue;
        }
        bool ok = true;
        if (std::strncmp(name, k_class, sizeof k_class - 1) == 0) {
            ok = note_import(rt, name + sizeof k_class - 1, false, addrs[id]);
        } else if (std::strncmp(name, k_meta, sizeof k_meta - 1) == 0) {
            ok = note_import(rt, name + sizeof k_meta - 1, true, addrs[id]);
        } else {
            for (const auto &b : k_blocks) {
                if (std::strcmp(name, b.symbol) == 0) {
                    ok = note_import(rt, b.cls, false, addrs[id]);
                }
            }
        }
        if (!ok) {
            log_line(rt, "objc: out of memory");
            return -1;
        }
    }
    return static_cast<int>(rt->imported_count - before);
}

extern "C" int mt_objc_add_host_category(mt_objc *rt, const mt_objc_host_category *cat) noexcept {
    const long ci = class_index(rt, mt_objc_class_named(rt, cat->class_name));
    if (ci < 0) {
        char line[160];
        std::snprintf(line, sizeof line, "objc: host category on %s: no such class",
                      cat->class_name);
        log_line(rt, line);
        return -1;
    }
    const long mi = static_cast<long>(rt->classes[ci].meta) - 1;
    for (u32 i = 0; i < cat->instance_method_count + cat->class_method_count; i++) {
        const bool inst = i < cat->instance_method_count;
        const mt_objc_host_method &hm =
            inst ? cat->instance_methods[i] : cat->class_methods[i - cat->instance_method_count];
        const long id = intern(rt, hm.selector, 0);
        if (id < 0 ||
            !add_method(rt, inst ? ci : mi, static_cast<u32>(id) + 1, 0, hm.fn, MT_OBJC_HOST)) {
            log_line(rt, "objc: out of memory");
            return -1;
        }
    }
    clear_caches(rt);
    return 0;
}

extern "C" u32 mt_objc_add_host_classes(mt_objc *rt) noexcept {
    u32 n = 0, failed = 0;
    const mt_objc_host_class *const *defs = mt_objc_host_registry(&n);
    for (u32 i = 0; i < n; i++) {
        failed += mt_objc_add_host_class(rt, defs[i], 0, 0) != 0 ? 1u : 0u;
    }
    const mt_objc_host_category *const *cats = mt_objc_host_category_registry(&n);
    for (u32 i = 0; i < n; i++) {
        failed += mt_objc_add_host_category(rt, cats[i]) != 0 ? 1u : 0u;
    }
    return failed;
}

extern "C" u32 mt_objc_add_placeholders(mt_objc *rt) noexcept {
    // Guest instance sizes of the SDK the game was built against, for the classes the image
    // subclasses (ADR 0026): their subclasses' ivars start right after them.
    static constexpr struct {
        const char *name;
        u32 size;
    } k_sdk_sizes[] = {{"UIView", 46}, {"UIViewController", 152}};
    u32 added = 0;
    for (u32 i = 0; i < rt->imported_count; i++) {
        const ImportedClass &imp = rt->imported[i];
        if (mt_objc_class_named(rt, imp.name) != 0 || imp.cls == 0) {
            continue; // implemented, or the image never names the class object itself
        }
        u32 size = 4;
        for (const auto &s : k_sdk_sizes) {
            size = std::strcmp(s.name, imp.name) == 0 ? s.size : size;
        }
        const mt_objc_host_class def = {imp.name, "NSObject", size, nullptr, 0, nullptr, 0};
        if (mt_objc_add_host_class(rt, &def, 0, 0) == 0) {
            const long ci = class_index(rt, mt_objc_class_named(rt, imp.name));
            rt->classes[ci].placeholder = true;
            rt->classes[rt->classes[ci].meta - 1].placeholder = true;
            added++;
        }
    }
    rt->stats.placeholders += added;
    if (added > 0) {
        char line[160];
        std::snprintf(line, sizeof line,
                      "objc: %u imported classes have no host implementation yet; they are "
                      "placeholders (subclasses of NSObject without methods)",
                      added);
        log_line(rt, line);
    }
    return added;
}

extern "C" int mt_objc_class_is_placeholder(const mt_objc *rt, u32 cls) noexcept {
    const long ci = class_index(rt, cls);
    return ci >= 0 && rt->classes[ci].placeholder ? 1 : 0;
}

extern "C" u32 mt_objc_start(mt_objc *rt, const mt_import_desc *descs, u32 count, const u32 *addrs,
                             const mt_objc_image *image) noexcept {
    u32 problems = 0;
    if (descs != nullptr && mt_objc_bind_imports(rt, descs, count, addrs) < 0) {
        problems++;
    }
    problems += mt_objc_add_host_classes(rt);
    mt_objc_add_placeholders(rt);
    problems += mt_objc_load_image(rt, image);
    problems += mt_objc_install_overrides(rt);
    return problems;
}

// --- selectors and classes ------------------------------------------------------------------

extern "C" u32 mt_objc_sel(mt_objc *rt, const char *name) noexcept {
    return sel_value(rt, intern(rt, name, 0));
}

extern "C" const char *mt_objc_sel_name(mt_objc *rt, u32 sel) noexcept {
    u32 v;
    return map_get(rt->sel_ids, sel, &v) ? rt->sel_names[v - 1] : guest_string(rt, sel);
}

extern "C" u32 mt_objc_class_named(const mt_objc *rt, const char *name) noexcept {
    for (u32 i = 0; i < rt->class_count; i++) {
        if (!rt->classes[i].is_meta && std::strcmp(rt->classes[i].name, name) == 0) {
            return rt->classes[i].addr;
        }
    }
    return 0;
}

extern "C" u32 mt_objc_metaclass(const mt_objc *rt, u32 cls) noexcept {
    const long ci = class_index(rt, cls);
    return ci >= 0 && !rt->classes[ci].is_meta ? rt->classes[rt->classes[ci].meta - 1].addr : 0;
}

extern "C" u32 mt_objc_superclass(const mt_objc *rt, u32 cls) noexcept {
    const long ci = class_index(rt, cls);
    return ci >= 0 && rt->classes[ci].super != 0 ? rt->classes[rt->classes[ci].super - 1].addr : 0;
}

extern "C" const char *mt_objc_class_name(const mt_objc *rt, u32 cls) noexcept {
    const long ci = class_index(rt, cls);
    return ci >= 0 ? rt->classes[ci].name : nullptr;
}

extern "C" u32 mt_objc_instance_size(const mt_objc *rt, u32 cls) noexcept {
    const long ci = class_index(rt, cls);
    return ci >= 0 ? rt->classes[ci].instance_size : 0;
}

extern "C" int mt_objc_lookup(mt_objc *rt, u32 cls, u32 sel, mt_objc_method *out) noexcept {
    const long ci = class_index(rt, cls);
    const long mi = ci >= 0 ? lookup_index(rt, ci, sel) : -1;
    if (mi < 0) {
        return 0;
    }
    const Method &m = rt->methods[mi];
    *out = mt_objc_method{m.guest_imp, m.fn, m.origin, m.owner};
    return 1;
}

// --- undeliverable messages ---------------------------------------------------------------

extern "C" void mt_objc_set_undeliverable_hook(mt_objc *rt, mt_objc_undeliverable_fn fn,
                                               void *user) noexcept {
    rt->undeliverable_hook = fn;
    rt->undeliverable_user = user;
}

extern "C" u32 mt_objc_undeliverable_count(const mt_objc *rt) noexcept {
    return rt->undeliverable_count;
}

extern "C" int mt_objc_undeliverable_at(const mt_objc *rt, u32 i,
                                        mt_objc_undeliverable *out) noexcept {
    if (i >= rt->undeliverable_count) {
        return -1;
    }
    *out = describe(rt, rt->undeliverable[i]);
    return 0;
}

// --- the message senders ------------------------------------------------------------------

extern "C" void mt_hle_objc_msgSend(mt_cpu *cpu, u32) noexcept {
    const u32 self = cpu->r[0];
    if (self == 0) {
        cpu->r[0] = 0;
        cpu->r[1] = 0;
        return;
    }
    mt_objc *rt = g_current;
    deliver(cpu, rt, self, rt != nullptr ? object_class(rt, self) : -1, cpu->r[1], false);
}

extern "C" void mt_hle_objc_msgSend_stret(mt_cpu *cpu, u32) noexcept {
    const u32 self = cpu->r[1];
    if (self == 0) {
        return;
    }
    mt_objc *rt = g_current;
    deliver(cpu, rt, self, rt != nullptr ? object_class(rt, self) : -1, cpu->r[2], true);
}

extern "C" void mt_hle_objc_msgSendSuper2(mt_cpu *cpu, u32) noexcept {
    mt_objc *rt = g_current;
    u32 self = 0, current = 0;
    if (rt == nullptr || !rd32(rt, cpu->r[0], &self) || !rd32(rt, cpu->r[0] + 4, &current)) {
        undeliverable(cpu, rt, cpu->r[0], -1, cpu->r[1], false);
        return;
    }
    cpu->r[0] = self;
    if (self == 0) {
        cpu->r[1] = 0;
        return;
    }
    deliver(cpu, rt, self, class_index(rt, mt_objc_superclass(rt, current)), cpu->r[1], false);
}

// --- sending from host code -----------------------------------------------------------------

extern "C" u32 mt_objc_msg(mt_cpu *cpu, u32 self, u32 sel, const u32 *args, u32 argc) noexcept {
    return send_from_host(cpu, g_current, self, -1, sel, args, argc);
}

extern "C" u32 mt_objc_msg_super(mt_cpu *cpu, u32 self, u32 cls, u32 sel, const u32 *args,
                                 u32 argc) noexcept {
    mt_objc *rt = g_current;
    const long start = rt != nullptr ? class_index(rt, mt_objc_superclass(rt, cls)) : -1;
    if (start < 0 && self != 0) {
        const HostFrame f = host_enter(cpu, rt, nullptr, 0);
        undeliverable(cpu, rt, self, -1, sel, false);
        host_leave(cpu, rt, f);
        return 0;
    }
    return send_from_host(cpu, rt, self, start, sel, args, argc);
}

extern "C" void mt_objc_msg_stret(mt_cpu *cpu, u32 result, u32 self, u32 sel, const u32 *args,
                                  u32 argc) noexcept {
    if (self == 0) {
        return;
    }
    mt_objc *rt = g_current;
    const HostFrame f = host_enter(cpu, rt, argc > 1 ? args + 1 : nullptr, argc > 1 ? argc - 1 : 0);
    cpu->r[0] = result;
    cpu->r[1] = self;
    cpu->r[2] = sel;
    cpu->r[3] = argc > 0 ? args[0] : 0u;
    deliver(cpu, rt, self, rt != nullptr ? object_class(rt, self) : -1, sel, true);
    host_leave(cpu, rt, f);
}
