// The Objective-C runtime on the real image, running recompiled methods (#47, ADR 0029): the
// armv7 slice is mapped (#42), its imports are linked to the HLE registry, placed and bound
// (#36), the ObjC start-up runs on its own sections, and then
//
//     [[MapleThiefAppDelegate alloc] init], -setWindow:, -window, -setGlView:, -glView,
//     -release -> -[MapleThiefAppDelegate dealloc] -> [super dealloc]
//     +[EAGLView_Instance Instance] (@synchronized, +allocWithZone: singleton), -setObjView:
//
// are sent through objc_msgSend from host code. The image's methods run as recompiled code
// (maplethief_gen, reached through mt_dispatch), and they call objc_setProperty,
// objc_msgSendSuper2, objc_sync_enter/exit and objc_msgSend through the import table.
//
// Needs work/slices/MapleThief.armv7 and the generated code (scripts/bootstrap, then the
// recompiler); exits 77, which CTest reports as skipped, without the slice. Prints what it ran
// as evidence.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "guest_trap.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/dispatch.h"
#include "maplethief/sjlj.h"

namespace {

using u32 = std::uint32_t;

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failures++;
    }
}

u32 rd32(const std::vector<std::uint8_t> &f, std::size_t at) {
    u32 v = 0;
    if (at + 4 <= f.size()) {
        std::memcpy(&v, f.data() + at, 4);
    }
    return v;
}

// (address, 32-bit entries) of a __DATA section of the thin 32-bit Mach-O.
void section(const std::vector<std::uint8_t> &f, const char *name, u32 *addr, u32 *count) {
    *addr = *count = 0;
    const u32 ncmds = rd32(f, 16);
    std::size_t at = 28;
    for (u32 i = 0; i < ncmds && at + 8 <= f.size(); i++) {
        const u32 cmd = rd32(f, at), size = rd32(f, at + 4);
        if (cmd == 1u && at + 56 <= f.size()) { // LC_SEGMENT
            const u32 nsects = rd32(f, at + 48);
            for (u32 s = 0; s < nsects && at + 56 + 68 * (s + 1) <= f.size(); s++) {
                const std::size_t sect = at + 56 + 68 * s;
                char sectname[17] = {}, segname[17] = {};
                std::memcpy(sectname, f.data() + sect, 16);
                std::memcpy(segname, f.data() + sect + 16, 16);
                if (std::strcmp(segname, "__DATA") == 0 && std::strcmp(sectname, name) == 0) {
                    *addr = rd32(f, sect + 32);
                    *count = rd32(f, sect + 36) / 4;
                }
            }
        }
        if (size < 8) {
            break;
        }
        at += size;
    }
}

struct Run {
    mt_cpu *cpu;
    mt_objc *rt;
    mt_mem *mem;
};

u32 send(Run *r, u32 obj, const char *sel, u32 arg = 0, bool has_arg = false) {
    return mt_objc_msg(r->cpu, obj, mt_objc_sel(r->rt, sel), has_arg ? &arg : nullptr,
                       has_arg ? 1u : 0u);
}

// The method of `cls` (class or metaclass object) for `sel` ran as recompiled code: it is an
// image IMP whose host function is the generated one.
bool recompiled(Run *r, u32 cls, const char *sel) {
    mt_objc_method m{};
    return mt_objc_lookup(r->rt, cls, mt_objc_sel(r->rt, sel), &m) == 1 &&
           m.origin == MT_OBJC_IMAGE && m.fn != nullptr && m.fn == mt_fn_lookup(m.guest_imp);
}

void run_app_delegate(void *arg) {
    auto *r = static_cast<Run *>(arg);
    char line[200];
    const u32 cls = mt_objc_class_named(r->rt, "MapleThiefAppDelegate");
    check(cls != 0, "MapleThiefAppDelegate is realized");
    const u32 obj = send(r, cls, "alloc");
    std::snprintf(line, sizeof line, "+[MapleThiefAppDelegate alloc] -> 0x%08X (%u bytes on the heap)",
                  obj, mt_heap_usable_size(r->mem, obj));
    check(obj != 0 && mt_heap_usable_size(r->mem, obj) >= mt_objc_instance_size(r->rt, cls), line);
    check(mt_objc_class_of(r->rt, obj) == cls, "its isa is the class");
    check(send(r, obj, "init") == obj, "-init (NSObject) returns self");

    // Stand-ins for the window and the GL view: plain NSObjects.
    const u32 nsobject = mt_objc_class_named(r->rt, "NSObject");
    const u32 window = send(r, send(r, nsobject, "alloc"), "init");
    const u32 view = send(r, send(r, nsobject, "alloc"), "init");
    send(r, obj, "setWindow:", window, true);
    check(send(r, obj, "window") == window, "-setWindow: then -window returns it");
    check(mt_objc_retain_count(r->rt, window) == 2u, "the window is retained (objc_setProperty)");
    send(r, obj, "setGlView:", view, true);
    check(send(r, obj, "glView") == view, "-setGlView: then -glView returns it");
    check(recompiled(r, cls, "setWindow:") && recompiled(r, cls, "window") &&
              recompiled(r, cls, "setGlView:") && recompiled(r, cls, "glView"),
          "the accessors ran as recompiled code");
    send(r, obj, "release");
    check(recompiled(r, cls, "dealloc"), "-[MapleThiefAppDelegate dealloc] ran as recompiled code");
    check(mt_heap_usable_size(r->mem, obj) == 0, "the delegate is freed ([super dealloc])");
    check(mt_objc_retain_count(r->rt, window) == 1u && mt_objc_retain_count(r->rt, view) == 1u,
          "its dealloc released the window and the GL view");
}

