/* The Objective-C runtime over the guest's own metadata (ADR 0026, ADR 0029; #46 prototype,
 * #47 runtime).
 *
 * Objects, classes and selectors are guest values: an object is a guest address whose first
 * word is its isa (a guest class_t), a class is the guest address of its class_t, and a
 * selector (SEL) is the guest address of its canonical C string: the first guest copy the
 * runtime sees, normally the image's (selrefs pointing at another copy are rewritten). The
 * runtime keys methods by selector name, so any copy finds the same method. Two kinds of
 * class share one method table model:
 *
 *   image classes  the 75 classes of __objc_classlist (18 app, 57 Chartboost), realized from
 *                  class_t -> class_ro_t -> method_list_t; their IMPs are recompiled guest
 *                  functions, reached through mt_dispatch.
 *   host classes   the framework classes (NSObject, UIView, NSString, ...) implemented in the
 *                  runtime. Their class and metaclass objects are the imported
 *                  _OBJC_CLASS_$_X / _OBJC_METACLASS_$_X data objects that the image's
 *                  superclass, isa and classref words are bound to (ADR 0019), so image
 *                  classes inherit from them like from the real frameworks. Their methods are
 *                  host functions with the same signature as a recompiled function; write
 *                  them with maplethief/objc/host.h.
 *
 * Categories attach to either kind. A host override replaces one method of any class with a
 * host function (Chartboost's +sharedChartboost, the StoreKit availability check). Every
 * method, guest or host, is entered the same way: r0 = self, r1 = _cmd, the other arguments
 * per maplethief/hle/abi.h, lr = the sender's return address.
 *
 * Start-up (ADR 0029), after mt_imports_link, mt_imports_place and mt_imports_apply_binds:
 *
 *     mt_objc *rt = mt_objc_create(cpu, mt_mem_guest_alloc, mem, &cfg);
 *     mt_objc_set_free(rt, mt_mem_guest_free);
 *     mt_objc_start(rt, mt_import_descs, mt_import_count, addrs, &image);
 *     mt_objc_set_current(rt);
 *
 * mt_objc_start is mt_objc_bind_imports (the placed class objects), mt_objc_add_host_classes
 * (the runtime's host class registry), mt_objc_add_placeholders (imported classes nobody
 * implements yet), mt_objc_load_image (canonicalizes __objc_selrefs, realizes every class,
 * attaches the categories) and mt_objc_install_overrides. The objc_msgSend family of HLE
 * functions below uses the current runtime.
 *
 * The runtime provides NSObject (alloc on the guest heap, retain counts in a side table,
 * dealloc, autorelease, the NSObject protocol methods), NSAutoreleasePool, +initialize,
 * properties, @synchronized and the blocks ABI (ADR 0029). ivar sliding is only detected
 * (mt_objc_stats.instance_start_problems); the game needs none.
 *
 * Plain C++ without exceptions, `new` or function-local statics (like the import code,
 * ADR 0019), so the recompiler's C test harness can link it into a C program. */
#ifndef MAPLETHIEF_OBJC_OBJC_H
#define MAPLETHIEF_OBJC_OBJC_H

#include <stdint.h>

#include "maplethief/hle/imports.h"
#include "maplethief/recomp/imports.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

typedef struct mt_objc mt_objc;

/* --- host classes ------------------------------------------------------------------------ */

typedef struct mt_objc_host_method {
    const char *selector;
    mt_guest_fn fn;
} mt_objc_host_method;

typedef struct mt_objc_host_class {
    const char *name;
    const char *superclass; /* NULL for a root class; must be added before this one */
    /* Guest bytes of an instance (isa included). For a class the image subclasses (NSObject,
     * UIView, UIViewController) this is the size of the SDK the game was built against: a
     * subclass's ivars start right after it. Host state lives in a side table keyed by the
     * guest object, never in these bytes (maplethief/objc/host.h). */
    uint32_t instance_size;
    const mt_objc_host_method *instance_methods;
    uint32_t instance_method_count;
    const mt_objc_host_method *class_methods;
    uint32_t class_method_count;
} mt_objc_host_class;

