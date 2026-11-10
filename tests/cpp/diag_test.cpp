// Diagnostics (ADR 0030, #53): the symbol table, the HLE call trace and its filters, the
// unimplemented report, the function-entry coverage and the guest crash reporter. The crash
// tests that need the process to die run tests/cpp/crash_child.cpp as a child process; the
// Nintendo Switch has no child processes, and MapleThiefTests.nro --crash-test does the same
// there by hand (tests/cpp/switch_test_host.cpp, #236).
#include "maplethief/diag.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "diag/diag_internal.h"
#include "diag_fixture.h"
#include "guest_trap.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"

namespace fx = mt_diag_fixture;

extern "C" {
// tests/cpp/trace_entry_fixture.c, compiled with MT_TRACE_ENTRY 1.
void f_000225E5(mt_cpu *cpu);
void f_00002001(mt_cpu *cpu);
}

namespace {

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

bool has(const std::string &text, const char *what) { return text.find(what) != std::string::npos; }

std::string joined() {
    std::string all;
    for (const auto &l : g_log) {
        all += l;
        all += '\n';
    }
    return all;
}

std::string report() {
    std::string s(mt_report_format(nullptr, 0), '\0');
    mt_report_format(s.data(), s.size() + 1u);
    return s;
}

// Fresh diagnostics with the fixture's symbols and the log captured.
struct Diag {
    explicit Diag(const char *trace_hle = nullptr, const char *trace = nullptr) {
        mt_diag_reset();
        g_log.clear();
        REQUIRE(mt_symbols_load_text(fx::k_symbols, std::strlen(fx::k_symbols), nullptr) == 4);
        mt_diag_config cfg = mt_diag_config_default();
        cfg.trace_hle = trace_hle;
        cfg.trace = trace;
        cfg.log = capture;
        REQUIRE(mt_diag_configure(&cfg) == 0);
    }
    ~Diag() { mt_diag_reset(); }
    Diag(const Diag &) = delete;
    Diag &operator=(const Diag &) = delete;
};

// A guest with 64 KiB of plain memory (no mt_mem) and sp at 0x8000.
struct Guest {
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x10000);
    mt_cpu cpu{};
    Guest() {
        cpu.mem = mem.data();
        cpu.mem_size = mem.size();
        cpu.r[MT_SP] = 0x8000u;
    }
};

#if defined(MT_CRASH_CHILD)
// Runs crash_child with `mode`; returns its stderr (and stdout).
std::string run_child(const char *mode, int *status) {
#if defined(_WIN32)
    const std::string cmd = std::string("\"\"") + MT_CRASH_CHILD + "\" " + mode + " 2>&1\"";
    std::FILE *p = _popen(cmd.c_str(), "r");
#else
    const std::string cmd = std::string("'") + MT_CRASH_CHILD + "' " + mode + " 2>&1";
    std::FILE *p = popen(cmd.c_str(), "r");
#endif
    REQUIRE(p != nullptr);
    std::string out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        out.append(buf, n);
    }
#if defined(_WIN32)
    *status = _pclose(p);
#else
    *status = pclose(p);
#endif
    return out;
}
#endif

} // namespace

