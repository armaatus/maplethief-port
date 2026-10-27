// Process start-up (maplethief/boot.h, ADR 0027) on a synthetic Mach-O built here: the load
// steps and their milestones, the kernel's stack layout, __program_vars, the static
// constructors with dyld's arguments, crt1's start and main, __cxa_atexit, UIApplicationMain
// and the three ways a run ends (exit, stop, trap). The "guest functions" are host functions
// behind a test double of mt_dispatch; the runtime's own mt_trap is linked. The real image is
// booted by tests/cpp/boot_real_test.cpp (local only).
#include "maplethief/boot.h"

#include <doctest/doctest.h>

#include "maplethief/gcd.h"
#include "maplethief/objc/objc.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using u32 = std::uint32_t;

// --- the synthetic image -------------------------------------------------------------------

// __TEXT 0x1000-0x2000 (file 0x0), __text at 0x1800; __DATA 0x2000-0x3000 (file 0x1000).
constexpr u32 k_entry = 0x1800; // crt1's start stub (ARM)
constexpr u32 k_main = 0x1841;  // the stub's blx target
constexpr u32 k_ctor1 = 0x1901; // __mod_init_func[0]
constexpr u32 k_ctor2 = 0x1911; // __mod_init_func[1]
constexpr u32 k_dtor1 = 0x1921; // registered by ctor1
constexpr u32 k_dtor2 = 0x1931; // registered by ctor2
constexpr u32 k_thumb_entry = 0x1941;
constexpr u32 k_block_invoke = 0x1951; // a GCD block's invoke (run on a worker thread)
constexpr u32 k_program_vars = 0x2000;
constexpr u32 k_mod_init = 0x2014;
constexpr u32 k_bound_slot = 0x201C; // bound to the data import
constexpr u32 k_nxargc = 0x2100, k_nxargv = 0x2104, k_environ = 0x2108, k_progname = 0x210C;

std::vector<std::uint8_t> g_file;

void put(std::vector<std::uint8_t> &out, std::size_t at, u32 v) {
    if (out.size() < at + 4) {
        out.resize(at + 4);
    }
    std::memcpy(out.data() + at, &v, 4);
}

void push(std::vector<std::uint8_t> &out, u32 v) { put(out, out.size(), v); }

void push_name(std::vector<std::uint8_t> &out, const char *name) {
    char buf[16] = {};
    std::memcpy(buf, name, std::strlen(name));
    out.insert(out.end(), buf, buf + 16);
}

struct Sect {
    const char *name;
    u32 addr, size, flags;
};

std::vector<std::uint8_t> segment(const char *name, u32 vmaddr, u32 vmsize, u32 fileoff,
                                  u32 filesize, u32 prot, const std::vector<Sect> &sects) {
    std::vector<std::uint8_t> c;
    push(c, 0x1);
    push(c, 56 + 68 * static_cast<u32>(sects.size()));
    push_name(c, name);
    for (u32 v :
         {vmaddr, vmsize, fileoff, filesize, prot, prot, static_cast<u32>(sects.size()), 0u}) {
        push(c, v);
    }
    for (const Sect &s : sects) {
        push_name(c, s.name);
        push_name(c, name);
        for (u32 v : {s.addr, s.size, s.addr - vmaddr + fileoff, 2u, 0u, 0u, s.flags, 0u, 0u}) {
            push(c, v);
        }
    }
    return c;
}

struct ImageOptions {
    u32 entry = k_entry;
    u32 cpsr = 0;
    u32 rebase_size = 0;
    bool program_vars = true;
    bool crt1 = true;
};

