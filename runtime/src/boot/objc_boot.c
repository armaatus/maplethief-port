/* The Objective-C steps of the start-up (ADR 0026 "Start-up", ADR 0027). boot.c calls them
 * in this order:
 *
 *   mt_boot_objc_create     before the data imports are placed: the runtime exists, so the
 *                           init hooks of the imported class objects can register host classes
 *                           with it (mt_process_objc(mt_process_current()))
 *   mt_boot_objc_image      after the binds: realize the image's classes and categories from
 *                           its __objc_classlist, __objc_catlist and __objc_selrefs
 *   mt_boot_objc_overrides  the host overrides, then mt_objc_set_current
 *
 * #47 grows these: host classes, +initialize, stricter checks. */
#include <stdio.h>

#include "boot/process.h"

int mt_boot_objc_create(mt_process *p) {
    p->objc = mt_objc_create(&p->cpu, mt_mem_guest_alloc, p->mem, &p->cfg.imports);
    if (p->objc == NULL) {
        mt_process_log(p, "boot: objc failed: out of host memory");
        return -1;
    }
    mt_process_milestone("objc", 0u, "Objective-C runtime created");
    return 0;
}

static void pointer_section(const mt_process *p, const char *name, uint32_t *addr,
                            uint32_t *count) {
    const mt_section *s = mt_image_section(&p->image, "__DATA", name);
    *addr = s != NULL ? s->addr : 0u;
    *count = s != NULL ? s->size / 4u : 0u;
}

int mt_boot_objc_image(mt_process *p) {
    mt_objc_image image;
    mt_objc_stats st;
    uint32_t problems;
    char detail[200];
    pointer_section(p, "__objc_classlist", &image.classlist, &image.classlist_count);
    pointer_section(p, "__objc_catlist", &image.catlist, &image.catlist_count);
    pointer_section(p, "__objc_selrefs", &image.selrefs, &image.selrefs_count);
    problems = mt_objc_load_image(p->objc, &image);
    st = mt_objc_get_stats(p->objc);
    /* Problems are not fatal yet: until the host classes exist (#47), every image class whose
     * superclass is a framework class is one. A message to such a class is undeliverable and
     * goes through the unimplemented-import policy. */
    snprintf(detail, sizeof detail,
             "%u image classes, %u categories, %u selectors, %u selrefs rewritten, %u problems",
             (unsigned)st.image_classes, (unsigned)st.categories, (unsigned)st.selectors,
             (unsigned)st.selrefs_changed, (unsigned)problems);
    mt_process_milestone("objc-image", st.image_classes, detail);
    return 0;
}

/* Host overrides of image methods (ADR 0026): the only way the port changes the shell's
 * behaviour. Each returns nil / NO. The host classes' overrides (+[SKPaymentQueue
 * canMakePayments]) come with their classes. */
static void return_zero(mt_cpu *MT_RESTRICT cpu) {
    cpu->r[0] = 0u;
    cpu->r[1] = 0u;
}

static const struct {
    const char *class_name;
    int class_method;
    const char *selector;
    mt_guest_fn fn;
    const char *reason;
} k_overrides[] = {
    {"Chartboost", 1, "sharedChartboost", return_zero, "ads off: no Chartboost code runs"},
    {"GameCenterManager", 1, "IsGameCenterAvailable", return_zero, "no Game Center (#52)"},
};

int mt_boot_objc_overrides(mt_process *p) {
    uint32_t i, installed = 0;
    char detail[160];
    for (i = 0; i < sizeof k_overrides / sizeof k_overrides[0]; i++) {
        if (mt_objc_override(p->objc, k_overrides[i].class_name, k_overrides[i].class_method,
                             k_overrides[i].selector, k_overrides[i].fn) == 0) {
            installed++;
        } else {
            /* A test image without the class; the game's image has both. */
            snprintf(detail, sizeof detail, "boot: objc-overrides: no class %s for %c[%s %s] (%s)",
                     k_overrides[i].class_name, k_overrides[i].class_method ? '+' : '-',
                     k_overrides[i].class_name, k_overrides[i].selector, k_overrides[i].reason);
            mt_process_log(p, detail);
        }
    }
    mt_objc_set_current(p->objc);
    snprintf(detail, sizeof detail, "%u host overrides installed", (unsigned)installed);
    mt_process_milestone("objc-overrides", installed, detail);
    return 0;
}