TEST_CASE("the symbol table: load, look up, symbolize, reject malformed lines") {
    mt_diag_reset();
    std::uint32_t bad = 99;
    const char text[] = "# maplethief symbols 1\r\n"
                        "\n"
                        "00003001 00000080 CFoo::bar  \r\n"
                        "000027E0 0000002C start\n"
                        "00002001 00000100 -[View drawRect:]\n"
                        "zz 10 nope\n"
                        "00005001 00000000 empty\n"
                        "00005001 00000010\n"
                        "123456789 00000010 too_long\n"
                        "FFFFFFF1 00000020 wraps\n";
    CHECK(mt_symbols_load_text(text, sizeof text - 1u, &bad) == 3);
    CHECK(bad == 5u);
    CHECK(mt_symbols_count() == 3u);
    std::uint32_t off = 0, ptr = 0;
    CHECK(std::string(mt_symbols_lookup(0x3001u, &off, &ptr)) == "CFoo::bar");
    CHECK(off == 0u);
    CHECK(ptr == 0x3001u);
    CHECK(std::string(mt_symbols_lookup(0x307Fu, &off, nullptr)) == "CFoo::bar");
    CHECK(off == 0x7Eu);
    CHECK(mt_symbols_lookup(0x3080u, nullptr, nullptr) == nullptr);
    CHECK(std::string(mt_symbols_lookup(0x27E8u, &off, &ptr)) == "start");
    CHECK(ptr == 0x27E0u); // ARM: no Thumb bit
    CHECK(mt_symbols_lookup(0x1000u, nullptr, nullptr) == nullptr);
    char buf[64];
    CHECK(mt_symbolize(0x2043u, buf, sizeof buf) == std::strlen("-[View drawRect:]+0x42"));
    CHECK(std::string(buf) == "-[View drawRect:]+0x42");
    CHECK(mt_symbolize(0x2200u, buf, sizeof buf) == 0u); // between two symbols
    CHECK(std::string(buf).empty());
    CHECK(mt_symbolize(0x2043u, buf, 6) == 5u); // truncated
    CHECK(std::string(buf) == "-[Vie");
    CHECK(mt_symbols_load("/nonexistent/maplethief.sym") == -1);
    CHECK(mt_symbols_count() == 3u); // kept
    mt_symbols_clear();
    CHECK(mt_symbols_lookup(0x3001u, nullptr, nullptr) == nullptr);
}

TEST_CASE("HLE trace filters: globs, exclusions, the last matching pattern wins") {
    CHECK(mt_trace_filter_match("*", "_memcpy") == 1);
    CHECK(mt_trace_filter_match("", "_memcpy") == 0);
    CHECK(mt_trace_filter_match(nullptr, "_memcpy") == 0);
    CHECK(mt_trace_filter_match("_mem*", "_memcpy") == 1);
    CHECK(mt_trace_filter_match("mem*", "_memcpy") == 1); // without the underscore
    CHECK(mt_trace_filter_match("memcpy", "_memcpy") == 1);
    CHECK(mt_trace_filter_match("memcpy", "_memcpy2") == 0);
    CHECK(mt_trace_filter_match("_str?en", "_strlen") == 1);
    CHECK(mt_trace_filter_match("*, !_objc_msgSend*", "_objc_msgSend") == 0);
    CHECK(mt_trace_filter_match("*, !_objc_msgSend*", "_objc_msgSend_stret") == 0);
    CHECK(mt_trace_filter_match("*, !_objc_msgSend*", "_free") == 1);
    CHECK(mt_trace_filter_match("!_free, *", "_free") == 1); // the last match wins
    CHECK(mt_trace_filter_match(" _CF* , _UI* ", "_UIApplicationMain") == 1);
    CHECK(mt_trace_filter_match("_CF*,_UI*", "_glClear") == 0);
}

TEST_CASE("trace channels") {
    {
        Diag d(nullptr, "boot, objc");
        CHECK(mt_trace_enabled("boot") == 1);
        CHECK(mt_trace_enabled("objc") == 1);
        CHECK(mt_trace_enabled("bo") == 0);
        CHECK(mt_trace_enabled("hle") == 0);
        mt_tracef("boot", "binds applied: %d", 1272);
        mt_tracef("gl", "not logged");
        REQUIRE(g_log.size() == 1u);
        CHECK(g_log[0] == "boot: binds applied: 1272");
    }
    {
        Diag d(nullptr, "all");
        CHECK(mt_trace_enabled("anything") == 1);
    }
    {
        Diag d;
        CHECK(mt_trace_enabled("boot") == 0);
    }
}