/* --- the image's metadata ---------------------------------------------------------------- */

/* Guest address and number of 32-bit entries of the image's ObjC pointer sections (from the
 * loader manifest, work/recomp/loader.json). */
typedef struct mt_objc_image {
    uint32_t classlist, classlist_count;
    uint32_t catlist, catlist_count;
    uint32_t selrefs, selrefs_count;
} mt_objc_image;

typedef struct mt_objc_stats {
    uint32_t classes;         /* realized classes, metaclasses not counted (host + image) */
    uint32_t image_classes;   /* realized from __objc_classlist */
    uint32_t categories;      /* attached */
    uint32_t methods;         /* entries of every method table (image, host, categories) */
    uint32_t selectors;       /* distinct selector strings */
    uint32_t selrefs_changed; /* selrefs rewritten to the canonical SEL */
    uint32_t overrides;       /* host overrides installed */
    /* Image classes whose ivars start before the end of their host superclass's instance:
     * libobjc would slide their ivars; the runtime only counts them. */
    uint32_t instance_start_problems;
    uint32_t problems;     /* everything mt_objc_load_image logged as a problem */
    uint32_t placeholders; /* placeholder host classes (mt_objc_add_placeholders) */
} mt_objc_stats;

/* `cpu` gives access to guest memory (cpu->mem / mem_size) for the runtime's lifetime, and
 * is the guest thread the runtime sends messages on when no host method's cpu is at hand
 * (the guest is single-threaded, ADR 0014); `alloc` places the guest memory the runtime
 * creates, zeroed: objects (+allocWithZone:), copied blocks, host class objects not bound by
 * the image, selector strings registered by host code. The real runtime passes
 * mt_mem_guest_alloc and its mt_mem (maplethief/memory.h, ADR 0025); tests may pass a bump
 * allocator over a plain buffer. `cfg` (NULL: the default) gives the log sink and the policy
 * for unrecognized selectors, shared with unimplemented imports
 * (MAPLETHIEF_UNIMPLEMENTED_IMPORT). NULL if out of memory. */
mt_objc *mt_objc_create(mt_cpu *cpu, mt_guest_alloc_fn alloc, void *alloc_user,
                        const mt_import_config *cfg) MT_NOEXCEPT;
void mt_objc_destroy(mt_objc *rt) MT_NOEXCEPT;

/* Frees guest memory from `alloc` (called with alloc_user); returns 0, or -1 if `addr` is no
 * live block (the runtime logs it). */
typedef int (*mt_guest_free_fn)(void *user, uint32_t addr);

/* Gives the runtime the free function of its allocator: mt_mem_guest_free for the guest heap.
 * Without one (NULL, the default), memory of deallocated objects and released blocks is
 * never reused. */
void mt_objc_set_free(mt_objc *rt, mt_guest_free_fn free_fn) MT_NOEXCEPT;

/* The runtime the objc_msgSend family uses. */
void mt_objc_set_current(mt_objc *rt) MT_NOEXCEPT;
mt_objc *mt_objc_current(void) MT_NOEXCEPT;

/* Adds a host class. `cls` and `meta` are the guest addresses of its imported class and
 * metaclass objects; 0 means the object mt_objc_bind_imports recorded for the name, or else
 * a new 20-byte object from the allocator. Writes isa and superclass into both objects.
 * Returns 0, or -1 (and logs) if the name is taken, the superclass is unknown or allocation
 * fails. The runtime copies what it keeps of `def` (names, methods). */
int mt_objc_add_host_class(mt_objc *rt, const mt_objc_host_class *def, uint32_t cls,
                           uint32_t meta) MT_NOEXCEPT;

/* Canonicalizes every selref, realizes every class of the class list (superclasses first)
 * and attaches the categories. Returns the number of problems (also logged and counted in
 * mt_objc_stats): a superclass that is neither an image class nor a host class, a category
 * on an unknown class, malformed lists. */
