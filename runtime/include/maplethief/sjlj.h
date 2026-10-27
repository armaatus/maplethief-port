/* The SjLj exception unwinder of the HLE runtime (#37, ADR 0020).
 *
 * The game's exception handling is setjmp/longjmp based. A function with landing pads keeps a
 * function context on the guest stack, registers it with _Unwind_SjLj_Register, stores the
 * current call-site index into it before each call that may throw and unregisters it on the
 * way out. The guest code does all of that itself; this module implements what the iOS
 * system libraries did for it:
 *
 *   _Unwind_SjLj_Register / _Unregister   the per-thread chain of contexts (guest memory)
 *   _Unwind_SjLj_Resume                   continue unwinding after a cleanup landing pad
 *   __gxx_personality_sj0 /               the personality: reads the function's LSDA from
 *   __objc_personality_v0                 guest memory (docs/formats/lsda.md)
 *   objc_begin_catch / objc_end_catch /   the catch bookkeeping of the two Chartboost @catch
 *   objc_exception_rethrow                blocks
 *   std::terminate                        traps
 *
 * and a raise entry point for HLE code (mt_sjlj_throw). The semantics follow GCC's
 * unwind-sjlj.c and its SjLj personality: two phases (search, then cleanup up to the
 * handler), landing pads installed by writing data[0] (the exception), data[1] (the
 * selector) and call_site (the landing-pad index) into the context and longjmp-ing to its
 * resume point. The host side of that longjmp is the setjmp the generated code takes at the
 * store of the resume address (MT_SJLJ_SETJMP in maplethief/recomp/calls.h): the runtime keeps
 * one host jmp_buf per live guest jump buffer.
 *
 * The game never throws in normal play (ANALYSIS.md, "Exceptions"): no guest code calls
 * __cxa_throw or objc_exception_throw. Everything that can go wrong here traps loudly:
 * MT_TRAP_UNCAUGHT (no handler), MT_TRAP_TERMINATE (std::terminate, an exception specification
 * or call_site 0) and MT_TRAP_SJLJ (unknown personality, a call site the LSDA does not have, a
 * context with no host resume point, a full table).
 *
 * All guest memory goes through the cpu pointer the guest code passed in (the restrict
 * contract of ADR 0014). C17; compiles as C++20 too. */
#ifndef MAPLETHIEF_SJLJ_H
#define MAPLETHIEF_SJLJ_H

#include <setjmp.h>
#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* The function context in guest memory (offsets; measured from the binary, see
 * recomp/exceptions.py): prev, call_site, data[4], personality, lsda, then the jump buffer
 * of LLVM's __builtin_setjmp: r7, the resume address, sp. */
#define MT_SJLJ_FC_PREV 0x00u
#define MT_SJLJ_FC_CALL_SITE 0x04u
#define MT_SJLJ_FC_DATA 0x08u
#define MT_SJLJ_FC_PERSONALITY 0x18u
#define MT_SJLJ_FC_LSDA 0x1Cu
#define MT_SJLJ_FC_JBUF 0x20u

#ifndef MT_SJLJ_MAX_FRAMES
#define MT_SJLJ_MAX_FRAMES 64 /* host jump buffers: SjLj frames alive at the same time */
#endif
#ifndef MT_SJLJ_MAX_EXCEPTIONS
#define MT_SJLJ_MAX_EXCEPTIONS 8 /* exceptions in flight or being handled */
#endif

/* Whether the catch clause for `catch_type` catches an exception of `thrown_type` (guest
 * addresses of type_info objects or ObjC classes; 0 never reaches it, it is catch-all).
 * `object` is the thrown object's address, or NULL when only the answer matters (exception
 * specifications): a match through a base class at a non-zero offset moves it to that base
 * subobject, which the catch clause then receives (GCC's adjustedPtr). The C++ matcher is
 * mt_cxx_can_catch (maplethief/hle/cxxabi.h, #45), which the boot installs. */
typedef int (*mt_sjlj_can_catch_fn)(mt_cpu *cpu, uint32_t catch_type, uint32_t thrown_type,
                                    uint32_t *object);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) /* x64's jmp_buf is 16-byte aligned: the padding is expected */
#endif

typedef struct mt_sjlj_frame {
    uint32_t jbuf; /* guest address of jbuf[0]; 0: free */
    jmp_buf host;
} mt_sjlj_frame;

