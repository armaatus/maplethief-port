/* The SjLj exception unwinder of the HLE runtime; see maplethief/sjlj.h and ADR 0020.
 *
 * GCC's unwind-sjlj.c and the SjLj variant of its C++ personality, reimplemented over guest
 * memory. The LSDA format is docs/formats/lsda.md; recomp/exceptions.py parses the same
 * tables offline. */
#include "maplethief/sjlj.h"

#include <string.h>

enum { FOUND_NOTHING, FOUND_CLEANUP, FOUND_HANDLER, FOUND_TERMINATE };

#define PE_OMIT 0xFFu
#define PE_ULEB128 0x01u
#define PE_UDATA2 0x02u
#define PE_UDATA4 0x03u
#define PE_SLEB128 0x09u
#define PE_SDATA2 0x0Au
#define PE_SDATA4 0x0Bu
#define PE_PCREL 0x10u
#define PE_INDIRECT 0x80u

#define CHAIN_LIMIT 100000u /* contexts on one chain: more means a cycle */

/* The guest PC a fault or trap in here reports: the caller of the import. */
static uint32_t site(const mt_cpu *cpu) { return cpu->r[MT_LR]; }

static uint32_t ld8(mt_cpu *cpu, uint32_t addr) { return mt_ld8(cpu, addr, site(cpu)); }
static uint32_t ld32(mt_cpu *cpu, uint32_t addr) { return mt_ld32(cpu, addr, site(cpu)); }
static void st32(mt_cpu *cpu, uint32_t addr, uint32_t value) {
    mt_st32(cpu, addr, value, site(cpu));
}

MT_NORETURN static void fail(mt_cpu *cpu, uint32_t kind, uint32_t detail) {
    mt_trap(cpu, kind, site(cpu), detail);
}

/* -- LSDA reading -------------------------------------------------------------------------- */

static uint32_t uleb(mt_cpu *cpu, uint32_t *p) {
    uint32_t value = 0, shift = 0, b;
    do {
        b = ld8(cpu, (*p)++);
        if (shift < 32u) {
            value |= (b & 0x7Fu) << shift;
        }
        shift += 7u;
    } while (b & 0x80u);
    return value;
}

static int32_t sleb(mt_cpu *cpu, uint32_t *p) {
    uint32_t value = 0, shift = 0, b;
    do {
        b = ld8(cpu, (*p)++);
        if (shift < 32u) {
            value |= (b & 0x7Fu) << shift;
        }
        shift += 7u;
    } while (b & 0x80u);
    if (shift < 32u && (b & 0x40u)) {
        value |= ~0u << shift;
    }
    return (int32_t)value;
}

/* GCC's read_encoded_value: a raw 0 stays 0 (no pc-relative or indirect adjustment). */
static uint32_t encoded(mt_cpu *cpu, uint32_t enc, uint32_t *p) {
    uint32_t at = *p, raw = 0u;
    switch (enc & 0x0Fu) {
    case 0x00u: /* absptr */
    case PE_UDATA4:
    case PE_SDATA4:
        raw = ld32(cpu, at);
        *p += 4u;
        break;
    case PE_UDATA2:
        raw = ld8(cpu, at) | (ld8(cpu, at + 1u) << 8);
        *p += 2u;
        break;
    case PE_SDATA2:
        raw = ld8(cpu, at) | (ld8(cpu, at + 1u) << 8);
        raw = (raw ^ 0x8000u) - 0x8000u;
        *p += 2u;
        break;
    case PE_ULEB128:
        raw = uleb(cpu, p);
        break;
    case PE_SLEB128:
        raw = (uint32_t)sleb(cpu, p);
        break;
    default:
        fail(cpu, MT_TRAP_SJLJ, enc);
    }
    if (raw == 0u) {
        return 0u;
    }
    if ((enc & 0x70u) == PE_PCREL) {
        raw += at;
    } else if ((enc & 0x70u) != 0u) {
        fail(cpu, MT_TRAP_SJLJ, enc);
    }
    if (enc & PE_INDIRECT) {
        raw = ld32(cpu, raw);
    }
    return raw;
}

