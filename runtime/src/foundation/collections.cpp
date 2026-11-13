// The collections (#49, ADR 0035): NSArray, NSMutableArray, NSSet, NSDictionary,
// NSMutableDictionary and NSNull, as host classes. An instance is an object of the class it
// was made from (no class clusters: [NSMutableArray alloc] is an NSMutableArray), its contents
// host state: a vector of retained guest objects. The game keeps short lists (the character
// slots of CStateMainMenu, the touches of a touch event, a few dictionary entries), so lookups
// are linear.
//
// Fast enumeration (for ... in, 28 game sends) copies the items into the caller's buffer;
// mutationsPtr points at a guest word that every mutation of a mutable collection bumps (the
// compiled loop calls objc_enumerationMutation when it changes), and at the object's isa for an
// immutable one.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "snapshot/snapshot_internal.h"

namespace mt_ns {

namespace {

#define MT_COUNT(a) static_cast<u32>(sizeof(a) / sizeof((a)[0]))

// --- storage -----------------------------------------------------------------------------------

struct ListState {
    std::vector<u32> *items; // retained objects
    u32 mutations;           // guest word bumped by every mutation, 0 until enumerated
    bool is_mutable;
};

void list_destroy(mt_cpu *cpu, u32, void *p) {
    auto *s = static_cast<ListState *>(p);
    if (s->items != nullptr) {
        const std::vector<u32> items = std::move(*s->items);
        delete s->items;
        s->items = nullptr;
        for (const u32 obj : items) {
            mt_objc_release(cpu, obj);
        }
    }
    if (s->mutations != 0) {
        mt_heap_free(mt_mem_from_cpu(cpu), s->mutations);
    }
}

void drop_list(void *state) { delete static_cast<ListState *>(state)->items; }
const mt_objc_state_type k_array_state = {"NSArray", sizeof(ListState), list_destroy, drop_list};
const mt_objc_state_type k_set_state = {"NSSet", sizeof(ListState), list_destroy, drop_list};

struct DictState {
    std::vector<std::pair<u32, u32>> *entries; // retained (copied) keys, retained values
    u32 mutations;
    bool is_mutable;
};

void dict_destroy(mt_cpu *cpu, u32, void *p) {
    auto *s = static_cast<DictState *>(p);
    if (s->entries != nullptr) {
        const std::vector<std::pair<u32, u32>> entries = std::move(*s->entries);
        delete s->entries;
        s->entries = nullptr;
        for (const auto &e : entries) {
            mt_objc_release(cpu, e.first);
            mt_objc_release(cpu, e.second);
        }
    }
    if (s->mutations != 0) {
        mt_heap_free(mt_mem_from_cpu(cpu), s->mutations);
    }
}

void drop_dict(void *state) { delete static_cast<DictState *>(state)->entries; }
const mt_objc_state_type k_dict_state = {"NSDictionary", sizeof(DictState), dict_destroy,
                                         drop_dict};

ListState *list_state(mt_cpu *cpu, u32 obj, const mt_objc_state_type *type) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr && obj != 0 ? static_cast<ListState *>(mt_objc_find_state(rt, obj, type))
                                     : nullptr;
}

DictState *dict_state(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr && obj != 0
               ? static_cast<DictState *>(mt_objc_find_state(rt, obj, &k_dict_state))
               : nullptr;
}

// A new instance of `cls` with empty list state of `type`.
u32 list_new(mt_cpu *cpu, u32 cls, const mt_objc_state_type *type, bool is_mutable) {
    mt_objc *rt = runtime(cpu);
    const u32 obj = cls != 0 ? mt_objc_create_instance(rt, cls) : 0u;
    auto *s = obj != 0 ? static_cast<ListState *>(mt_objc_state(rt, obj, type)) : nullptr;
    if (s == nullptr) {
        return 0;
    }
    s->items = new std::vector<u32>();
    s->is_mutable = is_mutable;
    return obj;
}

u32 dict_new(mt_cpu *cpu, u32 cls, bool is_mutable) {
    mt_objc *rt = runtime(cpu);
    const u32 obj = cls != 0 ? mt_objc_create_instance(rt, cls) : 0u;
    auto *s = obj != 0 ? static_cast<DictState *>(mt_objc_state(rt, obj, &k_dict_state)) : nullptr;
    if (s == nullptr) {
        return 0;
    }
    s->entries = new std::vector<std::pair<u32, u32>>();
    s->is_mutable = is_mutable;
    return obj;
}

void bump(mt_cpu *cpu, u32 mutations) {
    if (mutations != 0) {
        u32 v;
        std::memcpy(&v, cpu->mem + mutations, 4);
        v++;
        std::memcpy(cpu->mem + mutations, &v, 4);
    }
}

ListState &array_of(mt_cpu *cpu, u32 obj, const char *method) {
    ListState *s = list_state(cpu, obj, &k_array_state);
    if (s == nullptr) {
        raise(cpu, method, "not an NSArray", obj);
    }
    return *s;
}

ListState &set_of(mt_cpu *cpu, u32 obj, const char *method) {
    ListState *s = list_state(cpu, obj, &k_set_state);
    if (s == nullptr) {
        raise(cpu, method, "not an NSSet", obj);
    }
    return *s;
}

DictState &dict_of(mt_cpu *cpu, u32 obj, const char *method) {
    DictState *s = dict_state(cpu, obj);
    if (s == nullptr) {
        raise(cpu, method, "not an NSDictionary", obj);
    }
    return *s;
}

void return_autoreleased(mt_cpu *cpu, u32 obj) {
    cpu->r[0] = obj != 0 ? mt_objc_autorelease(cpu, obj) : 0u;
}

// -countByEnumeratingWithState:objects:count: over `items`.
void enumerate(mt_cpu *cpu, u32 self, const std::vector<u32> &items, u32 *mutations,
               bool is_mutable) {
    mt_args a = mt_objc_args(cpu);
    const u32 state = mt_arg_u32(&a), buffer = mt_arg_u32(&a), len = mt_arg_u32(&a);
    // NSFastEnumerationState: {unsigned long state; id *itemsPtr; unsigned long *mutationsPtr;
    // unsigned long extra[5];}
    std::uint8_t *st = mt_g2h(cpu, state, 32);
    u32 index;
    std::memcpy(&index, st, 4);
    const u32 size = static_cast<u32>(items.size());
    const u32 n = index < size ? std::min(len, size - index) : 0u;
    if (n != 0) {
        std::memcpy(mt_g2h(cpu, buffer, 4u * n), items.data() + index, 4u * n);
    }
    if (is_mutable && *mutations == 0) {
        *mutations = mt_heap_calloc(mt_mem_from_cpu(cpu), 1, 4);
    }
    const u32 mutations_ptr = is_mutable && *mutations != 0 ? *mutations : self;
    const u32 words[3] = {index + n, buffer, mutations_ptr};
    std::memcpy(st, words, sizeof words);
    cpu->r[0] = n;
}

// The key a dictionary stores for `key`: an immutable copy of a mutable string (NSCopying), else
// the key retained.
u32 store_key(mt_cpu *cpu, u32 key) {
    if (mt_cf::string_is_mutable(cpu, key)) {
        return mt_cf::string_create(cpu, *mt_cf::string_chars(cpu, key), false);
    }
    return mt_objc_retain(cpu, key);
}

bool dict_put(mt_cpu *cpu, DictState &s, u32 key, u32 value) {
    for (auto &e : *s.entries) {
        if (objects_equal(cpu, e.first, key)) {
            const u32 old = e.second;
            e.second = mt_objc_retain(cpu, value);
            mt_objc_release(cpu, old);
            bump(cpu, s.mutations);
            return true;
        }
    }
    const u32 k = store_key(cpu, key);
    s.entries->emplace_back(k, mt_objc_retain(cpu, value));
    bump(cpu, s.mutations);
    return true;
}

u32 dict_find(mt_cpu *cpu, const DictState &s, u32 key) {
    for (const auto &e : *s.entries) {
        if (objects_equal(cpu, e.first, key)) {
            return e.second;
        }
    }
    return 0;
}

// --- NSArray / NSMutableArray --------------------------------------------------------------------

bool array_class_is_mutable(mt_cpu *cpu, u32 cls) {
    return inherits(cpu, cls, class_named(cpu, "NSMutableArray"));
}

// +allocWithZone: an empty array of the receiving class.
void array_alloc(mt_cpu *cpu) {
    const u32 cls = cpu->r[0];
    cpu->r[0] = list_new(cpu, cls, &k_array_state, array_class_is_mutable(cpu, cls));
}

// +arrayWithObject: (nil raises)
void array_with_object(mt_cpu *cpu) {
    const u32 cls = cpu->r[0], obj = cpu->r[2];
    if (obj == 0) {
        raise(cpu, "+[NSArray arrayWithObject:]", "nil object", 0);
    }
    const u32 a = list_new(cpu, cls, &k_array_state, array_class_is_mutable(cpu, cls));
    if (a != 0) {
        array_of(cpu, a, "+[NSArray arrayWithObject:]").items->push_back(mt_objc_retain(cpu, obj));
    }
    return_autoreleased(cpu, a);
}

// -count
void array_count(mt_cpu *cpu) {
    cpu->r[0] = static_cast<u32>(array_of(cpu, cpu->r[0], "-[NSArray count]").items->size());
}

// -objectAtIndex: (out of range raises NSRangeException)
void array_object_at(mt_cpu *cpu) {
    const u32 self = cpu->r[0], i = cpu->r[2];
    const std::vector<u32> &items = *array_of(cpu, self, "-[NSArray objectAtIndex:]").items;
    if (i >= items.size()) {
        raise(cpu, "-[NSArray objectAtIndex:]", "index beyond bounds", i);
    }
    cpu->r[0] = items[i];
}

void array_enumerate(mt_cpu *cpu) {
    const u32 self = cpu->r[0];
    ListState &s = array_of(cpu, self, "-[NSArray countByEnumeratingWithState:objects:count:]");
    enumerate(cpu, self, *s.items, &s.mutations, s.is_mutable);
}

ListState &mutable_array_of(mt_cpu *cpu, u32 obj, const char *method) {
    ListState &s = array_of(cpu, obj, method);
    if (!s.is_mutable) {
        raise(cpu, method, "mutating an immutable array", obj);
    }
    return s;
}

// -initWithCapacity: (an empty array from +alloc)
void array_init_capacity(mt_cpu *) {}

// -addObject: (nil raises)
void array_add_object(mt_cpu *cpu) {
    const u32 self = cpu->r[0], obj = cpu->r[2];
    ListState &s = mutable_array_of(cpu, self, "-[NSMutableArray addObject:]");
    if (obj == 0) {
        raise(cpu, "-[NSMutableArray addObject:]", "nil object", 0);
    }
    s.items->push_back(mt_objc_retain(cpu, obj));
    bump(cpu, s.mutations);
}

// -removeAllObjects
void array_remove_all(mt_cpu *cpu) {
    ListState &s = mutable_array_of(cpu, cpu->r[0], "-[NSMutableArray removeAllObjects]");
    const std::vector<u32> items = std::move(*s.items);
    s.items->clear();
    bump(cpu, s.mutations);
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        mt_objc_release(cpu, *it);
    }
}