uint32_t mt_objc_load_image(mt_objc *rt, const mt_objc_image *image) MT_NOEXCEPT;

/* Replaces (or adds) the instance method (class_method 0) or class method (1) `selector` of
 * the class named `class_name` with `fn`. Returns 0, or -1 if there is no such class. */
int mt_objc_override(mt_objc *rt, const char *class_name, int class_method, const char *selector,
                     mt_guest_fn fn) MT_NOEXCEPT;

mt_objc_stats mt_objc_get_stats(const mt_objc *rt) MT_NOEXCEPT;

/* --- start-up (ADR 0029) ----------------------------------------------------------------- */

/* Records the placed object of every _OBJC_CLASS_$_X / _OBJC_METACLASS_$_X data import, and
 * of the block isa imports __NSConcreteStackBlock / __NSConcreteGlobalBlock (the class
 * objects of __NSStackBlock__ / __NSGlobalBlock__): addrs[id] is what mt_imports_place gave
 * import `id`. mt_objc_add_host_class then uses them for cls / meta 0. Returns the number of
 * class names recorded, or -1 if out of memory. */
int mt_objc_bind_imports(mt_objc *rt, const mt_import_desc *descs, uint32_t count,
                         const uint32_t *addrs) MT_NOEXCEPT;

/* The runtime's host classes, superclasses first: NSObject, NSAutoreleasePool and the block
 * classes (#47), then the framework classes of later issues (runtime/src/objc/classes.cpp,
 * one line per class, like the HLE registry). */
const mt_objc_host_class *const *mt_objc_host_registry(uint32_t *count) MT_NOEXCEPT;

/* mt_objc_add_host_class for every registry class (cls = meta = 0). Returns the number that
 * failed (logged). */
uint32_t mt_objc_add_host_classes(mt_objc *rt) MT_NOEXCEPT;

/* Adds a placeholder host class for every class mt_objc_bind_imports recorded that no host
 * class implements yet: a subclass of NSObject without methods, of the SDK instance size
 * where the image subclasses it (UIView 46, UIViewController 152, else 4). Image classes
 * realize on top of them, and a message to one is an unrecognized selector that names the
 * class (ADR 0029). Needs NSObject. Returns the number added; logs one summary line. */
uint32_t mt_objc_add_placeholders(mt_objc *rt) MT_NOEXCEPT;

/* 1 if `cls` (a class or metaclass object) is a placeholder. */
int mt_objc_class_is_placeholder(const mt_objc *rt, uint32_t cls) MT_NOEXCEPT;

/* The host overrides of ADR 0026 / ADR 0029, each with its reason in classes.cpp:
 * +[Chartboost sharedChartboost] -> nil (StoreKit and GameKit are host classes, #52, ADR
 * 0034). Returns the number that could not be installed (no such class; logged). */
uint32_t mt_objc_install_overrides(mt_objc *rt) MT_NOEXCEPT;

/* The whole ObjC start-up after the binds: mt_objc_bind_imports, mt_objc_add_host_classes,
 * mt_objc_add_placeholders, mt_objc_load_image, mt_objc_install_overrides. Returns the
 * number of problems (each logged): failed host classes, load problems, failed overrides. */
uint32_t mt_objc_start(mt_objc *rt, const mt_import_desc *descs, uint32_t count,
                       const uint32_t *addrs, const mt_objc_image *image) MT_NOEXCEPT;

/* --- selectors and classes --------------------------------------------------------------- */

/* sel_registerName: the canonical SEL of `name`: the image's copy of the string, or one the
 * runtime places for a name only host code uses; 0 if allocation fails. */
uint32_t mt_objc_sel(mt_objc *rt, const char *name) MT_NOEXCEPT;
/* The name of a SEL (canonical or not); NULL if `sel` is no known selector or string. */
const char *mt_objc_sel_name(mt_objc *rt, uint32_t sel) MT_NOEXCEPT;

