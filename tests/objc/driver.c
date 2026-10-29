/* Runs the Objective-C runtime (runtime/src/objc/objc.cpp, ADR 0026) on a guest memory image
 * and answers method lookups; driven by tests/objc_harness.py.
 *
 *     driver IMAGE COMMANDS
 *
 * IMAGE is flat guest memory from address 0 (the Mach-O's segments at their VM addresses, the
 * imported symbols placed after them and every bind applied, like mt_imports_place and
 * mt_imports_apply_binds). COMMANDS has one command per line, in this order:
 *
 *     heap ADDR                            first address of the allocator's free memory
 *     host NAME SUPER SIZE CLS META        a host class (SUPER "-" for a root; CLS or META 0:
 *                                          the runtime allocates the object)
 *     imethod SEL / cmethod SEL            an instance / class method of the last host class
 *     image CLASSLIST N CATLIST N SELREFS N
 *     override NAME +|- SEL                after the image: a host override
 *     lookup CLS SEL                       a lookup in class or metaclass object CLS
 *     lookupn NAME +|- SEL                 a lookup of a class (+) or instance (-) method of
 *                                          the class named NAME
 *     bench CLS SEL N                      N cached lookups, timed
 *
 * Output: "load problems=… classes=… …" after the image, then one line per lookup,
 * "lookup CLS SEL ORIGIN IMP OWNER" (ORIGIN image|category|host|override, IMP the guest IMP,
 * OWNER the defining class object) or "lookup CLS SEL miss" ("lookupn NAME +|- SEL …" for
 * lookupn), and "bench NS_PER_LOOKUP FOUND".
 * Problems the runtime logs go to stdout as "log …" lines. Host methods and overrides are
 * dummy host functions: a lookup only reports where a method comes from. */
#include "maplethief/objc/objc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Test doubles of the runtime's traps and dispatch: nothing is sent here. */
MT_NORETURN void mt_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) {
    (void)cpu;
    printf("trap %u 0x%08X 0x%08X\n", kind, pc, detail);
    exit(3);
}
mt_guest_fn mt_dispatch(mt_cpu *cpu, uint32_t target, uint32_t site) {
    (void)cpu;
    printf("dispatch 0x%08X 0x%08X\n", target, site);
    exit(3);
}
MT_NORETURN void mt_mem_fault(mt_cpu *cpu, uint32_t addr, uint32_t size, uint32_t pc) {
    (void)cpu;
    printf("fault 0x%08X %u 0x%08X\n", addr, size, pc);
    exit(3);
}

static void host_method(mt_cpu *MT_RESTRICT cpu) { (void)cpu; }
static void override_method(mt_cpu *MT_RESTRICT cpu) { (void)cpu; }

static uint32_t g_heap, g_heap_end;
static uint32_t bump(void *user, uint32_t size, uint32_t align) {
    uint32_t at = (g_heap + align - 1u) & ~(align - 1u);
    (void)user;
    if (at + size > g_heap_end) {
        return 0;
    }
    g_heap = at + size;
    return at;
}

static void log_line(void *user, const char *line) {
    (void)user;
    printf("log %s\n", line);
}

enum { MAX_HOSTS = 512, MAX_METHODS = 64, MAX_NAME = 256 };

typedef struct host {
    char name[MAX_NAME], super[MAX_NAME];
    unsigned size, cls, meta;
    char inst[MAX_METHODS][MAX_NAME], cls_m[MAX_METHODS][MAX_NAME];
    unsigned n_inst, n_cls;
} host;

static host g_hosts[MAX_HOSTS];
static unsigned g_host_count;

static int add_hosts(mt_objc *rt) {
    static mt_objc_host_method inst[MAX_METHODS], cls_m[MAX_METHODS];
    unsigned i, j;
    for (i = 0; i < g_host_count; i++) {
        host *h = &g_hosts[i];
        mt_objc_host_class def;
        for (j = 0; j < h->n_inst; j++) {
            inst[j].selector = h->inst[j];
            inst[j].fn = host_method;
        }
        for (j = 0; j < h->n_cls; j++) {
            cls_m[j].selector = h->cls_m[j];
            cls_m[j].fn = host_method;
        }
        def.name = h->name;
        def.superclass = strcmp(h->super, "-") == 0 ? NULL : h->super;
        def.instance_size = h->size;
        def.instance_methods = inst;
        def.instance_method_count = h->n_inst;
        def.class_methods = cls_m;
        def.class_method_count = h->n_cls;
        if (mt_objc_add_host_class(rt, &def, h->cls, h->meta) != 0) {
            printf("host %s failed\n", h->name);
            return -1;
        }
    }
    return 0;
}

static const char *origin_name(uint32_t origin) {
    switch (origin) {
    case MT_OBJC_IMAGE:
        return "image";
    case MT_OBJC_CATEGORY:
        return "category";
    case MT_OBJC_HOST:
        return "host";
    case MT_OBJC_OVERRIDE:
        return "override";
    default:
        return "?";
    }
}