const mt_objc_host_method k_array_instance[] = {
    {"count", array_count},
    {"objectAtIndex:", array_object_at},
    {"countByEnumeratingWithState:objects:count:", array_enumerate},
};
const mt_objc_host_method k_array_class[] = {
    {"allocWithZone:", array_alloc},
    {"arrayWithObject:", array_with_object},
};
const mt_objc_host_method k_mutable_array_instance[] = {
    {"initWithCapacity:", array_init_capacity},
    {"addObject:", array_add_object},
    {"removeAllObjects", array_remove_all},
};

// --- NSSet ---------------------------------------------------------------------------------------

void set_alloc(mt_cpu *cpu) { cpu->r[0] = list_new(cpu, cpu->r[0], &k_set_state, false); }

// +setWithObject: (nil raises)
void set_with_object(mt_cpu *cpu) {
    const u32 cls = cpu->r[0], obj = cpu->r[2];
    if (obj == 0) {
        raise(cpu, "+[NSSet setWithObject:]", "nil object", 0);
    }
    const u32 s = list_new(cpu, cls, &k_set_state, false);
    if (s != 0) {
        set_of(cpu, s, "+[NSSet setWithObject:]").items->push_back(mt_objc_retain(cpu, obj));
    }
    return_autoreleased(cpu, s);
}