/* objc_getClass: the guest class object named `name`, or 0. */
uint32_t mt_objc_class_named(const mt_objc *rt, const char *name) MT_NOEXCEPT;
/* The metaclass of a class object, or 0 if `cls` is no class. */
uint32_t mt_objc_metaclass(const mt_objc *rt, uint32_t cls) MT_NOEXCEPT;
/* The superclass of a class or metaclass object, or 0. */
uint32_t mt_objc_superclass(const mt_objc *rt, uint32_t cls) MT_NOEXCEPT;
/* The name of a class or metaclass object, or NULL. */
const char *mt_objc_class_name(const mt_objc *rt, uint32_t cls) MT_NOEXCEPT;
/* Guest instance size of a class (class_ro_t instanceSize or the host size), 0 if unknown. */
uint32_t mt_objc_instance_size(const mt_objc *rt, uint32_t cls) MT_NOEXCEPT;

/* --- lookup ------------------------------------------------------------------------------ */

enum mt_objc_origin {
    MT_OBJC_IMAGE = 1,    /* a method list of an image class */
    MT_OBJC_CATEGORY = 2, /* an image category */
    MT_OBJC_HOST = 3,     /* a host class's method */
    MT_OBJC_OVERRIDE = 4, /* mt_objc_override */
};

typedef struct mt_objc_method {
    uint32_t guest_imp; /* the IMP in the image (Thumb bit set); 0 for host methods */
    mt_guest_fn fn;     /* the host function; NULL for an image IMP not yet dispatched */
    uint32_t origin;    /* enum mt_objc_origin */
    uint32_t owner;     /* the class or metaclass object that defines it */
} mt_objc_method;

/* class_getMethodImplementation: finds `sel` in `cls` (a class object for instance methods,
 * a metaclass object for class methods) and its superclasses. Returns 1 and fills *out, or
 * 0 if no class in the chain implements it. Results are cached per class. */
int mt_objc_lookup(mt_objc *rt, uint32_t cls, uint32_t sel, mt_objc_method *out) MT_NOEXCEPT;

/* --- the message senders (HLE imports of libobjc, mt_import_fn) -------------------------- */

/* Every sender sends +initialize to the receiver's class (superclasses first) before the
 * class's first message, as libobjc does, if the class or a superclass implements it. */

