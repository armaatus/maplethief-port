// The real boot (#43, ADR 0027): the recompiled game (maplethief_gen) starts in the runtime
// from work/slices/MapleThief.armv7 up to UIApplicationMain. Local only: it needs work/gen
// (python -m recomp all) and the slice, and exits 77 (skipped) without them.
//
// Checks, with the unimplemented-import policy "stop", so that any import without an
// implementation before UIApplicationMain ends the run with a trap:
//   - the boot trace: binds applied -> the 2 static constructors (each registering its
//     destructor with __cxa_atexit) -> main -> UIApplicationMain, and nothing else
//   - UIApplicationMain(1, argv, nil, nil) is called from inside main
//   - no import went to the default handler
//   - __program_vars and the constructors' effects in guest memory
//
// Imports that the boot path calls but other M4 issues implement have stand-ins here (k_pending)
// until those issues land. The list is strict: once the runtime's registry implements one of
// them, the test fails until its stand-in is deleted, so it shrinks to empty on the epic branch.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(MT_HAVE_GENERATED_CODE)

#include "maplethief/boot.h"
#include "maplethief/hle/abi.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/dispatch.h"

namespace {

using u32 = std::uint32_t;

int g_failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) {
        g_failures++;
    }
}

std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", static_cast<unsigned>(v));
    return buf;
}

std::vector<std::string> g_log;
void capture(void *, const char *line) {
    g_log.emplace_back(line);
    std::printf("  | %s\n", line);
}

// --- stand-ins for imports that other issues implement ----------------------------------------

std::vector<std::string> g_standin_calls;

mt_mem *mem_of(mt_cpu *cpu) { return mt_mem_from_cpu(cpu); }

// void *operator new(size_t), operator new[](size_t): the guest heap
void standin_new(mt_cpu *cpu, u32) {
    g_standin_calls.emplace_back("operator new");
    const u32 size = cpu->r[0];
    cpu->r[0] = mt_heap_malloc(mem_of(cpu), size != 0 ? size : 1u);
}

// char *strcpy(char *dst, const char *src)
void standin_strcpy(mt_cpu *cpu, u32) {
    g_standin_calls.emplace_back("strcpy");
    const char *src = mt_g2h_str(cpu, cpu->r[1]);
    const u32 len = static_cast<u32>(std::strlen(src)) + 1u;
    std::memmove(mt_g2h(cpu, cpu->r[0], len), src, len);
}

struct Pending {
    const char *name;
    const char *owner; // the issue that implements it
    mt_import_fn fn;
};

const Pending k_pending[] = {
    {"__Znwm", "#45 (C++ ABI)", standin_new},
    {"__Znam", "#45 (C++ ABI)", standin_new},
    {"_strcpy", "#44 (libc)", standin_strcpy},
};

// --- UIApplicationMain ---------------------------------------------------------------------

mt_app_main_args g_app{};
bool g_app_called = false;

std::int32_t app_main(mt_process *, mt_cpu *cpu, const mt_app_main_args *args, void *) {
    g_app = *args;
    g_app_called = true;
    mt_process_stop(cpu, "boot_real_test: UIApplicationMain reached");
}

u32 rd(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, cpu->mem + addr, 4);
    return v;
}

std::vector<std::uint8_t> read_file(const char *path) {
    std::vector<std::uint8_t> out;
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        return out;
    }
    std::uint8_t buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return out;
}

} // namespace