TEST_CASE("trace and report: a test that calls an unimplemented import") {
    Diag d("*");
    Guest g;
    mt_import_config icfg = mt_import_config_default();
    icfg.unimplemented = MT_UNIMPLEMENTED_CONTINUE;
    icfg.log = capture;
    mt_import_fn table[fx::k_import_count] = {};
    CHECK(mt_imports_link(fx::k_descs, fx::k_import_count, table, fx::k_hle, 1u, &icfg) == 1u);
    // Both function imports are traced, the data import is not.
    CHECK(table[0] == mt_trace_import_call);
    CHECK(table[1] == mt_trace_import_call);
    CHECK(table[2] == mt_import_unimplemented);
    REQUIRE(!g_log.empty());
    CHECK(g_log.back() == "hle: tracing 2 of 3 imports (*)");
    g_log.clear();

    // An implemented import: call and return lines, with the symbolized guest caller.
    g.cpu.r[0] = 0x11u;
    g.cpu.r[1] = 0x22u;
    g.cpu.r[2] = 0x33u;
    g.cpu.r[3] = 0x44u;
    g.cpu.r[MT_LR] = fx::k_lr_bar;
    table[0](&g.cpu, 0u);
    CHECK(g.cpu.r[0] == 42u);
    REQUIRE(g_log.size() == 2u);
    CHECK(g_log[0] ==
          "hle: _answer(0x00000011, 0x00000022, 0x00000033, 0x00000044) from 0x00003021 "
          "CFoo::bar+0x20");
    CHECK(g_log[1] == "hle: _answer -> 0x0000002A (r1 0x00000022)");
    g_log.clear();

    // The unimplemented import, three times from two callers.
    for (std::uint32_t lr : {0x3021u, 0x2043u, 0x2043u}) {
        g.cpu.r[0] = 7u;
        g.cpu.r[MT_LR] = lr;
        table[1](&g.cpu, 1u);
        CHECK(g.cpu.r[0] == 0u);
    }
    const std::string log = joined();
    CHECK(has(log, "hle: _missing(0x00000007, 0x00000022, 0x00000033, 0x00000044) from "
                   "0x00003021 CFoo::bar+0x20\n"));
    CHECK(has(log, "unimplemented import _missing (id 1, libFoo.dylib) called from 0x00003021: "
                   "returning 0\n"));
    CHECK(has(log, "hle: _missing -> 0x00000000 (r1 0x00000000)\n"));
    CHECK(has(log, "hle: _missing(0x00000007, 0x00000000, 0x00000033, 0x00000044) from "
                   "0x00002043 CGame::Update+0x42\n"));

    const mt_report_stats s = mt_report_get_stats();
    CHECK(s.imports == 1u);
    CHECK(s.import_calls == 3u);
    CHECK(s.selectors == 0u);
    mt_report_unimplemented_selector("UIView", 0, "setFoo:", 0x2003u);
    mt_report_unimplemented_selector("UIView", 1, "layerClass", 0x2003u);
    mt_report_unimplemented_selector("UIView", 0, "setFoo:", 0x3003u);
    mt_report_unimplemented_selector(nullptr, 0, "size", 0x5000u);
    CHECK(report() ==
          "unimplemented report: 1 import (3 calls), 3 selectors (4 messages)\n"
          "import _missing (libFoo.dylib): 3 calls, first from 0x00003021 CFoo::bar+0x20\n"
          "selector -[UIView setFoo:]: 2 messages, first from 0x00002003 CGame::Update+0x2\n"
          "selector +[UIView layerClass]: 1 message, first from 0x00002003 CGame::Update+0x2\n"
          "selector -[(no known class) size]: 1 message, first from 0x00005000\n");
}

TEST_CASE("the report: many unique names, an empty report, written once at finish") {
    {
        Diag d;
        CHECK(report() == "unimplemented report: none\n");
        char name[32];
        for (int round = 0; round < 2; round++) {
            for (int i = 0; i < 500; i++) {
                std::snprintf(name, sizeof name, "_f%d", i);
                mt_report_unimplemented_import(name, "/usr/lib/libX.dylib",
                                               0x1000u + static_cast<std::uint32_t>(i));
            }
        }
        const mt_report_stats s = mt_report_get_stats();
        CHECK(s.imports == 500u);
        CHECK(s.import_calls == 1000u);
        const std::string r = report();
        CHECK(has(r, "import _f0 (libX.dylib): 2 calls, first from 0x00001000\n"));
        CHECK(has(r, "import _f499 (libX.dylib): 2 calls, first from 0x000011F3\n"));
        // Without a report path the report goes to the log at finish, once.
        mt_diag_finish();
        mt_diag_finish();
        CHECK(g_log.size() == 501u);
        CHECK(g_log[0] ==
              "unimplemented report: 500 imports (1000 calls), 0 selectors (0 messages)");
    }
    {
        Diag d; // nothing recorded, no path: nothing written
        mt_diag_finish();
        CHECK(g_log.empty());
    }
    {
        mt_diag_reset();
        g_log.clear();
        mt_diag_config cfg = mt_diag_config_default();
        cfg.report_path = "-"; // always, even when empty
        cfg.log = capture;
        REQUIRE(mt_diag_configure(&cfg) == 0);
        mt_diag_finish();
        REQUIRE(g_log.size() == 1u);
        CHECK(g_log[0] == "unimplemented report: none");
        mt_diag_reset();
    }
}