void set_count(mt_cpu *cpu) {
    cpu->r[0] = static_cast<u32>(set_of(cpu, cpu->r[0], "-[NSSet count]").items->size());
}

// -allObjects: an NSArray of the members.
void set_all_objects(mt_cpu *cpu) {
    const std::vector<u32> items = *set_of(cpu, cpu->r[0], "-[NSSet allObjects]").items;
    return_autoreleased(cpu,
                        array_create(cpu, items.data(), static_cast<u32>(items.size()), false));
}

void set_enumerate(mt_cpu *cpu) {
    const u32 self = cpu->r[0];
    ListState &s = set_of(cpu, self, "-[NSSet countByEnumeratingWithState:objects:count:]");
    enumerate(cpu, self, *s.items, &s.mutations, s.is_mutable);
}

const mt_objc_host_method k_set_instance[] = {
    {"count", set_count},
    {"allObjects", set_all_objects},
    {"countByEnumeratingWithState:objects:count:", set_enumerate},
};
const mt_objc_host_method k_set_class[] = {
    {"allocWithZone:", set_alloc},
    {"setWithObject:", set_with_object},
};

// --- NSDictionary / NSMutableDictionary ------------------------------------------------------

bool dict_class_is_mutable(mt_cpu *cpu, u32 cls) {
    return inherits(cpu, cls, class_named(cpu, "NSMutableDictionary"));
}