static uint32_t entry_size(mt_cpu *cpu, uint32_t enc) {
    switch (enc & 0x0Fu) {
    case 0x00u:
    case PE_UDATA4:
    case PE_SDATA4:
        return 4u;
    case PE_UDATA2:
    case PE_SDATA2:
        return 2u;
    default:
        fail(cpu, MT_TRAP_SJLJ, enc);
    }
}

typedef struct lsda_info {
    uint32_t ttype_enc;
    uint32_t ttype_base; /* 0: no type table */
    uint32_t call_sites; /* first call-site record */
    uint32_t actions;    /* the action table (= end of the call-site table) */
} lsda_info;

static void lsda_header(mt_cpu *cpu, uint32_t lsda, lsda_info *info) {
    uint32_t p = lsda, enc, len;
    enc = ld8(cpu, p++);
    if (enc != PE_OMIT) {
        (void)encoded(cpu, enc, &p); /* LPStart: SjLj landing pads are indices, not addresses */
    }
    info->ttype_enc = ld8(cpu, p++);
    info->ttype_base = 0u;
    if (info->ttype_enc != PE_OMIT) {
        uint32_t off = uleb(cpu, &p);
        info->ttype_base = p + off;
    }
    (void)ld8(cpu, p++); /* call-site encoding: SjLj records are ULEB128 whatever it says */
    len = uleb(cpu, &p);
    info->call_sites = p;
    info->actions = p + len;
}

static uint32_t type_at(mt_cpu *cpu, const lsda_info *info, uint32_t index) {
    uint32_t p;
    if (info->ttype_base == 0u) {
        fail(cpu, MT_TRAP_SJLJ, index);
    }
    p = info->ttype_base - index * entry_size(cpu, info->ttype_enc);
    return encoded(cpu, info->ttype_enc, &p);
}

static int catches(mt_cpu *cpu, const mt_sjlj_state *s, uint32_t catch_type, uint32_t thrown,
                   uint32_t *object) {
    if (catch_type == 0u) {
        return 1; /* catch (...) */
    }
    if (s->can_catch != NULL) {
        return s->can_catch(cpu, catch_type, thrown, object);
    }
    return catch_type == thrown;
}

/* Whether the exception specification at `filter` (< 0) allows the thrown type. */
static int spec_allows(mt_cpu *cpu, const mt_sjlj_state *s, const lsda_info *info, int32_t filter,
                       uint32_t thrown) {
    uint32_t p = info->ttype_base + (uint32_t)(-(filter + 1)), index;
    if (info->ttype_base == 0u) {
        fail(cpu, MT_TRAP_SJLJ, (uint32_t)filter);
    }
    while ((index = uleb(cpu, &p)) != 0u) {
        if (catches(cpu, s, type_at(cpu, info, index), thrown, NULL)) {
            return 1;
        }
    }
    return 0;
}

/* The personality for context `fc`: what a throw of `e` finds there, with the landing pad
 * and the selector to install. A catch clause it finds sets e->adjusted. */
static int scan(mt_cpu *cpu, const mt_sjlj_state *s, uint32_t fc, mt_sjlj_exception *e,
                uint32_t *landing_pad, int32_t *selector) {
    int32_t cs = (int32_t)ld32(cpu, fc + MT_SJLJ_FC_CALL_SITE);
    uint32_t personality, lsda, p, n, lp = 0u, action = 0u;
    int saw_cleanup = 0;
    lsda_info info;
    *landing_pad = 0u;
    *selector = 0;
    if (cs < 0) {
        return FOUND_NOTHING; /* -1: the call is not in a region with a landing pad */
    }
    if (cs == 0) {
        return FOUND_TERMINATE;
    }
    personality = ld32(cpu, fc + MT_SJLJ_FC_PERSONALITY);
    if (personality == 0u ||
        (personality != s->gxx_personality && personality != s->objc_personality)) {
        fail(cpu, MT_TRAP_SJLJ, personality);
    }
    lsda = ld32(cpu, fc + MT_SJLJ_FC_LSDA);
    if (lsda == 0u) {
        return FOUND_NOTHING;
    }
    lsda_header(cpu, lsda, &info);
    p = info.call_sites;
    for (n = (uint32_t)cs; n != 0u; n--) {
        if (p >= info.actions) {
            fail(cpu, MT_TRAP_SJLJ, (uint32_t)cs); /* the LSDA has no such call site */
        }
        lp = uleb(cpu, &p);
        action = uleb(cpu, &p);
    }
    *landing_pad = lp;
    if (action == 0u) {
        return FOUND_CLEANUP;
    }
    p = info.actions + action - 1u;
    for (;;) {
        uint32_t next;
        int32_t filter = sleb(cpu, &p), disp;
        next = p;
        disp = sleb(cpu, &p);
        if (filter == 0) {
            saw_cleanup = 1;
        } else if (filter > 0) {
            uint32_t object = e->object;
            if (catches(cpu, s, type_at(cpu, &info, (uint32_t)filter), e->type, &object)) {
                e->adjusted = object;
                *selector = filter;
                return FOUND_HANDLER;
            }
        } else if (!spec_allows(cpu, s, &info, filter, e->type)) {
            *selector = filter; /* the landing pad calls std::terminate (unexpected) */
            return FOUND_HANDLER;
        }
        if (disp == 0) {
            break;
        }
        p = next + (uint32_t)disp;
    }
    return saw_cleanup ? FOUND_CLEANUP : FOUND_NOTHING;
}