std::vector<std::uint8_t> build_image(const ImageOptions &o = {}) {
    std::vector<std::vector<std::uint8_t>> cmds;
    cmds.push_back(segment("__PAGEZERO", 0, 0x1000, 0, 0, 0, {}));
    cmds.push_back(
        segment("__TEXT", 0x1000, 0x1000, 0, 0x1000, 5, {{"__text", 0x1800, 0x200, 0x80000400u}}));
    std::vector<Sect> data = {{"__mod_init_func", k_mod_init, 8, 0x9},
                              {"__nl_symbol_ptr", k_bound_slot, 4, 0x6},
                              {"__common", 0x2100, 0x10, 0x1}};
    if (o.program_vars) {
        data.insert(data.begin(), {"__program_vars", k_program_vars, 0x14, 0});
    }
    cmds.push_back(segment("__DATA", 0x2000, 0x1000, 0x1000, 0x1000, 3, data));
    std::vector<std::uint8_t> thread;
    push(thread, 0x5);
    push(thread, 16 + 17 * 4);
    push(thread, 1);
    push(thread, 17);
    for (int i = 0; i < 15; i++) {
        push(thread, 0);
    }
    push(thread, o.entry);
    push(thread, o.cpsr);
    cmds.push_back(thread);
    if (o.rebase_size != 0) {
        std::vector<std::uint8_t> info;
        for (u32 v : {0x80000022u, 48u, 0x1F00u, o.rebase_size, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}) {
            push(info, v);
        }
        cmds.push_back(info);
    }
    std::vector<std::uint8_t> file;
    u32 sizeofcmds = 0;
    for (const auto &c : cmds) {
        sizeofcmds += static_cast<u32>(c.size());
    }
    for (u32 v : {0xFEEDFACEu, 12u, 9u, 2u, static_cast<u32>(cmds.size()), sizeofcmds, 0x85u}) {
        push(file, v);
    }
    for (const auto &c : cmds) {
        file.insert(file.end(), c.begin(), c.end());
    }
    file.resize(0x2000);
    if (o.crt1) {
        const u32 start[10] = {0xE59D0000u, 0xE28D1004u, 0xE2804001u, 0xE0812104u,
                               0xE3CDD007u, 0xE1A03002u, 0xE4934004u, 0xE3540000u,
                               0x1AFFFFFCu, 0xFA000005u}; // blx 0x1840 (+ Thumb)
        for (u32 i = 0; i < 10; i++) {
            put(file, k_entry - 0x1000 + 4 * i, start[i]);
        }
    }
    if (o.program_vars) {
        const u32 vars[5] = {0x1000, k_nxargc, k_nxargv, k_environ, k_progname};
        for (u32 i = 0; i < 5; i++) {
            put(file, 0x1000 + (k_program_vars - 0x2000) + 4 * i, vars[i]);
        }
    }
    put(file, 0x1000 + (k_mod_init - 0x2000), k_ctor1);
    put(file, 0x1000 + (k_mod_init - 0x2000) + 4, k_ctor2);
    return file;
}

// --- the synthetic program: imports ----------------------------------------------------------

enum : u32 { ID_ATEXIT, ID_UIAPP, ID_EXIT, ID_TRUE, ID_MISSING, ID_COUNT };
const char *const k_lib = "/usr/lib/libSystem.B.dylib";
const mt_import_desc k_descs[ID_COUNT] = {
    {"___cxa_atexit", k_lib, MT_IMPORT_FUNCTION, 0x1F01, 0, 0},
    {"_UIApplicationMain", "/System/Library/Frameworks/UIKit.framework/UIKit", MT_IMPORT_FUNCTION,
     0x1F05, 0, 0},
    {"_exit", k_lib, MT_IMPORT_FUNCTION, 0x1F09, 0, 0},
    {"_kCFBooleanTrue", k_lib, MT_IMPORT_DATA, 0, 4, 4},
    {"_frobnicate", k_lib, MT_IMPORT_FUNCTION, 0x1F0D, 0, 0},
};
const mt_import_bind k_binds[] = {{k_bound_slot, ID_TRUE, 0}};
mt_import_fn g_table[ID_COUNT];

mt_program program() { return mt_program{k_descs, ID_COUNT, g_table, k_binds, 1}; }

// --- the synthetic program: guest functions ---------------------------------------------------

struct Seen {
    u32 ctor_args[2][5];
    u32 ctors;
    u32 main_args[4];
    u32 main_sp, main_lr;
    u32 entry_sp;
    u32 dtors[4];
    u32 ndtors;
    bool main_calls_missing;
    u32 main_result;
};
Seen g;

u32 rd(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, cpu->mem + addr, 4);
    return v;
}

void call_import(mt_cpu *cpu, u32 id, u32 lr) {
    cpu->r[MT_LR] = lr;
    g_table[id](cpu, id);
}

template <u32 Self, u32 Dtor> void ctor(mt_cpu *MT_RESTRICT cpu) {
    u32 *a = g.ctor_args[g.ctors++ % 2];
    for (int i = 0; i < 4; i++) {
        a[i] = cpu->r[i];
    }
    a[4] = rd(cpu, cpu->r[MT_SP]);
    cpu->r[0] = Dtor;
    cpu->r[1] = Dtor + 0x1000u; // the object
    cpu->r[2] = 0x1000;         // the image's mach header, as GCC passes __dso_handle
    call_import(cpu, ID_ATEXIT, Self + 4);
}