int main() {
    const std::vector<std::uint8_t> slice = read_file(MT_ARMV7_SLICE);
    if (slice.empty()) {
        std::printf("skipped: %s is missing (run scripts/bootstrap)\n", MT_ARMV7_SLICE);
        return 77;
    }

    // The stand-ins, strictly: none may shadow an implementation the runtime has.
    u32 n_registry = 0;
    const mt_hle_entry *registry = mt_hle_registry(&n_registry);
    std::vector<mt_hle_entry> standins;
    for (const Pending &pending : k_pending) {
        bool implemented = false;
        for (u32 i = 0; i < n_registry; i++) {
            implemented |= std::strcmp(registry[i].name, pending.name) == 0;
        }
        check(!implemented, std::string(pending.name) + " is still pending (" + pending.owner +
                                "); once the runtime implements it, delete its stand-in");
        standins.push_back({pending.name, pending.fn, nullptr});
    }

    mt_program prog = MT_PROGRAM_GENERATED;
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    cfg.imports.unimplemented = MT_UNIMPLEMENTED_STOP;
    cfg.imports.log = capture;
    cfg.hle = standins.data();
    cfg.hle_count = static_cast<u32>(standins.size());
    cfg.app_main = app_main;
    mt_process *p = mt_process_load(&cfg, &prog, slice.data(), slice.size());
    if (p == nullptr) {
        std::printf("FAIL: the boot could not load the slice\n");
        return 1;
    }
    const u32 outcome = mt_process_run(p);
    const mt_process_result *r = mt_process_result_of(p);
    mt_cpu *cpu = mt_process_cpu(p);
    const mt_process_args *a = mt_process_args_of(p);

    std::printf("\n");
    check(outcome == MT_PROCESS_STOPPED && g_app_called,
          "the run stopped in UIApplicationMain (outcome " + std::to_string(outcome) + ")");
    if (outcome == MT_PROCESS_TRAPPED) {
        std::printf("trap kind %u at pc 0x%08X detail 0x%08X\n", r->trap_kind, r->trap_pc,
                    r->trap_detail);
    }

    // The trace.
    u32 n = 0;
    const mt_milestone *m = mt_process_milestones(p, &n);
    std::string trace;
    for (u32 i = 0; i < n; i++) {
        trace += std::string(i != 0 ? " -> " : "") + m[i].name + " " + hex(m[i].value);
    }
    std::printf("trace: %s\n", trace.c_str());
    // value 0 is any value: the place count and the stack pointer follow the runtime
    const std::vector<std::pair<std::string, u32>> expected = {
        {"image", 0x27E0},
        {"link", 0},
        {"objc", 0},
        {"place", 111},
        {"binds", 1268},
        {"objc-image", 75},
        {"objc-overrides", 2},
        {"seal", 0},
        {"stack", 0},
        {"program-vars", 0xF1370},
        {"constructor", 0xA925},
        {"atexit", 0x6639},
        {"constructor", 0x9E041},
        {"atexit", 0x9DBB1},
        {"main", 0x280D},
        {"UIApplicationMain", 0},
        {"stop", 0},
    };
    bool same = n == expected.size();
    for (u32 i = 0; same && i < n; i++) {
        same = expected[i].first == m[i].name &&
               (expected[i].second == 0 || expected[i].second == m[i].value);
    }
    check(same, "trace: binds applied -> 2 constructors -> main -> UIApplicationMain");

    // UIApplicationMain(argc, argv, nil, nil), called from main
    const mt_fn_entry *caller = mt_fn_containing(g_app.caller);
    check(caller != nullptr && caller->addr == 0x280D,
          "UIApplicationMain is called from inside main 0x280D (lr " + hex(g_app.caller) + ")");
    check(g_app.argc == 1 && g_app.argv == a->argv && g_app.principal_class_name == 0 &&
              g_app.delegate_class_name == 0,
          "UIApplicationMain(1, argv, nil, nil)");

    // No import went to the default handler.
    u32 unimplemented = 0;
    for (u32 id = 0; id < prog.import_count; id++) {
        if (mt_import_unimplemented_calls(id) != 0) {
            std::printf("unimplemented import hit: %s\n", prog.import_descs[id].name);
            unimplemented++;
        }
    }
    check(unimplemented == 0, "no unimplemented import is hit before UIApplicationMain");
    std::vector<std::pair<std::string, int>> calls; // in order of first call
    for (const auto &c : g_standin_calls) {
        auto it = calls.begin();
        while (it != calls.end() && it->first != c) {
            ++it;
        }
        if (it == calls.end()) {
            calls.emplace_back(c, 1);
        } else {
            it->second++;
        }
    }
    for (const auto &c : calls) {
        std::printf("stand-in calls: %s x%d\n", c.first.c_str(), c.second);
    }

    // __program_vars (0xF1370: mh, &NXArgc, &NXArgv, &environ, &__progname)
    check(rd(cpu, 0xF1370) == 0x1000 && rd(cpu, 0x11A400) == 1 && rd(cpu, 0x11A404) == a->argv &&
              rd(cpu, 0x11A408) == a->envp &&
              std::strcmp(reinterpret_cast<const char *>(cpu->mem + rd(cpu, 0x11A40C)),
                          "MapleThief") == 0,
          "NXArgc, NXArgv, environ and __progname are set");

    // The constructors' effects: the text cache's records on the heap, two __cxa_atexit
    // registrations for the image.
    check(rd(cpu, 0x11A428) >= MT_GUEST_HEAP_BASE && rd(cpu, 0x11A428) < MT_GUEST_HEAP_END,
          "TextCache_StaticInit stored its records (" + hex(rd(cpu, 0x11A428)) + ")");
    const mt_atexit_entry *e = mt_process_atexit_entries(p, &n);
    check(n == 2 && e[0].func == 0x6639 && e[1].func == 0x9DBB1,
          "2 destructors registered with __cxa_atexit");

    // The Objective-C runtime (#47, ADR 0029): every image class realized on the host and
    // placeholder classes, the two overrides, and main's [[NSAutoreleasePool alloc] init]
    // delivered (objc_msgSend is the runtime's, with the policy stop: an undeliverable message
    // would have trapped).
    mt_objc *objc = mt_process_objc(p);
    const mt_objc_stats st = mt_objc_get_stats(objc);
    bool clean_objc_image = false;
    for (const auto &line : g_log) {
        clean_objc_image |= line.rfind("boot: objc-image: ", 0) == 0 &&
                            line.find(", 0 problems") != std::string::npos;
    }
    check(st.image_classes == 75 && st.categories == 4 && clean_objc_image,
          "75 image classes and 4 categories realized with 0 problems (" +
              std::to_string(st.placeholders) + " placeholder classes)");
    check(st.overrides == 2, "2 host overrides: +[Chartboost sharedChartboost], "
                             "+[SKPaymentQueue canMakePayments]");
    check(mt_objc_undeliverable_count(objc) == 0, "no undeliverable message");
    const u32 token = mt_objc_pool_push(objc);
    check(token == 2, "main's NSAutoreleasePool is in place (a new pool is the 2nd, token " +
                          std::to_string(token) + ")");
    mt_objc_pool_pop(objc, token);

    mt_process_destroy(p);
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

#else

int main() {
    std::printf("skipped: built without the generated code (work/gen; python -m recomp all)\n");
    return 77;
}

#endif