TEST_CASE("the HLE trace is off by default and wraps only matching imports") {
    {
        Diag d; // off: the table holds the implementations themselves
        mt_import_fn table[fx::k_import_count] = {};
        mt_imports_link(fx::k_descs, fx::k_import_count, table, fx::k_hle, 1u, nullptr);
        CHECK(table[0] == fx::k_hle[0].fn);
        CHECK(table[1] == mt_import_unimplemented);
        CHECK(g_log.empty());
    }
    {
        Diag d("*,!_missing");
        mt_import_fn table[fx::k_import_count] = {};
        mt_imports_link(fx::k_descs, fx::k_import_count, table, fx::k_hle, 1u, nullptr);
        CHECK(table[0] == mt_trace_import_call);
        CHECK(table[1] == mt_import_unimplemented);
    }
}

TEST_CASE("the HLE trace names the selector of objc_msgSend calls") {
    Diag d("_objc_msgSend*");
    Guest g;
    const mt_import_desc descs[] = {
        {"_objc_msgSend", "/usr/lib/libobjc.A.dylib", MT_IMPORT_FUNCTION, 0xF0D00u, 0u, 0u},
        {"_objc_msgSend_stret", "/usr/lib/libobjc.A.dylib", MT_IMPORT_FUNCTION, 0xF0D04u, 0u, 0u},
    };
    const mt_hle_entry hle[] = {{"_objc_msgSend", fx::hle_answer, nullptr},
                                {"_objc_msgSend_stret", fx::hle_answer, nullptr}};
    mt_import_fn table[2] = {};
    mt_imports_link(descs, 2u, table, hle, 2u, nullptr);
    std::memcpy(&g.mem[0x2000], "initWithFrame:", 15);
    g.cpu.r[0] = 0x1000u;
    g.cpu.r[1] = 0x2000u;
    g.cpu.r[MT_LR] = 0x3003u;
    g_log.clear();
    table[0](&g.cpu, 0u);
    REQUIRE(!g_log.empty());
    CHECK(has(g_log[0], ") [initWithFrame:] from 0x00003003 CFoo::bar+0x2"));
    g.cpu.r[1] = 0x500u; // the hidden struct pointer
    g.cpu.r[2] = 0x2000u;
    g_log.clear();
    table[1](&g.cpu, 1u);
    CHECK(has(g_log[0], "[initWithFrame:]"));
    g.cpu.r[2] = 0xFFFFFFF0u; // not a guest string
    g_log.clear();
    table[1](&g.cpu, 1u);
    CHECK(!has(g_log[0], "["));
}

TEST_CASE("function-entry coverage (MT_TRACE_ENTRY 1)") {
    Diag d(nullptr, "entry");
    Guest g;
    const std::uint32_t before = mt_coverage_count();
    g.cpu.r[MT_LR] = 0x4011u;
    f_000225E5(&g.cpu);
    f_000225E5(&g.cpu);
    f_00002001(&g.cpu);
    f_000225E5(&g.cpu);
    CHECK(g.cpu.r[0] == 4u);
    CHECK(mt_coverage_count() == before + 2u);
    REQUIRE(g_log.size() == 2u);
    CHECK(g_log[0] == "entry: f_000225E5 (first entry, from 0x00004011)");
    CHECK(g_log[1] == "entry: f_00002001 CGame::Update (first entry, from 0x00004011)");
    std::string cov(mt_coverage_format(nullptr, 0), '\0');
    mt_coverage_format(cov.data(), cov.size() + 1u);
    CHECK(cov == "# maplethief coverage 1: 2 guest functions entered (pointer, entries, name)\n"
                 "0x00002001 1 CGame::Update\n"
                 "0x000225E5 3 f_000225E5\n");
}