// Records the handler its object belongs to (ctor registered Dtor with object Dtor + 0x1000).
template <u32 Self> void dtor(mt_cpu *MT_RESTRICT cpu) {
    CHECK(cpu->r[0] == Self + 0x1000u);
    g.dtors[g.ndtors++ % 4] = Self;
}

void guest_main(mt_cpu *MT_RESTRICT cpu) {
    for (int i = 0; i < 4; i++) {
        g.main_args[i] = cpu->r[i];
    }
    g.main_sp = cpu->r[MT_SP];
    g.main_lr = cpu->r[MT_LR];
    // UIApplicationMain(argc, argv, nil, nil)
    cpu->r[2] = 0;
    cpu->r[3] = 0;
    call_import(cpu, ID_UIAPP, k_main + 0x10);
    g.main_result = cpu->r[0];
    if (g.main_calls_missing) {
        call_import(cpu, ID_MISSING, k_main + 0x20);
    }
    cpu->r[0] = g.main_result + 1u;
}

void thumb_entry(mt_cpu *MT_RESTRICT cpu) {
    g.entry_sp = cpu->r[MT_SP];
    cpu->r[0] = 42;
}

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

struct Booted {
    mt_process *p;
    u32 outcome;
};

Booted boot(mt_boot_config cfg, const ImageOptions &o = {}) {
    g = Seen{};
    g_log.clear();
    g_file = build_image(o);
    cfg.imports.log = capture;
    const mt_program prog = program();
    mt_process *p = mt_process_load(&cfg, &prog, g_file.data(), g_file.size());
    REQUIRE(p != nullptr);
    return {p, mt_process_run(p)};
}

std::vector<std::string> milestone_names(const mt_process *p) {
    u32 n = 0;
    const mt_milestone *m = mt_process_milestones(p, &n);
    std::vector<std::string> out;
    for (u32 i = 0; i < n; i++) {
        out.emplace_back(m[i].name);
    }
    return out;
}

std::string guest_string(mt_cpu *cpu, u32 addr) {
    return std::string(reinterpret_cast<const char *>(cpu->mem + addr));
}