typedef struct mt_sjlj_exception {
    uint32_t ue;         /* guest address of its _Unwind_Exception (data[0]); 0: free */
    uint32_t type;       /* thrown type (0: matches only catch-all) */
    uint32_t object;     /* the thrown object */
    uint32_t adjusted;   /* what begin_catch returns: the object as the handler phase 1 found
                            sees it (adjusted to a base subobject by can_catch) */
    uint32_t handler_fc; /* the context phase 1 found */
    int32_t handlers;    /* begin_catch minus end_catch */
    uint32_t rethrown;
} mt_sjlj_exception;

typedef struct mt_sjlj_state {
    uint32_t top;                   /* innermost registered context (guest address), 0: none */
    uint32_t gxx_personality;       /* guest address the runtime gave __gxx_personality_sj0 */
    uint32_t objc_personality;      /* ... and __objc_personality_v0 (0: not bound) */
    mt_sjlj_can_catch_fn can_catch; /* NULL: the types must be equal */
    /* ue of the exceptions being handled, innermost last */
    uint32_t caught[MT_SJLJ_MAX_EXCEPTIONS];
    uint32_t caught_count;
    mt_sjlj_exception exceptions[MT_SJLJ_MAX_EXCEPTIONS];
    mt_sjlj_frame frames[MT_SJLJ_MAX_FRAMES];
} mt_sjlj_state;

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* The unwinder state of the guest thread `cpu` belongs to. sjlj.c has one state for the one
 * guest thread the game runs (it creates no threads, ANALYSIS.md); a runtime with more
 * threads keeps one per mt_cpu instead. A zero-initialised state is a valid empty one
 * (mt_sjlj_init resets it). */
mt_sjlj_state *mt_sjlj_state_of(mt_cpu *cpu) MT_NOEXCEPT;

void mt_sjlj_init(mt_sjlj_state *s) MT_NOEXCEPT;

/* Tell the unwinder which guest addresses the personality imports are bound to. */
void mt_sjlj_set_personalities(mt_sjlj_state *s, uint32_t gxx, uint32_t objc) MT_NOEXCEPT;

/* The unwinder (fc, ue: guest addresses). */
void mt_sjlj_register(mt_cpu *cpu, uint32_t fc) MT_NOEXCEPT;
void mt_sjlj_unregister(mt_cpu *cpu, uint32_t fc) MT_NOEXCEPT;
MT_NORETURN void mt_sjlj_resume(mt_cpu *cpu, uint32_t ue) MT_NOEXCEPT;

/* Raise an exception from HLE code. `ue` is a guest address that identifies it (the value
 * landing pads receive in data[0]; the caller owns that memory), `type` the thrown type and
 * `object` what a catch clause receives from begin_catch (moved to the base subobject when a
 * handler catches it by a base class, see mt_sjlj_can_catch_fn). Never returns: it installs a
 * landing pad or traps (MT_TRAP_UNCAUGHT when nothing handles it, before any cleanup runs). */
MT_NORETURN void mt_sjlj_throw(mt_cpu *cpu, uint32_t ue, uint32_t type,
                               uint32_t object) MT_NOEXCEPT;
uint32_t mt_sjlj_begin_catch(mt_cpu *cpu, uint32_t ue) MT_NOEXCEPT;
void mt_sjlj_end_catch(mt_cpu *cpu) MT_NOEXCEPT;
MT_NORETURN void mt_sjlj_rethrow(mt_cpu *cpu) MT_NOEXCEPT;

/* Import handlers (mt_import_fn, the argument in cpu->r[0]): runtime/src/hle/registry.cpp
 * registers them under __Unwind_SjLj_Register, __Unwind_SjLj_Unregister,
 * __Unwind_SjLj_Resume, _objc_begin_catch, _objc_end_catch, _objc_exception_rethrow and
 * __ZSt9terminatev, and mt_imports_link binds them by name (ADR 0019). */
void mt_sjlj_hle_register(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_sjlj_hle_unregister(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
MT_NORETURN void mt_sjlj_hle_resume(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_sjlj_hle_objc_begin_catch(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_sjlj_hle_objc_end_catch(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
MT_NORETURN void mt_sjlj_hle_objc_exception_rethrow(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
MT_NORETURN void mt_sjlj_hle_terminate(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;

/* The personality routines are function imports without a stub: the guest only stores
 * their addresses (thunk addresses, MT_IMPORT_THUNK_BASE + 4 * id) in its contexts. This
 * looks up ___gxx_personality_sj0 and ___objc_personality_v0 in the generated import
 * descriptors and passes their addresses to mt_sjlj_set_personalities. */
void mt_sjlj_bind_personalities(mt_sjlj_state *s, const mt_import_desc *descs,
                                uint32_t count) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_SJLJ_H */