TEST_CASE("the trap report: kind, pc, registers and a symbolized r7 backtrace") {
    Diag d;
    mt_mem_config mcfg = mt_mem_config_default();
    mt_mem *mem = mt_mem_create(&mcfg);
    REQUIRE(mem != nullptr);
    mt_stack stack{};
    REQUIRE(mt_stack_create(mem, MT_STACK_GRANULE, &stack) == 0);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    cpu.r[MT_SP] = stack.top;
    fx::build_frames(&cpu);
    const std::uint32_t block = mt_heap_malloc(mem, 32u);
    cpu.r[4] = block + 8u;

    std::uint32_t frames[8];
    REQUIRE(mt_crash_backtrace(&cpu, frames, 8u) == 2u);
    CHECK(frames[0] == fx::k_ret_update);
    CHECK(frames[1] == fx::k_ret_loop);
    CHECK(mt_crash_backtrace(&cpu, frames, 1u) == 1u);

    mt_crash_info info{};
    info.kind = MT_TRAP_NO_FUNCTION;
    info.pc = 0x3011u;
    info.detail = 0x3005u;
    std::string r(mt_crash_format(&cpu, &info, nullptr, 0), '\0');
    mt_crash_format(&cpu, &info, r.data(), r.size() + 1u);
    INFO(r);
    CHECK(has(r, "=== guest crash: no guest function at an indirect target ===\n"));
    CHECK(has(r, "pc 0x00003011 CFoo::bar+0x10; detail 0x00003005 CFoo::bar+0x4\n"));
    CHECK(has(r, "  r4  0x"));
    CHECK(has(r, "[heap block"));
    CHECK(has(r, "  lr  0x00003021 CFoo::bar+0x20\n"));
    CHECK(has(r, "[guest stack"));
    CHECK(has(r, "backtrace (r7 frame chain):\n"
                 "  #0 0x00003011 CFoo::bar+0x10\n"
                 "  #1 0x00002043 CGame::Update+0x42\n"
                 "  #2 0x00004011 main_loop+0x10\n"
                 "=== end of guest crash ===\n"));

    // The import trap names the import.
    mt_import_fn table[fx::k_import_count] = {};
    mt_imports_link(fx::k_descs, fx::k_import_count, table, fx::k_hle, 1u, nullptr);
    info.kind = MT_TRAP_IMPORT;
    info.detail = 1u;
    r.assign(mt_crash_format(&cpu, &info, nullptr, 0), '\0');
    mt_crash_format(&cpu, &info, r.data(), r.size() + 1u);
    CHECK(has(r, "=== guest crash: unimplemented import ===\n"));
    CHECK(has(r, "detail 0x00000001 (import _missing)\n"));

    // A broken chain: r7 outside any stack, or a frame that does not move up.
    cpu.r[7] = block;
    CHECK(mt_crash_backtrace(&cpu, frames, 8u) == 0u);
    cpu.r[7] = stack.top - 0x80u;
    fx::put32(&cpu, cpu.r[7], cpu.r[7] - 0x10u); // next below this one
    CHECK(mt_crash_backtrace(&cpu, frames, 8u) == 1u);
    cpu.r[7] = stack.guard + 0x10u; // the guard is never read
    CHECK(mt_crash_backtrace(&cpu, frames, 8u) == 0u);
    cpu.r[7] = 0x1002u; // misaligned
    CHECK(mt_crash_backtrace(&cpu, frames, 8u) == 0u);
    mt_mem_destroy(mem);
}

