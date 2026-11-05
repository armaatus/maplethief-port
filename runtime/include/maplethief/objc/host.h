/* Writing host classes: the framework classes (Foundation #49, UIImage/UIFont #51, the stubs
 * #52, UIKit #54) that guest code sees as ordinary Objective-C classes (ADR 0026, ADR 0029).
 *
 * A host class is an mt_objc_host_class (maplethief/objc/objc.h): a name, a superclass, the
 * guest instance size, and tables of host methods. A host method is an mt_guest_fn,
 * `void m(mt_cpu *cpu)`, entered like any method: r0 = self, r1 = _cmd, the other arguments
 * per the iOS armv7 convention (maplethief/hle/abi.h), the result in r0 (r0:r1 for 64 bits)
 * or, for a structure larger than 4 bytes, through objc_msgSend_stret's hidden pointer.
 *
 *     // -[Counter addValue:(int)v times:(int)n] -> int
 *     static void counter_add(mt_cpu *cpu) {
 *         mt_objc *rt = mt_objc_current();
 *         mt_args a = mt_objc_args(cpu);         // the cursor starts after self and _cmd
 *         int32_t v = mt_arg_i32(&a), n = mt_arg_i32(&a);
 *         counter_state *s = (counter_state *)mt_objc_state(rt, mt_objc_self(cpu), &k_counter);
 *         s->total += v * n;
 *         mt_ret_u32(cpu, (uint32_t)s->total);
 *     }
 *     // -[Counter frame] -> CGRect (16 bytes: sent with objc_msgSend_stret)
 *     static void counter_frame(mt_cpu *cpu) {
 *         uint32_t result, self;
 *         mt_args a = mt_objc_args_stret(cpu, &result, &self);
 *         float rect[4] = {0, 0, 320, 480};
 *         (void)a;
 *         mt_ret_struct(cpu, result, rect, sizeof rect);
 *     }
 *     static const mt_objc_host_method k_counter_methods[] = {
 *         {"addValue:times:", counter_add}, {"frame", counter_frame}};
 *     const mt_objc_host_class k_counter_class = {
 *         "Counter", "NSObject", 4, k_counter_methods, 2, NULL, 0};
 *
 * and one line in runtime/src/objc/classes.cpp registers it. tests/cpp/objc_services_test.cpp has
 * this example as a working class.
 *
 * Rules:
 *   - Read every argument before sending a message or calling guest code: both clobber
 *     r0-r3 (and the result registers).
 *   - Host state lives in the side table below, keyed by the guest object, never in guest
 *     bytes: a host class's instance size is the SDK size the image's subclasses were
 *     compiled against (NSObject 4, UIView 46, UIViewController 152), so image subclasses'
 *     ivars follow it.
 *   - Retain and release guest objects through messages (mt_objc_retain/_release below), so
 *     that classes overriding them (EAGLView_Instance, blocks) keep working.
 *   - A selector a host class does not implement is an unrecognized selector (logged, and
 *     the MAPLETHIEF_UNIMPLEMENTED_IMPORT policy applies), never a silent no-op: implement
 *     exactly what game code sends (#11's lists).
 *
 * C17 that also compiles as C++20; the functions are in runtime/src/objc/. */
#ifndef MAPLETHIEF_OBJC_HOST_H
#define MAPLETHIEF_OBJC_HOST_H

#include <stdint.h>

#include "maplethief/hle/abi.h"
#include "maplethief/objc/objc.h"

MT_EXTERN_C_BEGIN

/* --- arguments ---------------------------------------------------------------------------- */

/* self and _cmd of a method sent with objc_msgSend or objc_msgSendSuper2. */
MT_INLINE uint32_t mt_objc_self(const mt_cpu *cpu) { return cpu->r[0]; }
MT_INLINE uint32_t mt_objc_cmd(const mt_cpu *cpu) { return cpu->r[1]; }

/* The method's arguments after self and _cmd (iOS rules: 64-bit values 4-byte aligned). */
MT_INLINE mt_args mt_objc_args(mt_cpu *cpu) {
    mt_args a = mt_args_begin(cpu);
    a.ncrn = 2u;
    return a;
}

/* A method sent with objc_msgSend_stret (a structure result larger than 4 bytes): *result
 * gets the hidden result pointer (r0), *self the receiver (r1); the cursor starts after
 * _cmd (r2). Write the result with mt_ret_struct(cpu, *result, ...). */
MT_INLINE mt_args mt_objc_args_stret(mt_cpu *cpu, uint32_t *result, uint32_t *self) {
    mt_args a = mt_args_begin(cpu);
    *result = cpu->r[0];
    *self = cpu->r[1];
    a.ncrn = 3u;
    return a;
}

/* --- sending messages from host code ------------------------------------------------------ */
/* Each sends on `cpu` like objc_msgSend from guest code: +initialize, nil receivers (0), the
 * undeliverable policy. `args` are the word arguments after _cmd (any number: those past r3
 * go on the guest stack below sp). They return r0; a 64-bit result's high word is in
 * cpu->r[1]. lr and sp are preserved; r0-r3 and r12 are not. */

/* [self sel args...]; `sel` is a SEL (mt_objc_sel). */
uint32_t mt_objc_msg(mt_cpu *cpu, uint32_t self, uint32_t sel, const uint32_t *args,
                     uint32_t argc) MT_NOEXCEPT;

/* [super sel args...] as written in a method of class `cls`: the lookup starts at the
 * superclass of `cls` (a class object, or a metaclass object for a class method). */
uint32_t mt_objc_msg_super(mt_cpu *cpu, uint32_t self, uint32_t cls, uint32_t sel,
                           const uint32_t *args, uint32_t argc) MT_NOEXCEPT;