bool logged(const std::string &needle) {
    for (const auto &l : g_log) {
        if (l.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::int32_t g_hook_result = 0;
bool g_hook_exits = false;
mt_app_main_args g_hook_args{};

std::int32_t app_main(mt_process *, mt_cpu *cpu, const mt_app_main_args *args, void *user) {
    g_hook_args = *args;
    CHECK(user == &g_hook_result);
    if (g_hook_exits) {
        mt_process_exit(cpu, 3);
    }
    return g_hook_result;
}

// A block on a GCD worker thread ends the run: exit(5) (mode 1) or a guest trap (mode 2).
int g_block_mode = 0;
int g_block_on_worker = -1;

void block_invoke(mt_cpu *MT_RESTRICT cpu) {
    g_block_on_worker = mt_gcd_on_worker();
    if (g_block_mode == 1) {
        mt_process_exit(cpu, 5);
    }
    mt_trap(cpu, MT_TRAP_UDF, k_block_invoke - 1u, 7u);
}

// UIApplicationMain's hook: dispatch_async(global queue, block), then a run-loop turn.
std::int32_t gcd_app_main(mt_process *p, mt_cpu *cpu, const mt_app_main_args *, void *) {
    const u32 block = mt_heap_calloc(mt_process_mem(p), 1u, 32u);
    const u32 words[5] = {mt_objc_class_named(mt_process_objc(p), "__NSGlobalBlock__"), 1u << 28,
                          0u, k_block_invoke, block + 20u};
    std::memcpy(cpu->mem + block, words, sizeof words);
    cpu->r[0] = 0;
    cpu->r[1] = 0;
    mt_hle_dispatch_get_global_queue(cpu, 0);
    cpu->r[1] = block;
    mt_hle_dispatch_async(cpu, 0);
    mt_gcd_drain_main(cpu); // the worker ends the run: no return
    return 99;
}

} // namespace

extern "C" mt_guest_fn mt_dispatch(mt_cpu *cpu, u32 target, u32 site) noexcept {
    switch (target) {
    case k_main:
        return guest_main;
    case k_ctor1:
        return ctor<k_ctor1, k_dtor1>;
    case k_ctor2:
        return ctor<k_ctor2, k_dtor2>;
    case k_dtor1:
        return dtor<k_dtor1>;
    case k_dtor2:
        return dtor<k_dtor2>;
    case k_thumb_entry:
        return thumb_entry;
    case k_block_invoke:
        return block_invoke;
    default:
        mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target);
    }
}

TEST_CASE("the start-up runs every step in order and stops at UIApplicationMain") {
    Booted b = boot(mt_boot_config_default());
    mt_process *p = b.p;
    CHECK(mt_process_current() == p);
    CHECK(b.outcome == MT_PROCESS_STOPPED);
    CHECK(std::string(mt_process_result_of(p)->stop_reason).find("UIApplicationMain") == 0);
    CHECK(milestone_names(p) == std::vector<std::string>{"image", "link", "objc", "place", "binds",
                                                         "objc-image", "objc-overrides", "seal",
                                                         "stack", "program-vars", "constructor",
                                                         "atexit", "constructor", "atexit", "main",
                                                         "UIApplicationMain", "stop"});
    u32 n = 0;
    const mt_milestone *m = mt_process_milestones(p, &n);
    REQUIRE(n == 17);
    CHECK(m[4].value == 1); // binds
    CHECK(m[10].value == k_ctor1);
    CHECK(m[11].value == k_dtor1);
    CHECK(m[12].value == k_ctor2);
    CHECK(m[14].value == k_main);
    CHECK(m[15].value == k_main + 0x10); // UIApplicationMain's caller, inside main
    CHECK(logged("boot: binds: 1 binds applied"));
    CHECK(logged("boot: constructor: 0x00001901 (1 of 2)"));
    CHECK(logged("boot: image: 2 segments mapped, 5 sections, entry 0x00001800 (crt1 start, "
                 "main 0x00001841)"));
    // the handlers are registered, and a stop does not run them
    const mt_atexit_entry *e = mt_process_atexit_entries(p, &n);
    REQUIRE(n == 2);
    CHECK((e[0].func == k_dtor1 && e[0].arg == k_dtor1 + 0x1000u && e[0].dso == 0x1000));
    CHECK(e[1].func == k_dtor2);
    CHECK(g.ndtors == 0);
    mt_process_destroy(p);
    CHECK(mt_process_current() == nullptr);
}

TEST_CASE("the kernel's stack, __program_vars and the arguments of constructors and main") {
    Booted b = boot(mt_boot_config_default());
    mt_process *p = b.p;
    mt_cpu *cpu = mt_process_cpu(p);
    const mt_process_args *a = mt_process_args_of(p);
    // sp -> argc, argv[], 0, envp[], 0, apple[], 0
    CHECK(a->sp % 16 == 0);
    CHECK(a->sp >= a->stack.base);
    CHECK(a->sp < a->stack.top);
    CHECK(rd(cpu, a->sp) == 1);
    CHECK(a->argc == 1);
    CHECK(a->argv == a->sp + 4);
    CHECK(guest_string(cpu, rd(cpu, a->argv)) == MT_BOOT_DEFAULT_EXECUTABLE);
    CHECK(rd(cpu, a->argv + 4) == 0);
    CHECK(a->envp == a->argv + 8);
    std::vector<std::string> env;
    for (u32 at = a->envp; rd(cpu, at) != 0; at += 4) {
        env.push_back(guest_string(cpu, rd(cpu, at)));
    }
    const std::string home = "/var/mobile/Applications/00000000-0000-0000-0000-000000000000";
    CHECK(env == std::vector<std::string>{"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
                                          "TMPDIR=" + home + "/tmp/", "SHELL=/bin/sh",
                                          "HOME=" + home, "USER=mobile", "LOGNAME=mobile",
                                          "CFFIXED_USER_HOME=" + home});
    CHECK(a->apple == a->envp + 4 * 8);
    CHECK(guest_string(cpu, rd(cpu, a->apple)) == MT_BOOT_DEFAULT_EXECUTABLE);
    CHECK(rd(cpu, a->apple + 4) == 0);
    // dyld's ProgramVars
    CHECK(a->program_vars == k_program_vars);
    CHECK(rd(cpu, k_nxargc) == 1);
    CHECK(rd(cpu, k_nxargv) == a->argv);
    CHECK(rd(cpu, k_environ) == a->envp);
    CHECK(guest_string(cpu, rd(cpu, k_progname)) == "MapleThief");
    // the bind, and the iOS FPSCR
    CHECK(rd(cpu, k_bound_slot) == mt_process_import_addr(p, ID_TRUE));
    CHECK(mt_process_import_addr(p, ID_TRUE) >= MT_GUEST_HEAP_BASE);
    CHECK(cpu->fpscr == MT_FPSCR_IOS);
    // constructors: (argc, argv, envp, apple, &vars), in order
    CHECK(g.ctors == 2);
    for (const auto &c : g.ctor_args) {
        CHECK(c[0] == 1);
        CHECK(c[1] == a->argv);
        CHECK(c[2] == a->envp);
        CHECK(c[3] == a->apple);
        CHECK(c[4] == k_program_vars);
    }
    // main as crt1's start calls it: r0-r3, sp aligned, lr = start's `b exit`
    CHECK(g.main_args[0] == 1);
    CHECK(g.main_args[1] == a->argv);
    CHECK(g.main_args[2] == a->envp);
    CHECK(g.main_args[3] == a->apple);
    CHECK(g.main_sp == a->sp);
    CHECK(g.main_lr == k_entry + 40);
    mt_process_destroy(p);
}

TEST_CASE("an app hook result returns from UIApplicationMain, and exit runs the handlers") {
    mt_boot_config cfg = mt_boot_config_default();
    cfg.app_main = app_main;
    cfg.app_user = &g_hook_result;
    g_hook_result = 6;
    g_hook_exits = false;
    Booted b = boot(cfg);
    CHECK(b.outcome == MT_PROCESS_EXITED);
    CHECK(mt_process_result_of(b.p)->exit_status == 7); // main returns the hook's result + 1
    CHECK(g.main_result == 6);
    CHECK(g_hook_args.argc == 1);
    CHECK(g_hook_args.argv == mt_process_args_of(b.p)->argv);
    CHECK(g_hook_args.caller == k_main + 0x10);
    // __cxa_atexit handlers: last registered first, each with its object
    CHECK(g.ndtors == 2);
    CHECK(g.dtors[0] == k_dtor2);
    CHECK(g.dtors[1] == k_dtor1);
    CHECK(milestone_names(b.p).back() == "exit");
    CHECK(logged("boot: main-returned: main returned 7"));
    CHECK(logged("boot: exit: exit(7)"));
    // a process runs once
    CHECK(mt_process_run(b.p) == MT_PROCESS_EXITED);
    CHECK(g.ndtors == 2);
    mt_process_destroy(b.p);
}

TEST_CASE("exit from inside the app lifecycle ends the run") {
    mt_boot_config cfg = mt_boot_config_default();
    cfg.app_main = app_main;
    cfg.app_user = &g_hook_result;
    g_hook_exits = true;
    Booted b = boot(cfg);
    g_hook_exits = false;
    CHECK(b.outcome == MT_PROCESS_EXITED);
    CHECK(mt_process_result_of(b.p)->exit_status == 3);
    CHECK(g.ndtors == 2);
    mt_process_destroy(b.p);
}

TEST_CASE("a guest trap ends the run with the trap") {
    mt_boot_config cfg = mt_boot_config_default();
    cfg.app_main = app_main;
    cfg.app_user = &g_hook_result;
    g_hook_result = 0;
    g = Seen{};
    g.main_calls_missing = true; // _frobnicate is unimplemented; the policy is stop
    g_file = build_image();
    g_log.clear();
    cfg.imports.log = capture;
    const mt_program prog = program();
    mt_process *p = mt_process_load(&cfg, &prog, g_file.data(), g_file.size());
    REQUIRE(p != nullptr);
    g.main_calls_missing = true;
    CHECK(mt_process_run(p) == MT_PROCESS_TRAPPED);
    const mt_process_result *r = mt_process_result_of(p);
    CHECK(r->trap_kind == MT_TRAP_IMPORT);
    CHECK(r->trap_pc == k_main + 0x20);
    CHECK(r->trap_detail == ID_MISSING);
    CHECK(logged("unimplemented import _frobnicate (id 4, libSystem.B.dylib) called from "
                 "0x00001861: stopping"));
    CHECK(logged("boot: trap: unimplemented import (kind 7) at pc 0x00001861"));
    // The crash report and the unimplemented report of the run (#53, ADR 0030), in the
    // process's log.
    CHECK(logged("=== guest crash: unimplemented import ==="));
    CHECK(logged("pc 0x00001861; detail 0x00000004 (import _frobnicate)"));
    CHECK(logged("unimplemented report: 1 import (1 call), 0 selectors (0 messages)"));
    CHECK(logged("import _frobnicate (libSystem.B.dylib): 1 call, first from 0x00001861"));
    CHECK(g.ndtors == 0);
    mt_process_destroy(p);
}

TEST_CASE("a GCD worker thread that exits or traps ends the run on the main thread") {
    mt_boot_config cfg = mt_boot_config_default();
    cfg.app_main = gcd_app_main;
    SUBCASE("exit") {
        g_block_mode = 1;
        Booted b = boot(cfg);
        CHECK(g_block_on_worker == 1);
        CHECK(b.outcome == MT_PROCESS_EXITED);
        CHECK(mt_process_result_of(b.p)->exit_status == 5);
        CHECK(g.ndtors == 2); // the handlers ran (on the worker)
        CHECK(logged("boot: exit: exit(5)"));
        mt_process_destroy(b.p); // ends the parked worker thread
    }
    SUBCASE("trap") {
        g_block_mode = 2;
        Booted b = boot(cfg);
        CHECK(g_block_on_worker == 1);
        CHECK(b.outcome == MT_PROCESS_TRAPPED);
        const mt_process_result *r = mt_process_result_of(b.p);
        CHECK(r->trap_kind == MT_TRAP_UDF);
        CHECK(r->trap_pc == k_block_invoke - 1u);
        CHECK(r->trap_detail == 7u);
        CHECK(g.ndtors == 0);
        mt_process_destroy(b.p);
    }
    mt_gcd_stats s;
    mt_gcd_get_stats(&s);
    CHECK(s.workers == 0u); // mt_process_destroy reset GCD
    g_block_mode = 0;
}

TEST_CASE("extra implementations take precedence over the registry") {
    mt_boot_config cfg = mt_boot_config_default();
    static const mt_hle_entry extra[] = {
        {"_UIApplicationMain", [](mt_cpu *cpu, u32) { cpu->r[0] = 40; }, nullptr},
    };
    cfg.hle = extra;
    cfg.hle_count = 1;
    Booted b = boot(cfg);
    CHECK(b.outcome == MT_PROCESS_EXITED);
    CHECK(mt_process_result_of(b.p)->exit_status == 41);
    CHECK(logged("boot: link: 5 imports, 1 function imports left to the default handler"));
    mt_process_destroy(b.p);
}

TEST_CASE("arguments, environment and executable path from the configuration") {
    mt_boot_config cfg = mt_boot_config_default();
    static const char *const args[] = {"-a", "bee", nullptr};
    static const char *const env[] = {"X=1", nullptr};
    cfg.executable_path = "/Apps/Game.app/Game";
    cfg.args = args;
    cfg.env = env;
    Booted b = boot(cfg);
    mt_cpu *cpu = mt_process_cpu(b.p);
    const mt_process_args *a = mt_process_args_of(b.p);
    CHECK(a->argc == 3);
    CHECK(guest_string(cpu, rd(cpu, a->argv)) == "/Apps/Game.app/Game");
    CHECK(guest_string(cpu, rd(cpu, a->argv + 8)) == "bee");
    CHECK(rd(cpu, a->argv + 12) == 0);
    CHECK(guest_string(cpu, rd(cpu, a->envp)) == "X=1");
    CHECK(rd(cpu, a->envp + 4) == 0);
    CHECK(guest_string(cpu, rd(cpu, k_progname)) == "Game");
    CHECK(rd(cpu, k_nxargc) == 3);
    mt_process_destroy(b.p);
}

TEST_CASE("an entry that is not crt1's start runs as the kernel would start it") {
    ImageOptions o;
    o.crt1 = false;
    o.entry = k_thumb_entry & ~1u;
    o.cpsr = MT_CPSR_T;
    o.program_vars = false;
    Booted b = boot(mt_boot_config_default(), o);
    CHECK(mt_process_image(b.p)->main == 0);
    CHECK(b.outcome == MT_PROCESS_EXITED);
    CHECK(mt_process_result_of(b.p)->exit_status == 42);
    CHECK(g.entry_sp == mt_process_args_of(b.p)->sp);
    CHECK(logged("boot: program-vars: the image has no __program_vars"));
    CHECK(logged("boot: entry: 0x00001941"));
    // the constructors still got their arguments, with no program vars
    CHECK(g.ctor_args[0][4] == 0);
    mt_process_destroy(b.p);
}

TEST_CASE("images the boot cannot load") {
    const mt_program prog = program();
    mt_boot_config cfg = mt_boot_config_default();
    cfg.imports.log = capture;
    g_log.clear();
    ImageOptions rebased;
    rebased.rebase_size = 8;
    std::vector<std::uint8_t> file = build_image(rebased);
    CHECK(mt_process_load(&cfg, &prog, file.data(), file.size()) == nullptr);
    CHECK(logged("boot: image failed: the image has rebase opcodes"));
    CHECK(mt_process_current() == nullptr);

    file = build_image();
    file[0] = 0;
    CHECK(mt_process_load(&cfg, &prog, file.data(), file.size()) == nullptr);
    file = build_image();
    put(file, 20, 0x5000); // sizeofcmds beyond the file
    CHECK(mt_process_load(&cfg, &prog, file.data(), 0x1000) == nullptr);
    file = build_image();
    put(file, 28 + 4, 7); // a command size that is not a multiple of 4
    CHECK(mt_process_load(&cfg, &prog, file.data(), file.size()) == nullptr);
    CHECK(mt_process_load(&cfg, &prog, file.data(), 16) == nullptr);
    CHECK(mt_process_current() == nullptr);

    // one process at a time
    file = build_image();
    mt_process *p = mt_process_load(&cfg, &prog, file.data(), file.size());
    REQUIRE(p != nullptr);
    CHECK(mt_process_load(&cfg, &prog, file.data(), file.size()) == nullptr);
    mt_process_destroy(p);
}

TEST_CASE("crt1's start is recognised and its blx decoded") {
    std::vector<std::uint8_t> file = build_image();
    mt_image_info info{};
    REQUIRE(mt_image_info_read(file.data(), file.size(), &info) == 0);
    CHECK(info.entry == k_entry);
    CHECK(info.main == k_main);
    CHECK(info.mach_header == 0x1000);
    CHECK(info.rebase_size == 0);
    const mt_section *s = mt_image_section(&info, "__DATA", "__mod_init_func");
    REQUIRE(s != nullptr);
    CHECK((s->addr == k_mod_init && s->size == 8 && (s->flags & 0xFF) == 9));
    CHECK(mt_image_section(&info, "__DATA", "__nope") == nullptr);
    // a backward blx with H set: target = 0x1824 + 8 - 0x20 + 2 = 0x180E, Thumb
    put(file, k_entry - 0x1000 + 36, 0xFB000000u | (0x00FFFFFFu & static_cast<u32>(-8)));
    REQUIRE(mt_image_info_read(file.data(), file.size(), &info) == 0);
    CHECK(info.main == 0x180F);
    // one wrong word and it is not crt1's stub
    put(file, k_entry - 0x1000 + 8, 0xE2804002u);
    REQUIRE(mt_image_info_read(file.data(), file.size(), &info) == 0);
    CHECK(info.main == 0);
}

TEST_CASE("the start-up imports without a process behave as unimplemented") {
    // (policy continue: the default handler returns 0 instead of trapping)
    mt_import_config ic = mt_import_config_default();
    ic.unimplemented = MT_UNIMPLEMENTED_CONTINUE;
    ic.log = capture;
    g_log.clear();
    mt_imports_link(k_descs, ID_COUNT, g_table, nullptr, 0, &ic);
    mt_cpu cpu{};
    cpu.r[0] = 5;
    mt_hle_entry const *reg = nullptr;
    u32 n = 0;
    reg = mt_hle_registry(&n);
    mt_import_fn atexit_fn = nullptr;
    for (u32 i = 0; i < n; i++) {
        if (std::strcmp(reg[i].name, "___cxa_atexit") == 0) {
            atexit_fn = reg[i].fn;
        }
    }
    REQUIRE(atexit_fn != nullptr);
    atexit_fn(&cpu, ID_ATEXIT);
    CHECK(cpu.r[0] == 0);
    CHECK(mt_import_unimplemented_calls(ID_ATEXIT) == 1);
    CHECK(logged("unimplemented import ___cxa_atexit"));
}