TEST_CASE("a checked memory fault: the trap report has the access size and the region") {
    Diag d;
    mt_mem_config mcfg = mt_mem_config_default();
    mt_mem *mem = mt_mem_create(&mcfg);
    REQUIRE(mem != nullptr);
    mt_stack stack{};
    REQUIRE(mt_stack_create(mem, MT_STACK_GRANULE, &stack) == 0);
    static mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    cpu.r[MT_SP] = stack.top;
    fx::build_frames(&cpu);
    CHECK(mt_test::stops([](void *) { (void)mt_g2h(&cpu, 4u, 2u); }, nullptr));
    CHECK(mt_test::g_trap.kind == MT_TRAP_MEMORY);
    mt_crash_info info{};
    info.kind = MT_TRAP_MEMORY;
    info.pc = mt_test::g_trap.pc;
    info.detail = mt_test::g_trap.detail;
    mt_mem_fault_info f{};
    REQUIRE(mt_mem_last_fault(&f) == 0);
    info.has_fault = 1;
    info.fault_addr = f.addr;
    info.fault_size = f.size;
    std::string r(mt_crash_format(&cpu, &info, nullptr, 0), '\0');
    mt_crash_format(&cpu, &info, r.data(), r.size() + 1u);
    INFO(r);
    CHECK(has(r, "=== guest crash: memory fault ===\n"));
    CHECK(has(r, "pc 0x00003021 CFoo::bar+0x20; detail 0x00000004\n"));
    CHECK(has(r, "fault: 2-byte access at guest 0x00000004: page zero (a null pointer)\n"));
    CHECK(has(r, "  #1 0x00002043 CGame::Update+0x42\n"));
    mt_mem_destroy(mem);
}

TEST_CASE("the access of a fault comes from the AArch64 ESR (macOS arm64, the Switch)") {
    CHECK(mt_crash_access_from_esr(0x92000046u) == MT_ACCESS_WRITE);   // EC 0x24, WnR set
    CHECK(mt_crash_access_from_esr(0x96000006u) == MT_ACCESS_READ);    // EC 0x25, WnR clear
    CHECK(mt_crash_access_from_esr(0x82000006u) == MT_ACCESS_EXECUTE); // EC 0x20
    CHECK(mt_crash_access_from_esr(0x86000006u) == MT_ACCESS_EXECUTE); // EC 0x21
    CHECK(mt_crash_access_from_esr(0x02000000u) == MT_ACCESS_UNKNOWN); // EC 0: undefined
    CHECK(mt_crash_access_from_esr(0x56000000u) == MT_ACCESS_UNKNOWN); // EC 0x15: svc
}

namespace {
// Two host functions with different bodies, so that no linker folds them into one.
void table_fn_a(mt_cpu *cpu) { cpu->r[0] = 1u; }
void table_fn_b(mt_cpu *cpu) { cpu->r[0] = 2u; }
} // namespace

TEST_CASE("without dladdr, a host pc names the nearest generated function of the table") {
    const mt_fn_entry table[] = {{0x3001u, table_fn_a}, {0x2001u, table_fn_b}, {0x4001u, nullptr}};
    const auto a = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(table_fn_a));
    const auto b = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(table_fn_b));
    const std::uint64_t hi = a > b ? a : b;
    const std::uint32_t hi_fn = a > b ? 0x3001u : 0x2001u;
    std::uint64_t off = 0;
    CHECK(mt_crash_nearest_function(table, 3u, hi + 0x10u, &off) == hi_fn);
    CHECK(off == 0x10u);
    CHECK(mt_crash_nearest_function(table, 3u, hi, &off) == hi_fn);
    CHECK(off == 0u);
    const std::uint64_t lo = a < b ? a : b;
    CHECK(mt_crash_nearest_function(table, 3u, lo - 1u, &off) == 0u);         // below every one
    CHECK(mt_crash_nearest_function(table, 3u, hi + (1u << 20), &off) == 0u); // too far past
    CHECK(mt_crash_nearest_function(nullptr, 0u, hi, &off) == 0u);
}