// +allocWithZone: an empty dictionary ([[NSDictionary alloc] init]: CF's resource properties).
void dict_alloc(mt_cpu *cpu) {
    const u32 cls = cpu->r[0];
    cpu->r[0] = dict_new(cpu, cls, dict_class_is_mutable(cpu, cls));
}

// +dictionaryWithObjectsAndKeys: (object, key, ..., nil); a later key replaces an earlier one.
void dict_with_objects_and_keys(mt_cpu *cpu) {
    const u32 cls = cpu->r[0];
    mt_args a = mt_objc_args(cpu);
    std::vector<std::pair<u32, u32>> pairs; // read every argument before sending a message
    for (;;) {
        const u32 value = mt_arg_u32(&a);
        if (value == 0) {
            break;
        }
        const u32 key = mt_arg_u32(&a);
        if (key == 0) {
            raise(cpu, "+[NSDictionary dictionaryWithObjectsAndKeys:]", "nil key", value);
        }
        pairs.emplace_back(key, value);
    }
    const u32 d = dict_new(cpu, cls, dict_class_is_mutable(cpu, cls));
    if (d != 0) {
        DictState &s = dict_of(cpu, d, "+[NSDictionary dictionaryWithObjectsAndKeys:]");
        for (const auto &p : pairs) {
            dict_put(cpu, s, p.first, p.second);
        }
    }
    return_autoreleased(cpu, d);
}

// +dictionaryWithCapacity: (NSMutableDictionary; the game's GameCenterManager cache)
void dict_with_capacity(mt_cpu *cpu) {
    const u32 cls = cpu->r[0];
    return_autoreleased(cpu, dict_new(cpu, cls, dict_class_is_mutable(cpu, cls)));
}

// -objectForKey: (nil for an absent or nil key)
void dict_object_for_key(mt_cpu *cpu) {
    const u32 self = cpu->r[0], key = cpu->r[2];
    const DictState &s = dict_of(cpu, self, "-[NSDictionary objectForKey:]");
    cpu->r[0] = key != 0 ? dict_find(cpu, s, key) : 0u;
}

// -count
void dict_count(mt_cpu *cpu) {
    cpu->r[0] = static_cast<u32>(dict_of(cpu, cpu->r[0], "-[NSDictionary count]").entries->size());
}