/* -- host jump buffers ------------------------------------------------------------------------ */

static mt_sjlj_frame *frame_of(mt_sjlj_state *s, uint32_t jbuf) {
    uint32_t i;
    for (i = 0; i < MT_SJLJ_MAX_FRAMES; i++) {
        if (s->frames[i].jbuf == jbuf) {
            return &s->frames[i];
        }
    }
    return NULL;
}

static void release(mt_sjlj_state *s, uint32_t fc) {
    mt_sjlj_frame *f = frame_of(s, fc + MT_SJLJ_FC_JBUF);
    if (f != NULL) {
        f->jbuf = 0u;
    }
}

static int registered(mt_cpu *cpu, const mt_sjlj_state *s, uint32_t jbuf) {
    uint32_t fc = s->top, n = 0;
    while (fc != 0u) {
        if (fc + MT_SJLJ_FC_JBUF == jbuf) {
            return 1;
        }
        if (++n > CHAIN_LIMIT) {
            fail(cpu, MT_TRAP_SJLJ, fc);
        }
        fc = ld32(cpu, fc + MT_SJLJ_FC_PREV);
    }
    return 0;
}

jmp_buf *mt_sjlj_jmpbuf(mt_cpu *cpu, uint32_t guest_jbuf) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    mt_sjlj_frame *f = frame_of(s, guest_jbuf);
    uint32_t i;
    if (guest_jbuf == 0u) {
        fail(cpu, MT_TRAP_SJLJ, guest_jbuf);
    }
    if (f == NULL) {
        f = frame_of(s, 0u);
    }
    if (f == NULL) {
        /* Full: drop the buffers of frames that are gone without unregistering (none in this
         * game, but a frame is only known to be gone once its context is off the chain). */
        for (i = 0; i < MT_SJLJ_MAX_FRAMES; i++) {
            if (!registered(cpu, s, s->frames[i].jbuf)) {
                s->frames[i].jbuf = 0u;
            }
        }
        f = frame_of(s, 0u);
        if (f == NULL) {
            fail(cpu, MT_TRAP_SJLJ, guest_jbuf);
        }
    }
    f->jbuf = guest_jbuf;
    return &f->host;
}

/* -- the unwinder -------------------------------------------------------------------------- */

void mt_sjlj_init(mt_sjlj_state *s) { memset(s, 0, sizeof *s); }

void mt_sjlj_set_personalities(mt_sjlj_state *s, uint32_t gxx, uint32_t objc) {
    s->gxx_personality = gxx;
    s->objc_personality = objc;
}

void mt_sjlj_register(mt_cpu *cpu, uint32_t fc) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    st32(cpu, fc + MT_SJLJ_FC_PREV, s->top);
    s->top = fc;
}

void mt_sjlj_unregister(mt_cpu *cpu, uint32_t fc) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    s->top = ld32(cpu, fc + MT_SJLJ_FC_PREV);
    release(s, fc);
}

static mt_sjlj_exception *find(mt_cpu *cpu, mt_sjlj_state *s, uint32_t ue) {
    uint32_t i;
    for (i = 0; i < MT_SJLJ_MAX_EXCEPTIONS; i++) {
        if (ue != 0u && s->exceptions[i].ue == ue) {
            return &s->exceptions[i];
        }
    }
    fail(cpu, MT_TRAP_SJLJ, ue);
}

