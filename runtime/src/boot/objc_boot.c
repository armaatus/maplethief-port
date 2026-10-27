/* The Objective-C steps of the start-up (ADR 0026, ADR 0027, ADR 0029). boot.c calls them
 * in this order:
 *
 *   mt_boot_objc_create     before the data imports are placed: the runtime exists, with the
 *                           guest heap's alloc and free, so the init hooks of imported data
 *                           objects can reach it (mt_process_objc(mt_process_current()))
 *   mt_boot_objc_image      after the binds: the ObjC start-up of ADR 0029 (mt_objc_start
 *                           without the overrides): record the placed class objects, add the
 *                           host class registry and placeholders, then realize the image's
 *                           classes and categories from __objc_classlist, __objc_catlist and
 *                           __objc_selrefs
 *   mt_boot_objc_overrides  the host overrides (mt_objc_install_overrides), then
 *                           mt_objc_set_current */
#include <stdio.h>

#include "boot/process.h"

int mt_boot_objc_create(mt_process *p) {
    p->objc = mt_objc_create(&p->cpu, mt_mem_guest_alloc, p->mem, &p->cfg.imports);
    if (p->objc == NULL) {
        mt_process_log(p, "boot: objc failed: out of host memory");
        return -1;
    }
    mt_objc_set_free(p->objc, mt_mem_guest_free);
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
    uint32_t problems = 0;
    char detail[240];
    pointer_section(p, "__objc_classlist", &image.classlist, &image.classlist_count);
    pointer_section(p, "__objc_catlist", &image.catlist, &image.catlist_count);
    pointer_section(p, "__objc_selrefs", &image.selrefs, &image.selrefs_count);
    if (mt_objc_bind_imports(p->objc, p->prog.import_descs, p->prog.import_count, p->import_addrs) <
        0) {
        mt_process_log(p, "boot: objc-image failed: out of host memory");
        return -1;
    }
    problems += mt_objc_add_host_classes(p->objc);
    mt_objc_add_placeholders(p->objc);
    problems += mt_objc_load_image(p->objc, &image);
    st = mt_objc_get_stats(p->objc);
    /* Problems (each logged) are not fatal: a class that failed to realize makes its messages
     * undeliverable, and those go through the unimplemented-import policy. The game's image
     * has none. */
    snprintf(detail, sizeof detail,
             "%u image classes, %u host classes (%u placeholders), %u categories, %u selectors, "
             "%u selrefs rewritten, %u problems",
             (unsigned)st.image_classes, (unsigned)(st.classes - st.image_classes),
             (unsigned)st.placeholders, (unsigned)st.categories, (unsigned)st.selectors,
             (unsigned)st.selrefs_changed, (unsigned)problems);
    mt_process_milestone("objc-image", st.image_classes, detail);
    return 0;
}

/* Host overrides of image methods (ADR 0026, ADR 0029): the only way the port changes the
 * shell's behaviour, listed with their reasons in runtime/src/objc/classes.cpp. An override
 * whose class the image lacks (a test image) is logged and skipped. */
int mt_boot_objc_overrides(mt_process *p) {
    uint32_t installed;
    char detail[160];
    mt_objc_install_overrides(p->objc);
    installed = mt_objc_get_stats(p->objc).overrides;
    mt_objc_set_current(p->objc);
    snprintf(detail, sizeof detail, "%u host overrides installed", (unsigned)installed);
    mt_process_milestone("objc-overrides", installed, detail);
    return 0;
}