// -setObject:forKey: (nil raises)
void dict_set_object(mt_cpu *cpu) {
    const u32 self = cpu->r[0], value = cpu->r[2], key = cpu->r[3];
    DictState &s = dict_of(cpu, self, "-[NSMutableDictionary setObject:forKey:]");
    if (!s.is_mutable) {
        raise(cpu, "-[NSMutableDictionary setObject:forKey:]", "mutating an immutable dictionary",
              self);
    }
    if (value == 0 || key == 0) {
        raise(cpu, "-[NSMutableDictionary setObject:forKey:]", "nil object or key", 0);
    }
    dict_put(cpu, s, key, value);
}

const mt_objc_host_method k_dict_instance[] = {
    {"objectForKey:", dict_object_for_key},
    {"count", dict_count},
};
const mt_objc_host_method k_dict_class[] = {
    {"allocWithZone:", dict_alloc},
    {"dictionaryWithObjectsAndKeys:", dict_with_objects_and_keys},
};
const mt_objc_host_method k_mutable_dict_instance[] = {{"setObject:forKey:", dict_set_object}};
const mt_objc_host_method k_mutable_dict_class[] = {
    {"dictionaryWithCapacity:", dict_with_capacity}};

// --- NSNull --------------------------------------------------------------------------------------

// +null: the singleton (the empty character slots of CStateMainMenu).
void null_null(mt_cpu *cpu) { cpu->r[0] = null_object(cpu); }

void null_description(mt_cpu *cpu) {
    const u32 s = string_from_utf8(cpu, "<null>");
    return_autoreleased(cpu, s);
}

const mt_objc_host_method k_null_instance[] = {
    {"retain", singleton_retain},
    {"release", singleton_release},
    {"retainCount", singleton_retain_count},
    {"description", null_description},
};
const mt_objc_host_method k_null_class[] = {{"null", null_null}};

} // namespace

const mt_objc_host_class k_nsarray = {"NSArray",
                                      "NSObject",
                                      4,
                                      k_array_instance,
                                      MT_COUNT(k_array_instance),
                                      k_array_class,
                                      MT_COUNT(k_array_class)};
const mt_objc_host_class k_nsmutablearray = {
    "NSMutableArray", "NSArray", 4, k_mutable_array_instance, MT_COUNT(k_mutable_array_instance),
    nullptr,          0};
const mt_objc_host_class k_nsset = {
    "NSSet",     "NSObject",           4, k_set_instance, MT_COUNT(k_set_instance),
    k_set_class, MT_COUNT(k_set_class)};
const mt_objc_host_class k_nsdictionary = {
    "NSDictionary",        "NSObject", 4, k_dict_instance, MT_COUNT(k_dict_instance), k_dict_class,
    MT_COUNT(k_dict_class)};
const mt_objc_host_class k_nsmutabledictionary = {"NSMutableDictionary",
                                                  "NSDictionary",
                                                  4,
                                                  k_mutable_dict_instance,
                                                  MT_COUNT(k_mutable_dict_instance),
                                                  k_mutable_dict_class,
                                                  MT_COUNT(k_mutable_dict_class)};
const mt_objc_host_class k_nsnull = {"NSNull",
                                     "NSObject",
                                     4,
                                     k_null_instance,
                                     MT_COUNT(k_null_instance),
                                     k_null_class,
                                     MT_COUNT(k_null_class)};

// --- host API
// --------------------------------------------------------------------------------------

u32 array_create(mt_cpu *cpu, const u32 *items, u32 count, bool is_mutable) {
    const u32 a = list_new(cpu, class_named(cpu, is_mutable ? "NSMutableArray" : "NSArray"),
                           &k_array_state, is_mutable);
    ListState *s = a != 0 ? list_state(cpu, a, &k_array_state) : nullptr;
    if (s == nullptr) {
        return 0;
    }
    for (u32 i = 0; i < count; i++) {
        s->items->push_back(mt_objc_retain(cpu, items[i]));
    }
    return a;
}

const std::vector<u32> *array_items(mt_cpu *cpu, u32 obj) {
    const ListState *s = list_state(cpu, obj, &k_array_state);
    return s != nullptr ? s->items : nullptr;
}