void run_eaglview_instance(void *arg) {
    auto *r = static_cast<Run *>(arg);
    char line[200];
    const u32 cls = mt_objc_class_named(r->rt, "EAGLView_Instance");
    const u32 meta = mt_objc_metaclass(r->rt, cls);
    const u32 shared = send(r, cls, "Instance");
    std::snprintf(line, sizeof line, "+[EAGLView_Instance Instance] -> 0x%08X", shared);
    check(shared != 0 && mt_objc_class_of(r->rt, shared) == cls, line);
    check(send(r, cls, "Instance") == shared, "a second +Instance returns the same object");
    check(send(r, cls, "alloc") == 0, "+alloc of the singleton afterwards returns nil");
    check(recompiled(r, meta, "Instance") && recompiled(r, meta, "allocWithZone:"),
          "+Instance and +allocWithZone: ran as recompiled code");
    const u32 view = send(r, send(r, mt_objc_class_named(r->rt, "NSObject"), "alloc"), "init");
    send(r, shared, "setObjView:", view, true);
    check(send(r, shared, "objView") == view, "-setObjView: then -objView returns it");
    send(r, shared, "release"); // the singleton overrides -release
    check(send(r, cls, "Instance") == shared, "-release does not free the singleton");
    // Every objc_sync_enter had its exit: exiting once more fails.
    r->cpu->r[0] = cls;
    mt_hle_objc_sync_exit(r->cpu, 0);
    check(r->cpu->r[0] == 0xFFFFFFFFu, "@synchronized(self) left no lock behind");
}

} // namespace

int main() {
    std::ifstream in(MT_ARMV7_SLICE, std::ios::binary);
    if (!in) {
        std::printf("skipped: %s is missing (run scripts/bootstrap)\n", MT_ARMV7_SLICE);
        return 77;
    }
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    mt_mem_config mcfg = mt_mem_config_default();
    mcfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    mt_mem *mem = mt_mem_create(&mcfg);
    if (mem == nullptr) {
        std::printf("FAIL cannot create the address space\n");
        return 1;
    }
    static mt_cpu cpu;
    mt_mem_attach(mem, &cpu);
    check(mt_mem_map_macho(mem, file.data(), file.size()) == 0, "the slice is mapped");

    // The import tables (ADR 0019): HLE by name, data objects on the heap, binds.
    mt_import_config cfg = mt_import_config_default(); // stop on anything unimplemented
    u32 n_hle = 0;
    const mt_hle_entry *hle = mt_hle_registry(&n_hle);
    const u32 left = mt_imports_link(mt_import_descs, mt_import_count, mt_import_table, hle, n_hle,
                                     &cfg);
    std::printf("     %u imports, %u function imports left to the default handler\n",
                mt_import_count, left);
    std::vector<u32> addrs(mt_import_count);
    check(mt_imports_place(&cpu, mt_import_descs, mt_import_count, hle, n_hle, mt_mem_guest_alloc,
                           mem, addrs.data()) == 0,
          "the data imports are placed");
    check(mt_imports_apply_binds(&cpu, mt_import_binds, mt_import_bind_count, addrs.data(),
                                 mt_import_count) == 0,
          "every bind is written");
    mt_sjlj_state *sjlj = mt_sjlj_state_of(&cpu);
    mt_sjlj_init(sjlj);
    mt_sjlj_bind_personalities(sjlj, mt_import_descs, mt_import_count);

    // The ObjC start-up (ADR 0029).
    mt_objc_image image{};
    section(file, "__objc_classlist", &image.classlist, &image.classlist_count);
    section(file, "__objc_catlist", &image.catlist, &image.catlist_count);
    section(file, "__objc_selrefs", &image.selrefs, &image.selrefs_count);
    mt_objc *rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
    mt_objc_set_free(rt, mt_mem_guest_free);
    const u32 problems = mt_objc_start(rt, mt_import_descs, mt_import_count, addrs.data(), &image);
    mt_objc_set_current(rt);
    const mt_objc_stats s = mt_objc_get_stats(rt);
    std::printf("     objc: %u classes (%u image, %u placeholders), %u categories, %u methods, "
                "%u selectors, %u overrides, %u problems\n",
                s.classes, s.image_classes, s.placeholders, s.categories, s.methods, s.selectors,
                s.overrides, problems);
    check(problems == 0 && s.image_classes == 75u && s.categories == 4u && s.overrides == 2u,
          "mt_objc_start: 75 image classes, 4 categories, 2 overrides, no problems");

    mt_stack stack{};
    check(mt_stack_create(mem, MT_STACK_MAIN_SIZE, &stack) == 0, "a guest stack");
    cpu.r[MT_SP] = stack.top;
    cpu.r[MT_LR] = MT_HLE_RETURN;
    Run run{&cpu, rt, mem};
    for (auto fn : {run_app_delegate, run_eaglview_instance}) {
        if (mt_test::stops(fn, &run)) {
            std::printf("FAIL the guest stopped: trap %u at 0x%08X (detail 0x%08X)\n",
                        mt_test::g_trap.kind, mt_test::g_trap.pc, mt_test::g_trap.detail);
            g_failures++;
        }
    }
    check(mt_objc_undeliverable_count(rt) == 0, "no undeliverable message");
    mt_objc_destroy(rt);
    mt_mem_destroy(mem);
    std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