/* A message with a structure result of more than 4 bytes (objc_msgSend_stret): the method
 * writes it to guest address `result`; a nil receiver leaves it unwritten. */
void mt_objc_msg_stret(mt_cpu *cpu, uint32_t result, uint32_t self, uint32_t sel,
                       const uint32_t *args, uint32_t argc) MT_NOEXCEPT;

/* Shorthands by selector name, on the current runtime. */
MT_INLINE uint32_t mt_objc_send0(mt_cpu *cpu, uint32_t self, const char *selector) {
    return mt_objc_msg(cpu, self, mt_objc_sel(mt_objc_current(), selector), NULL, 0u);
}
MT_INLINE uint32_t mt_objc_send1(mt_cpu *cpu, uint32_t self, const char *selector, uint32_t a0) {
    return mt_objc_msg(cpu, self, mt_objc_sel(mt_objc_current(), selector), &a0, 1u);
}
MT_INLINE uint32_t mt_objc_send2(mt_cpu *cpu, uint32_t self, const char *selector, uint32_t a0,
                                 uint32_t a1) {
    uint32_t args[2];
    args[0] = a0;
    args[1] = a1;
    return mt_objc_msg(cpu, self, mt_objc_sel(mt_objc_current(), selector), args, 2u);
}

/* --- objects ------------------------------------------------------------------------------ */

/* [obj retain], [obj release], [obj autorelease] (nil: nothing, returns 0). */
uint32_t mt_objc_retain(mt_cpu *cpu, uint32_t obj) MT_NOEXCEPT;
void mt_objc_release(mt_cpu *cpu, uint32_t obj) MT_NOEXCEPT;
uint32_t mt_objc_autorelease(mt_cpu *cpu, uint32_t obj) MT_NOEXCEPT;

/* class_createInstance: a zeroed instance of class `cls` with its guest instance size, isa
 * set, retain count 1; no message is sent (+allocWithZone: of NSObject uses it). 0 if `cls`
 * is no class or the heap is full. */
uint32_t mt_objc_create_instance(mt_objc *rt, uint32_t cls) MT_NOEXCEPT;

/* object_getClass: the isa of `obj` (for a class object, its metaclass), 0 for nil. */
uint32_t mt_objc_class_of(const mt_objc *rt, uint32_t obj) MT_NOEXCEPT;

/* [obj isKindOfClass:cls]: 1 if the isa of `obj` is `cls` or a subclass of it. */
int mt_objc_is_kind_of(const mt_objc *rt, uint32_t obj, uint32_t cls) MT_NOEXCEPT;

/* The retain count of `obj` (-retainCount of NSObject's side table; 0 for nil). */
uint32_t mt_objc_retain_count(const mt_objc *rt, uint32_t obj) MT_NOEXCEPT;

/* --- host state ------------------------------------------------------------------------- */
/* A host class keeps its per-object state in host memory: a block of `size` bytes per
 * (object, type), zeroed when created, freed after `destroy` when the object is deallocated
 * (-[NSObject dealloc], i.e. after the subclasses' -dealloc ran). Each class of a host
 * hierarchy can have its own type (UIView's state and UILabel's state of one object are two
 * entries). Host pointers to state stay valid until the object is deallocated. */

typedef struct mt_objc_state_type {
    const char *name; /* for logs */
    uint32_t size;    /* bytes of host state */
    /* Runs at dealloc with the state, before its memory is freed; may send messages (e.g.
     * release the guest objects the state holds). NULL: nothing to do. */
    void (*destroy)(mt_cpu *cpu, uint32_t obj, void *state);
} mt_objc_state_type;

/* The state of `type` for `obj`, created (zeroed) on first use; NULL if out of memory. */
void *mt_objc_state(mt_objc *rt, uint32_t obj, const mt_objc_state_type *type) MT_NOEXCEPT;
/* The state of `type` for `obj` if it exists, else NULL. */
void *mt_objc_find_state(const mt_objc *rt, uint32_t obj,
                         const mt_objc_state_type *type) MT_NOEXCEPT;
/* Calls fn(user, obj, type, state) for every live host state, in table order: the census of
 * the host side that a save state has to carry (#190, ADR 0105). fn must not create or free
 * states. */
void mt_objc_each_state(const mt_objc *rt,
                        void (*fn)(void *user, uint32_t obj, const mt_objc_state_type *type,
                                   const void *state),
                        void *user) MT_NOEXCEPT;
/* Objects with a retain count above 1, and autoreleased objects not yet released: two more
 * host-side tables a save state carries. */
void mt_objc_ref_counts(const mt_objc *rt, uint32_t *retained, uint32_t *autoreleased) MT_NOEXCEPT;

/* --- blocks ------------------------------------------------------------------------------- */
/* For host methods that take a block (completion handlers, #52 GameKit): keep it with
 * mt_block_copy, call it with mt_block_invoke, drop it with mt_block_release. */

/* _Block_copy: a heap copy of a stack block (running its copy helper), the same block with
 * one more reference for a heap block, the block itself for a global block; 0 for nil. */
uint32_t mt_block_copy(mt_cpu *cpu, uint32_t block) MT_NOEXCEPT;
/* _Block_release: drops one reference of a heap block; at the last one, runs its dispose
 * helper and frees it. Stack and global blocks: nothing. */
void mt_block_release(mt_cpu *cpu, uint32_t block) MT_NOEXCEPT;
/* Calls the block: block->invoke(block, args...), with at most 3 word arguments. Returns r0. */
uint32_t mt_block_invoke(mt_cpu *cpu, uint32_t block, const uint32_t *args,
                         uint32_t argc) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_OBJC_HOST_H */