u32 set_create(mt_cpu *cpu, const u32 *items, u32 count) {
    const u32 set = list_new(cpu, class_named(cpu, "NSSet"), &k_set_state, false);
    ListState *s = set != 0 ? list_state(cpu, set, &k_set_state) : nullptr;
    if (s == nullptr) {
        return 0;
    }
    for (u32 i = 0; i < count; i++) {
        bool seen = false;
        for (const u32 m : *s->items) {
            seen = seen || objects_equal(cpu, m, items[i]);
        }
        if (!seen && items[i] != 0) {
            s->items->push_back(mt_objc_retain(cpu, items[i]));
        }
    }
    return set;
}

const std::vector<u32> *set_items(mt_cpu *cpu, u32 obj) {
    const ListState *s = list_state(cpu, obj, &k_set_state);
    return s != nullptr ? s->items : nullptr;
}

u32 dictionary_create(mt_cpu *cpu, bool is_mutable) {
    return dict_new(cpu, class_named(cpu, is_mutable ? "NSMutableDictionary" : "NSDictionary"),
                    is_mutable);
}

u32 dictionary_get(mt_cpu *cpu, u32 dict, u32 key) {
    const DictState *s = dict_state(cpu, dict);
    return s != nullptr && key != 0 ? dict_find(cpu, *s, key) : 0u;
}

bool dictionary_set(mt_cpu *cpu, u32 dict, u32 key, u32 value) {
    DictState *s = dict_state(cpu, dict);
    return s != nullptr && key != 0 && value != 0 && dict_put(cpu, *s, key, value);
}

u32 null_object(mt_cpu *cpu) { return singleton(cpu, "NSNull"); }

} // namespace mt_ns

// --- save states (#202, ADR 0105) ---------------------------------------------------------------

namespace mt_ns {

namespace {

bool save_list(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const ListState *>(state);
    w.boolean(s->items != nullptr);
    if (s->items != nullptr) {
        w.words(*s->items);
    }
    w.u32(s->mutations);
    w.boolean(s->is_mutable);
    return true;
}

bool load_list(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<ListState *>(state);
    bool has = false;
    std::vector<u32> items;
    if (!r.boolean(&has) || (has && !r.words(&items)) || !r.u32(&s->mutations) ||
        !r.boolean(&s->is_mutable)) {
        return false;
    }
    s->items = has ? new std::vector<u32>(std::move(items)) : nullptr;
    return true;
}

bool save_dict(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const DictState *>(state);
    w.boolean(s->entries != nullptr);
    if (s->entries != nullptr) {
        w.u32(static_cast<u32>(s->entries->size()));
        for (const auto &e : *s->entries) {
            w.u32(e.first);
            w.u32(e.second);
        }
    }
    w.u32(s->mutations);
    w.boolean(s->is_mutable);
    return true;
}

bool load_dict(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<DictState *>(state);
    bool has = false;
    std::vector<std::pair<u32, u32>> entries;
    if (!r.boolean(&has)) {
        return false;
    }
    if (has) {
        u32 n = 0;
        if (!r.u32(&n) || !r.count_fits(n, 8)) {
            return false;
        }
        entries.resize(n);
        for (auto &e : entries) {
            r.u32(&e.first);
            r.u32(&e.second);
        }
    }
    if (!r.u32(&s->mutations) || !r.boolean(&s->is_mutable)) {
        return false;
    }
    s->entries = has ? new std::vector<std::pair<u32, u32>>(std::move(entries)) : nullptr;
    return true;
}

} // namespace

extern const mt_snap::StateOps k_snap_array;
const mt_snap::StateOps k_snap_array = {&k_array_state, "NSArray", save_list, load_list, nullptr};
extern const mt_snap::StateOps k_snap_set;
const mt_snap::StateOps k_snap_set = {&k_set_state, "NSSet", save_list, load_list, nullptr};
extern const mt_snap::StateOps k_snap_dict;
const mt_snap::StateOps k_snap_dict = {&k_dict_state, "NSDictionary", save_dict, load_dict,
                                       nullptr};

} // namespace mt_ns
