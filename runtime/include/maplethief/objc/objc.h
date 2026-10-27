/* The Objective-C runtime over the guest's own metadata (ADR 0026; #46 prototype, #47 grows
 * it into the full runtime).
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
 *                  host functions with the same signature as a recompiled function.
 *
 * Categories attach to either kind. A host override replaces one method of any class with a
 * host function (Chartboost's +sharedChartboost, the StoreKit and Game Center availability
 * checks). Every method, guest or host, is entered the same way: r0 = self, r1 = _cmd, the
 * other arguments per maplethief/hle/abi.h, lr = the sender's return address.
 *
 * Start-up (#43): mt_objc_create, mt_objc_add_host_class for every host class (with the
 * addresses of its placed import objects), mt_objc_load_image with the image's ObjC sections
 * (canonicalizes __objc_selrefs, realizes every class, attaches the categories), then
 * mt_objc_override for each host override, and mt_objc_set_current. The objc_msgSend family
 * of HLE functions below uses the current runtime.
 *
 * Prototype scope (#46): metadata, selectors, lookup, caches, overrides and the message
 * senders. Not yet here (#47): alloc/retain/release/autorelease, properties, @synchronized,
 * blocks, +initialize, ivar sliding (only detected: see mt_objc_stats.instance_start_problems).
 * Guest memory comes from a caller-provided allocator; the runtime uses #42's guest heap.
 *
 * Plain C++ without exceptions, `new` or function-local statics (like the import code,
 * ADR 0019), so the recompiler's C test harness can link it into a C program. */
#ifndef MAPLETHIEF_OBJC_OBJC_H
#define MAPLETHIEF_OBJC_OBJC_H

#include <stdint.h>

#include "maplethief/hle/imports.h"
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
     * guest object, never in these bytes. */
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
     * libobjc would slide their ivars; the prototype only counts them. */
    uint32_t instance_start_problems;
    uint32_t problems; /* everything mt_objc_load_image logged as a problem */
} mt_objc_stats;

/* `cpu` gives access to guest memory (cpu->mem / mem_size) for the runtime's lifetime;
 * `alloc` places guest objects the runtime creates (host class objects not bound by the
 * image, selector strings registered by host code); `cfg` (NULL: the default) gives the
 * log sink and the policy for unrecognized selectors, shared with unimplemented imports
 * (MAPLETHIEF_UNIMPLEMENTED_IMPORT). NULL if out of memory. */
mt_objc *mt_objc_create(mt_cpu *cpu, mt_guest_alloc_fn alloc, void *alloc_user,
                        const mt_import_config *cfg) MT_NOEXCEPT;
void mt_objc_destroy(mt_objc *rt) MT_NOEXCEPT;

/* The runtime the objc_msgSend family uses. */
void mt_objc_set_current(mt_objc *rt) MT_NOEXCEPT;
mt_objc *mt_objc_current(void) MT_NOEXCEPT;

/* Adds a host class. `cls` and `meta` are the guest addresses of its imported class and
 * metaclass objects (0: the runtime allocates a 20-byte object). Writes isa and superclass
 * into both objects. Returns 0, or -1 (and logs) if the name is taken, the superclass is
 * unknown or allocation fails. */
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

/* objc_msgSend(self, _cmd, ...): nil self returns 0 in r0 and r1. */
void mt_hle_objc_msgSend(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* objc_msgSend_stret(result, self, _cmd, ...): nil self returns without writing *result,
 * as libobjc's armv7 objc_msgSend_stret does. */
void mt_hle_objc_msgSend_stret(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* objc_msgSendSuper2(struct objc_super *, _cmd, ...): struct objc_super { receiver,
 * current class }; the lookup starts at the current class's superclass. */
void mt_hle_objc_msgSendSuper2(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_OBJC_OBJC_H */