/* Install the landing pad: data[0] = the exception, data[1] = the selector, call_site = the
 * landing-pad index; pop the contexts above fc (GCC's _Unwind_SjLj_SetContext), restore r7
 * and sp from the guest jump buffer and longjmp to the frame's resume point. */
MT_NORETURN static void install(mt_cpu *cpu, mt_sjlj_state *s, uint32_t fc,
                                const mt_sjlj_exception *e, uint32_t lp, int32_t selector) {
    uint32_t c = s->top, n = 0;
    mt_sjlj_frame *f;
    st32(cpu, fc + MT_SJLJ_FC_DATA, e->ue);
    st32(cpu, fc + MT_SJLJ_FC_DATA + 4u, (uint32_t)selector);
    st32(cpu, fc + MT_SJLJ_FC_CALL_SITE, lp);
    while (c != fc && c != 0u) {
        release(s, c);
        if (++n > CHAIN_LIMIT) {
            fail(cpu, MT_TRAP_SJLJ, c);
        }
        c = ld32(cpu, c + MT_SJLJ_FC_PREV);
    }
    s->top = fc;
    f = frame_of(s, fc + MT_SJLJ_FC_JBUF);
    if (f == NULL) {
        fail(cpu, MT_TRAP_SJLJ, fc); /* the function has no resume hook */
    }
    cpu->r[7] = ld32(cpu, fc + MT_SJLJ_FC_JBUF);
    cpu->r[MT_SP] = ld32(cpu, fc + MT_SJLJ_FC_JBUF + 8u);
    MT_HOST_LONGJMP(f->host, 1);
}

/* Phase 2 from context `fc`: run each cleanup and stop at the handler phase 1 found. */
MT_NORETURN static void phase2(mt_cpu *cpu, mt_sjlj_state *s, mt_sjlj_exception *e, uint32_t fc) {
    uint32_t n = 0;
    while (fc != 0u) {
        uint32_t lp;
        int32_t selector;
        int found = scan(cpu, s, fc, e, &lp, &selector);
        if (found == FOUND_TERMINATE) {
            fail(cpu, MT_TRAP_TERMINATE, e->type);
        }
        if (fc == e->handler_fc && found != FOUND_HANDLER) {
            fail(cpu, MT_TRAP_SJLJ, fc); /* the handler is gone since phase 1 */
        }
        if (found == FOUND_HANDLER || found == FOUND_CLEANUP) {
            install(cpu, s, fc, e, lp, found == FOUND_HANDLER ? selector : 0);
        }
        if (++n > CHAIN_LIMIT) {
            fail(cpu, MT_TRAP_SJLJ, fc);
        }
        fc = ld32(cpu, fc + MT_SJLJ_FC_PREV);
    }
    fail(cpu, MT_TRAP_SJLJ, e->ue); /* ran off the chain: Resume without a handler */
}

/* Phase 1: find the handler; with none, nothing is unwound (std::terminate semantics). */
MT_NORETURN static void raise_exception(mt_cpu *cpu, mt_sjlj_state *s, mt_sjlj_exception *e) {
    uint32_t fc = s->top, n = 0;
    while (fc != 0u) {
        uint32_t lp;
        int32_t selector;
        int found = scan(cpu, s, fc, e, &lp, &selector);
        if (found == FOUND_HANDLER || found == FOUND_TERMINATE) {
            break;
        }
        if (++n > CHAIN_LIMIT) {
            fail(cpu, MT_TRAP_SJLJ, fc);
        }
        fc = ld32(cpu, fc + MT_SJLJ_FC_PREV);
    }
    if (fc == 0u) {
        fail(cpu, MT_TRAP_UNCAUGHT, e->type);
    }
    e->handler_fc = fc;
    phase2(cpu, s, e, s->top);
}