/* objc_msgSend(self, _cmd, ...): nil self returns 0 in r0 and r1. */
void mt_hle_objc_msgSend(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* objc_msgSend_stret(result, self, _cmd, ...): nil self returns without writing *result,
 * as libobjc's armv7 objc_msgSend_stret does. */
void mt_hle_objc_msgSend_stret(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* objc_msgSendSuper2(struct objc_super *, _cmd, ...): struct objc_super { receiver,
 * current class }; the lookup starts at the current class's superclass. */
void mt_hle_objc_msgSendSuper2(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;

/* --- undeliverable messages (#53) -------------------------------------------------------- */

/* One distinct undeliverable message: a (class, selector) pair. */
typedef struct mt_objc_undeliverable {
    const char *class_name; /* the class the lookup started in; NULL: no known class */
    uint32_t class_method;  /* 1: sent to a class object (a class method) */
    const char *selector;   /* "?" if the SEL is no string */
    uint32_t sel;           /* the SEL as first sent */
    uint32_t receiver;      /* the first receiver */
    uint32_t first_caller;  /* lr of the first send (for a message host code sent, the guest
                             * caller of that host code) */
    uint32_t count;         /* sends so far */
} mt_objc_undeliverable;

/* Called for every undeliverable message, after it is recorded and logged and before the
 * policy applies (stop traps right after the hook returns). */
typedef void (*mt_objc_undeliverable_fn)(void *user, const mt_objc_undeliverable *msg);
void mt_objc_set_undeliverable_hook(mt_objc *rt, mt_objc_undeliverable_fn fn,
                                    void *user) MT_NOEXCEPT;

/* The distinct undeliverable messages so far, in the order of their first send. */
uint32_t mt_objc_undeliverable_count(const mt_objc *rt) MT_NOEXCEPT;
/* Fills *out with record i; returns 0, or -1 if i is out of range. */
int mt_objc_undeliverable_at(const mt_objc *rt, uint32_t i, mt_objc_undeliverable *out) MT_NOEXCEPT;

/* --- autorelease pools (the run loop) ---------------------------------------------------- */

/* Pushes an autorelease pool, like a new NSAutoreleasePool; returns its token. The run loop
 * pushes one before each turn and pops it after, so autoreleased objects are released every
 * turn (ADR 0029). */
uint32_t mt_objc_pool_push(mt_objc *rt) MT_NOEXCEPT;
/* Releases every object autoreleased since the push of `token` (pools pushed after it are
 * popped too), newest first, on the runtime's cpu. */
void mt_objc_pool_pop(mt_objc *rt, uint32_t token) MT_NOEXCEPT;
/* mt_objc_pool_pop, releasing on `cpu`: the guest thread that pushed the pool (a GCD worker,
 * maplethief/gcd.h), not the runtime's (main) cpu. */
void mt_objc_pool_pop_on(mt_cpu *cpu, mt_objc *rt, uint32_t token) MT_NOEXCEPT;
/* Objects waiting in all pools. */
uint32_t mt_objc_pool_pending(const mt_objc *rt) MT_NOEXCEPT;

/* Per-thread pool stacks (ADR 0033). Autorelease pools belong to a guest thread. With GCD's
 * worker threads (maplethief/gcd.h) there are several, but only one runs guest code at a
 * time: the runtime holds the running thread's pools, and every other thread's wait in an
 * mt_objc_pools. A switch of guest threads exchanges them with mt_objc_pools_swap (twice
 * restores). */
typedef struct mt_objc_pools mt_objc_pools;
/* An empty pool stack; NULL if out of host memory. */
mt_objc_pools *mt_objc_pools_create(void) MT_NOEXCEPT;
/* Frees the host memory of a pool stack (objects still in it are not released). NULL is
 * ignored. */
void mt_objc_pools_destroy(mt_objc_pools *pools) MT_NOEXCEPT;
/* Exchanges the runtime's current pool stack with *pools. */
void mt_objc_pools_swap(mt_objc *rt, mt_objc_pools *pools) MT_NOEXCEPT;

/* --- libobjc and libSystem imports (mt_import_fn) ---------------------------------------- */

/* id objc_getProperty(id self, SEL _cmd, ptrdiff_t offset, BOOL atomic): an atomic get
 * returns [[value retain] autorelease] (no lock: one guest thread). */
void mt_hle_objc_getProperty(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* void objc_setProperty(id self, SEL _cmd, ptrdiff_t offset, id value, BOOL atomic,
 * signed char shouldCopy): stores [value copyWithZone:NULL] (shouldCopy 1),
 * [value mutableCopyWithZone:NULL] (2) or [value retain], then releases the old value. */
void mt_hle_objc_setProperty(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* int objc_sync_enter(id) / objc_sync_exit(id): a recursive count per object (one guest
 * thread); exit without a matching enter returns -1 (OBJC_SYNC_NOT_OWNING_THREAD_ERROR);
 * nil does nothing and returns 0. */
void mt_hle_objc_sync_enter(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_hle_objc_sync_exit(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* void objc_enumerationMutation(id): logs and stops the guest (MT_TRAP_OBJC, pc = lr,
 * detail = the collection), where libobjc raises an exception the game never catches. */
void mt_hle_objc_enumerationMutation(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* The blocks runtime (libclosure as in iOS 4): void *_Block_copy(const void *),
 * void _Block_release(const void *), void _Block_object_assign(void *dest, const void *obj,
 * int flags), void _Block_object_dispose(const void *obj, int flags). */
void mt_hle_Block_copy(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_hle_Block_release(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_hle_Block_object_assign(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_hle_Block_object_dispose(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_OBJC_OBJC_H */