int main(int argc, char **argv) {
    static mt_cpu cpu;
    static char line[1024];
    FILE *f;
    long size;
    uint8_t *mem;
    mt_objc *rt;
    mt_import_config cfg;
    if (argc != 3) {
        fprintf(stderr, "usage: driver IMAGE COMMANDS\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    mem = (uint8_t *)malloc((size_t)size);
    if (mem == NULL || fread(mem, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    cpu.mem = mem;
    cpu.mem_size = (uint64_t)size;
    g_heap_end = (uint32_t)size;
    cfg = mt_import_config_default();
    cfg.log = log_line;
    rt = mt_objc_create(&cpu, bump, NULL, &cfg);
    if (rt == NULL) {
        return 2;
    }
    mt_objc_set_current(rt);

    f = fopen(argv[2], "r");
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char a[MAX_NAME], b[MAX_NAME], c[MAX_NAME];
        unsigned u1, u2, u3, u4, u5, u6;
        host *h = g_host_count > 0 ? &g_hosts[g_host_count - 1] : NULL;
        if (sscanf(line, "heap %x", &u1) == 1) {
            g_heap = u1;
        } else if (sscanf(line, "host %255s %255s %u %x %x", a, b, &u1, &u2, &u3) == 5) {
            if (g_host_count == MAX_HOSTS) {
                return 2;
            }
            h = &g_hosts[g_host_count++];
            strcpy(h->name, a);
            strcpy(h->super, b);
            h->size = u1;
            h->cls = u2;
            h->meta = u3;
        } else if (sscanf(line, "imethod %255s", a) == 1 && h != NULL && h->n_inst < MAX_METHODS) {
            strcpy(h->inst[h->n_inst++], a);
        } else if (sscanf(line, "cmethod %255s", a) == 1 && h != NULL && h->n_cls < MAX_METHODS) {
            strcpy(h->cls_m[h->n_cls++], a);
        } else if (sscanf(line, "image %x %u %x %u %x %u", &u1, &u2, &u3, &u4, &u5, &u6) == 6) {
            mt_objc_image img;
            mt_objc_stats s;
            uint32_t problems;
            clock_t t0;
            double us;
            if (add_hosts(rt) != 0) {
                return 1;
            }
            img.classlist = u1;
            img.classlist_count = u2;
            img.catlist = u3;
            img.catlist_count = u4;
            img.selrefs = u5;
            img.selrefs_count = u6;
            t0 = clock();
            problems = mt_objc_load_image(rt, &img);
            us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC;
            s = mt_objc_get_stats(rt);
            printf("load problems=%u classes=%u image_classes=%u categories=%u methods=%u "
                   "selectors=%u selrefs_changed=%u instance_start_problems=%u load_us=%.0f\n",
                   problems, s.classes, s.image_classes, s.categories, s.methods, s.selectors,
                   s.selrefs_changed, s.instance_start_problems, us);
        } else if (sscanf(line, "override %255s %1s %255s", a, b, c) == 3) {
            if (mt_objc_override(rt, a, b[0] == '+', c, override_method) != 0) {
                printf("override %s failed\n", a);
                return 1;
            }
        } else if (sscanf(line, "lookup %x %255s", &u1, a) == 2) {
            mt_objc_method m;
            if (mt_objc_lookup(rt, u1, mt_objc_sel(rt, a), &m)) {
                printf("lookup %X %s %s %X %X\n", u1, a, origin_name(m.origin), m.guest_imp,
                       m.owner);
            } else {
                printf("lookup %X %s miss\n", u1, a);
            }
        } else if (sscanf(line, "lookupn %255s %1s %255s", a, b, c) == 3) {
            mt_objc_method m;
            uint32_t cls = mt_objc_class_named(rt, a);
            if (b[0] == '+') {
                cls = mt_objc_metaclass(rt, cls);
            }
            if (cls != 0 && mt_objc_lookup(rt, cls, mt_objc_sel(rt, c), &m)) {
                printf("lookupn %s %s %s %s %X %X\n", a, b, c, origin_name(m.origin), m.guest_imp,
                       m.owner);
            } else {
                printf("lookupn %s %s %s miss\n", a, b, c);
            }
        } else if (sscanf(line, "bench %x %255s %u", &u1, a, &u2) == 3) {
            mt_objc_method m;
            const uint32_t sel = mt_objc_sel(rt, a);
            uint32_t i, found = 0;
            clock_t t0 = clock();
            for (i = 0; i < u2; i++) {
                found += (uint32_t)mt_objc_lookup(rt, u1, sel, &m);
            }
            printf("bench %.1f %u\n", (double)(clock() - t0) * 1e9 / CLOCKS_PER_SEC / u2, found);
        } else if (line[0] != '\n' && line[0] != '#') {
            printf("bad command: %s", line);
            return 2;
        }
    }
    fclose(f);
    mt_objc_destroy(rt);
    free(mem);
    return 0;
}