TEST_CASE("the crash file gets the same report as the log (the Switch's crash.txt)") {
    namespace fs = std::filesystem;
    Diag d;
    const fs::path dir = fs::temp_directory_path() / "maplethief-crash-file-test";
    fs::remove_all(dir);
    const fs::path file = dir / "crash.txt";
    mt_crash_set_file(file.string().c_str());
    mt_mem_config mcfg = mt_mem_config_default();
    mt_mem *mem = mt_mem_create(&mcfg);
    REQUIRE(mem != nullptr);
    mt_stack stack{};
    REQUIRE(mt_stack_create(mem, MT_STACK_GRANULE, &stack) == 0);
    static mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    cpu.r[MT_SP] = stack.top;
    fx::build_frames(&cpu);
    mt_crash_report_trap(&cpu, MT_TRAP_UNREACHABLE, 0x3021u, 0u);
    mt_crash_set_file(nullptr);
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    INFO(text);
    CHECK(has(text, "=== guest crash: unreachable code reached ===\n"));
    CHECK(has(text, "  #1 0x00002043 CGame::Update+0x42\n"));
    CHECK(has(text, "=== end of guest crash ===\n"));
    CHECK(has(joined(), "=== guest crash: unreachable code reached ==="));
    mt_mem_destroy(mem);
    in.close();
    fs::remove_all(dir);
}

#if defined(MT_CRASH_CHILD)

TEST_CASE("the unimplemented report is written at exit") {
    int status = 0;
    const std::string out = run_child("exit", &status);
    INFO(out);
    CHECK(status == 0);
    CHECK(has(out, "unimplemented import _missing (id 1, libFoo.dylib) called from 0x00003021: "
                   "returning 0\n"));
    CHECK(has(out, "unimplemented report: 1 import (1 call), 0 selectors (0 messages)\n"
                   "import _missing (libFoo.dylib): 1 call, first from 0x00003021 "
                   "CFoo::bar+0x20\n"));
}

TEST_CASE("a deliberate guest fault (hardware, page zero) prints a symbolized backtrace") {
    int status = 0;
    const std::string out = run_child("null", &status);
    INFO(out);
    CHECK(status != 0);
    CHECK(has(out, "=== guest crash: hardware fault ===\n"));
    CHECK(has(out, " at guest 0x00000008: page zero (a null pointer)\n"));
#if defined(__APPLE__)
    // dladdr names the generated function at the host PC, and the fault context has the
    // access. (Linux: the same code, but an x86-64 container on an arm64 Mac runs under
    // translation, whose fault context is not checked here; Windows: no dladdr.)
    CHECK(has(out, "in guest function 0x00003001 CFoo::bar (the guest pc is not known"));
    CHECK(has(out, "fault: write at guest 0x00000008: page zero (a null pointer)\n"));
#endif
    CHECK(has(out, "  r0  0x01000000 [heap block 0x01000000+0x0 (32 bytes)]\n"));
    CHECK(has(out, "  lr  0x00003021 CFoo::bar+0x20\n"));
    CHECK(has(out, "backtrace (r7 frame chain):\n"
                   "  #0 0x00002043 CGame::Update+0x42\n"
                   "  #1 0x00004011 main_loop+0x10\n"
                   "=== end of guest crash ===\n"));
    // The report follows the crash report.
    const std::size_t crash = out.find("=== end of guest crash ===");
    const std::size_t rep = out.find("unimplemented report: 1 import (1 call)");
    CHECK(crash != std::string::npos);
    CHECK(rep != std::string::npos);
    CHECK(rep > crash);
}

TEST_CASE("a deliberate guest stack overflow (hardware, stack guard) is named") {
    int status = 0;
    const std::string out = run_child("guard", &status);
    INFO(out);
    CHECK(status != 0);
    CHECK(has(out, "=== guest crash: hardware fault ===\n"));
    CHECK(has(out, ": guard below guest stack 0x"));
    CHECK(has(out, "  #0 0x00002043 CGame::Update+0x42\n"));
}

TEST_CASE("a checked guest fault reaches the trap report through mt_trap") {
    int status = 0;
    const std::string out = run_child("trap", &status);
    INFO(out);
    CHECK(status != 0);
    CHECK(has(out, "guest memory fault: 4-byte access to 0x00000004 at pc 0x00003021"));
    CHECK(has(out, "=== guest crash: memory fault ===\n"
                   "pc 0x00003021 CFoo::bar+0x20; detail 0x00000004\n"
                   "fault: 4-byte access at guest 0x00000004: page zero (a null pointer)\n"));
    CHECK(has(out, "  #2 0x00004011 main_loop+0x10\n"));
    CHECK(has(out, "unimplemented report: 1 import (1 call)")); // exit() runs atexit
}

#endif