void mt_sjlj_throw(mt_cpu *cpu, uint32_t ue, uint32_t type, uint32_t object) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    mt_sjlj_exception *e = NULL;
    uint32_t i;
    for (i = 0; i < MT_SJLJ_MAX_EXCEPTIONS && e == NULL; i++) {
        if (s->exceptions[i].ue == ue) {
            e = &s->exceptions[i]; /* thrown again: a new exception in the same storage */
        }
    }
    for (i = 0; i < MT_SJLJ_MAX_EXCEPTIONS && e == NULL; i++) {
        if (s->exceptions[i].ue == 0u) {
            e = &s->exceptions[i];
        }
    }
    if (e == NULL || ue == 0u) {
        fail(cpu, MT_TRAP_SJLJ, ue);
    }
    memset(e, 0, sizeof *e);
    e->ue = ue;
    e->type = type;
    e->object = object;
    e->adjusted = object;
    raise_exception(cpu, s, e);
}

void mt_sjlj_resume(mt_cpu *cpu, uint32_t ue) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    phase2(cpu, s, find(cpu, s, ue), s->top);
}

uint32_t mt_sjlj_begin_catch(mt_cpu *cpu, uint32_t ue) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    mt_sjlj_exception *e = find(cpu, s, ue);
    if (e->handlers++ == 0) {
        if (s->caught_count == MT_SJLJ_MAX_EXCEPTIONS) {
            fail(cpu, MT_TRAP_SJLJ, ue);
        }
        s->caught[s->caught_count++] = ue;
    }
    e->rethrown = 0u;
    return e->adjusted;
}

void mt_sjlj_end_catch(mt_cpu *cpu) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    mt_sjlj_exception *e;
    if (s->caught_count == 0u) {
        fail(cpu, MT_TRAP_SJLJ, 0u);
    }
    e = find(cpu, s, s->caught[s->caught_count - 1u]);
    if (--e->handlers == 0) {
        s->caught_count--;
        if (!e->rethrown) {
            memset(e, 0, sizeof *e); /* handled: the exception is destroyed */
        }
    }
}

void mt_sjlj_rethrow(mt_cpu *cpu) {
    mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    mt_sjlj_exception *e;
    if (s->caught_count == 0u) {
        fail(cpu, MT_TRAP_TERMINATE, 0u); /* throw; with no exception being handled */
    }
    e = find(cpu, s, s->caught[s->caught_count - 1u]);
    e->rethrown = 1u;
    raise_exception(cpu, s, e);
}

/* -- import handlers --------------------------------------------------------------------- */

void mt_sjlj_hle_register(mt_cpu *cpu, uint32_t id) {
    (void)id;
    mt_sjlj_register(cpu, cpu->r[0]);
}

void mt_sjlj_hle_unregister(mt_cpu *cpu, uint32_t id) {
    (void)id;
    mt_sjlj_unregister(cpu, cpu->r[0]);
}

void mt_sjlj_hle_resume(mt_cpu *cpu, uint32_t id) {
    (void)id;
    mt_sjlj_resume(cpu, cpu->r[0]);
}

void mt_sjlj_hle_objc_begin_catch(mt_cpu *cpu, uint32_t id) {
    (void)id;
    cpu->r[0] = mt_sjlj_begin_catch(cpu, cpu->r[0]);
}

void mt_sjlj_hle_objc_end_catch(mt_cpu *cpu, uint32_t id) {
    (void)id;
    mt_sjlj_end_catch(cpu);
}

void mt_sjlj_hle_objc_exception_rethrow(mt_cpu *cpu, uint32_t id) {
    (void)id;
    mt_sjlj_rethrow(cpu);
}

void mt_sjlj_hle_terminate(mt_cpu *cpu, uint32_t id) {
    (void)id;
    fail(cpu, MT_TRAP_TERMINATE, 0u);
}

void mt_sjlj_bind_personalities(mt_sjlj_state *s, const mt_import_desc *descs, uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (descs[i].name == NULL) {
            continue;
        }
        if (strcmp(descs[i].name, "___gxx_personality_sj0") == 0) {
            s->gxx_personality = descs[i].addr;
        } else if (strcmp(descs[i].name, "___objc_personality_v0") == 0) {
            s->objc_personality = descs[i].addr;
        }
    }
}

/* The one guest thread's state (see mt_sjlj_state_of in maplethief/sjlj.h). */
static mt_sjlj_state mt_sjlj_main_state;

mt_sjlj_state *mt_sjlj_state_of(mt_cpu *cpu) {
    (void)cpu;
    return &mt_sjlj_main_state;
}
