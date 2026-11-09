// The player's log and crash report (#198, player_log.h, crash_report.h), with synthetic data
// only: the header, the rotation, the redaction, the tail, the crash marker's life cycle, the
// safe start's settings file byte for byte, a synthetic guest that traps through run_game, and
// (not on Windows) a child process that dies of a host signal.
#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "crash_report.h"
#include "data_location.h"
#include "game.h"
#include "maplethief/boot.h"
#include "maplethief/diag.h"
#include "maplethief/hle/imports.h"
#include "maplethief/recomp/calls.h"
#include "player_log.h"

extern std::uint32_t g_platform_test_guest_addr;
extern mt_guest_fn g_platform_test_guest_fn;

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;

namespace {

using u32 = std::uint32_t;

// A fresh folder under the test output directory.
fs::path fresh_dir(const char *name) {
    fs::path dir = fs::path(MT_TEST_OUTPUT_DIR) / "crash_report" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string slurp(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_file(const fs::path &file, const std::string &text) {
    std::ofstream(file, std::ios::binary) << text;
}

std::vector<fs::path> files_with(const fs::path &dir, const std::string &ext) {
    std::vector<fs::path> out;
    for (const auto &e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ext) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string u8(const fs::path &p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// --- a synthetic guest that traps ---------------------------------------------------------------

constexpr u32 k_entry = 0x1941; // a Thumb entry point: mt_dispatch hands it to guest_trap
constexpr u32 k_lr_a = 0x1A11, k_lr_b = 0x1B21;

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

std::vector<std::uint8_t> segment(const char *name, u32 addr, u32 size, u32 off, u32 filesize,
                                  u32 prot, const char *sect, u32 sect_addr, u32 sect_size) {
    std::vector<std::uint8_t> c;
    push(c, 0x1);
    push(c, 56u + (sect != nullptr ? 68u : 0u));
    push_name(c, name);
    for (u32 v : {addr, size, off, filesize, prot, prot, sect != nullptr ? 1u : 0u, 0u}) {
        push(c, v);
    }
    if (sect != nullptr) {
        push_name(c, sect);
        push_name(c, name);
        for (u32 v :
             {sect_addr, sect_size, sect_addr - addr + off, 2u, 0u, 0u, 0x80000400u, 0u, 0u}) {
            push(c, v);
        }
    }
    return c;
}

// A thin armv7 Mach-O: page zero, one __TEXT, a thread command at the Thumb entry point.
std::vector<std::uint8_t> trapping_image() {
    std::vector<std::vector<std::uint8_t>> cmds;
    cmds.push_back(segment("__PAGEZERO", 0, 0x1000, 0, 0, 0, nullptr, 0, 0));
    cmds.push_back(segment("__TEXT", 0x1000, 0x1000, 0, 0x1000, 5, "__text", 0x1800, 0x200));
    std::vector<std::uint8_t> thread;
    push(thread, 0x5);
    push(thread, 16 + 17 * 4);
    push(thread, 1);
    push(thread, 17);
    for (int i = 0; i < 15; i++) {
        push(thread, 0);
    }
    push(thread, k_entry & ~1u);
    push(thread, MT_CPSR_T);
    cmds.push_back(thread);
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
    file.resize(0x1000);
    return file;
}

const mt_import_desc k_descs[1] = {
    {"_unused", "/usr/lib/libSystem.B.dylib", MT_IMPORT_FUNCTION, 0x1F01, 0, 0}};
mt_import_fn g_table[1];

// Two r7 frames on the guest stack, then a trap: what generated code does at a bad instruction.
void guest_trap(mt_cpu *MT_RESTRICT cpu) {
    const u32 sp = cpu->r[MT_SP] - 16u;
    const u32 outer = sp + 8u;
    const u32 words[4] = {outer, k_lr_a, 0u, k_lr_b};
    std::memcpy(cpu->mem + sp, words, sizeof words);
    cpu->r[7] = sp;
    cpu->r[MT_SP] = sp;
    mt_trap(cpu, MT_TRAP_UDF, 0x1944u, 0x1234u);
}

} // namespace

// --- the log -------------------------------------------------------------------------------------

TEST_CASE("player log: the header names the version, commit, OS, GPU, settings and IPA check") {
    fe::LogHeader h;
    h.version = "1.2.3";
    h.commit = "abc1234";
    h.os = "macOS (Darwin 25.6.0 arm64)";
    h.gpu = "gpu (mt_gfx on the GPU), renderer gpu";
    h.settings = "preset=original, scale_mode=integer";
    h.ipa_check = "ok (the executable is 1.5's)";
    const std::string text = fe::format_log_header(h);
    CHECK(text.rfind("MapleThief 1.2.3\n", 0) == 0);
    for (const char *needle : {"commit: abc1234\n", "os: macOS (Darwin 25.6.0 arm64)\n",
                               "gpu: gpu (mt_gfx on the GPU), renderer gpu\n",
                               "settings: preset=original, scale_mode=integer\n",
                               "ipa check: ok (the executable is 1.5's)\n"}) {
        INFO(needle);
        CHECK(text.find(needle) != std::string::npos);
    }
    CHECK(text.ends_with("\n\n"));
}

TEST_CASE("player log: the settings line hides paths and device names") {
    fe::Settings s;
    s.save_dir = "/Users/somebody/saves";
    s.audio_device = "Somebody's AirPods";
    s.texture_pack = "/Users/somebody/hd";
    const std::string text = fe::log_settings_text(s);
    CHECK(text.find("somebody") == std::string::npos);
    CHECK(text.find("Somebody") == std::string::npos);
    CHECK(text.find("save_dir=<set>") != std::string::npos);
    CHECK(text.find("audio_device=<set>") != std::string::npos);
    CHECK(text.find("texture_pack=<set>") != std::string::npos);
    // An unset one is not marked, and the rest are there.
    CHECK(text.find("audio_pack=,") != std::string::npos);
    CHECK(text.find("scale_mode=") != std::string::npos);
    CHECK(text.rfind("preset=none, ", 0) == 0);
}

TEST_CASE("player log: redaction replaces the home, data and game folders") {
    const fe::Redactions r = {{"/Users/me/Games/maple/data", "<data>"},
                              {"/Users/me/Games/maple", "<game folder>"},
                              {"/Users/me", "~"}};
    CHECK(fe::redact_log_text("settings: read /Users/me/Games/maple/data/settings.ini", r) ==
          "settings: read <data>/settings.ini");
    CHECK(fe::redact_log_text("game: bundle /Users/me/Games/maple/game/x", r) ==
          "game: bundle <game folder>/game/x");
    CHECK(fe::redact_log_text("x: /Users/me/.config and /Users/me", r) == "x: ~/.config and ~");
    CHECK(fe::redact_log_text("no paths here", r) == "no paths here");
}

TEST_CASE("player log: stamps are UTC calendar times that sort") {
    char s[16];
    fe::log_stamp(0, s);
    CHECK(std::string(s) == "19700101-000000");
    fe::log_stamp(1609459199, s);
    CHECK(std::string(s) == "20201231-235959");
    fe::log_stamp(1709251200, s); // the day after a leap day
    CHECK(std::string(s) == "20240301-000000");
}

TEST_CASE("player log: rotation keeps the newest five and nothing else is touched") {
    const fs::path dir = fresh_dir("rotation");
    for (int i = 0; i < 8; i++) {
        write_file(dir / ("MapleThief-2026010" + std::to_string(i + 1) + "-000000.log"), "x");
    }
    write_file(dir / "notes.txt", "keep");
    write_file(dir / "MapleThief-keep.txt", "keep");
    CHECK(fe::rotate_logs(dir, fe::kLogFilesKept) == 3);
    const auto logs = files_with(dir, ".log");
    REQUIRE(logs.size() == fe::kLogFilesKept);
    CHECK(logs.front().filename() == "MapleThief-20260104-000000.log");
    CHECK(logs.back().filename() == "MapleThief-20260108-000000.log");
    CHECK(fs::exists(dir / "notes.txt"));
    CHECK(fs::exists(dir / "MapleThief-keep.txt"));
}

TEST_CASE("player log: lines before the header follow it; the tail keeps the last 200") {
    const fs::path dir = fresh_dir("tee");
    const std::string path = fe::player_log_open(u8(dir));
    REQUIRE(!path.empty());
    REQUIRE(fs::exists(fe::player_log_path()));
    SDL_Log("before the header");
    fe::LogHeader h;
    h.version = "9.9.9";
    fe::player_log_header(h);
    SDL_Log("after the header");
    for (int i = 0; i < 250; i++) {
        SDL_Log("line %d", i);
    }
    const char *lines[fe::kLogTailLines];
    const std::size_t n = fe::player_log_tail(lines, fe::kLogTailLines);
    REQUIRE(n == fe::kLogTailLines);
    CHECK(std::string(lines[0]) == "line 50");
    CHECK(std::string(lines[n - 1]) == "line 249");
    fe::player_log_close();
    const std::string text = slurp(dir / fs::path(path).filename());
    CHECK(text.rfind("MapleThief 9.9.9\n", 0) == 0);
    CHECK(text.find("before the header") < text.find("after the header"));
    CHECK(text.find("line 249\n") != std::string::npos);
    CHECK(fe::player_log_path() == path);
}

TEST_CASE("player log: a run that ends before the header still gets one") {
    const fs::path dir = fresh_dir("early");
    const std::string path = fe::player_log_open(u8(dir));
    REQUIRE(!path.empty());
    SDL_Log("early line");
    fe::player_log_close();
    const std::string text = slurp(dir / fs::path(path).filename());
    CHECK(text.rfind("MapleThief ", 0) == 0);
    CHECK(text.find("gpu: not known") != std::string::npos);
    CHECK(text.find("early line") != std::string::npos);
}

// --- the marker and the safe start
// ----------------------------------------------------------------

TEST_CASE("crash marker: set at the start, cleared on a clean exit, a crash leaves it") {
    const fs::path dir = fresh_dir("marker");
    SUBCASE("a clean run") {
        const fe::LastRun first = fe::crash_marker_begin(u8(dir));
        CHECK_FALSE(first.crashed);
        CHECK(fs::exists(dir / fe::kCrashMarkerName));
        fe::crash_marker_end(u8(dir));
        CHECK_FALSE(fs::exists(dir / fe::kCrashMarkerName));
        CHECK_FALSE(fe::crash_marker_begin(u8(dir)).crashed);
    }
    SUBCASE("a run that was not cleared: the next start sees it and offers the safe start") {
        fe::crash_marker_begin(u8(dir)); // the marker is set, never cleared: a crash
        write_file(dir / "20260101-000000.txt", "older report");
        write_file(dir / "20260102-000000.txt", "newest report");
        const fe::LastRun second = fe::crash_marker_begin(u8(dir));
        CHECK(second.crashed);
        CHECK(second.crash_file == u8(dir / "20260102-000000.txt"));
        // The marker is set again for the new run.
        CHECK(fs::exists(dir / fe::kCrashMarkerName));
        std::vector<std::string> asked;
        fe::set_safe_start_prompt_for_test([&](const std::string &file) {
            asked.push_back(file);
            return fe::SafeStartChoice::kDefaults;
        });
        CHECK(fe::decide_safe_start(false, second, true));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0] == second.crash_file);
        fe::set_safe_start_prompt_for_test(
            [](const std::string &) { return fe::SafeStartChoice::kNormal; });
        CHECK_FALSE(fe::decide_safe_start(false, second, true));
        fe::set_safe_start_prompt_for_test(nullptr);
        // No crash: not offered, not asked; a scripted run never asks; --safe-start always is.
        CHECK_FALSE(fe::decide_safe_start(false, fe::LastRun{}, true));
        CHECK_FALSE(fe::decide_safe_start(false, second, false));
        CHECK(fe::decide_safe_start(true, fe::LastRun{}, false));
    }
    SUBCASE("no data folder: the marker does nothing") {
        CHECK_FALSE(fe::crash_marker_begin("").crashed);
        fe::crash_marker_end("");
    }
}

TEST_CASE("safe start: defaults for this run, the player's settings file stays byte for byte") {
    const fs::path dir = fresh_dir("safe_start");
    const std::string original = "# my settings, with a comment and odd spacing\n"
                                 "  preset =   enhanced\n\n"
                                 "texture_pack = /some/pack\n"
                                 "audio_pack = /some/audio\n"
                                 "gfx_backend = gpu\n"
                                 "render_scale = 3\n"
                                 "unknown_future_key = stays\n";
    write_file(dir / "settings.ini", original);

    fe::Settings in;
    in.preset = fe::Preset::kEnhanced;
    in.texture_pack = "/some/pack";
    in.audio_pack = "/some/audio";
    in.gfx_backend = fe::GfxBackend::kGpu;
    in.render_scale = 3;
    in.save_dir = "/my/saves";
    in.save_backups = 42;
    in.language = fe::Language::kKo;
    const fe::Settings safe = fe::safe_start_settings(in);
    CHECK(safe.texture_pack.empty());
    CHECK(safe.audio_pack.empty());
    CHECK(safe.text_pack.empty());
    CHECK(safe.gfx_backend == fe::GfxBackend::kSoftware);
    CHECK(safe.render_scale == fe::Settings{}.render_scale);
    CHECK(safe.filter == fe::Settings{}.filter);
    // What the game needs to find the player's saves stays.
    CHECK(safe.save_dir == "/my/saves");
    CHECK(safe.save_backups == 42);
    CHECK(safe.language == fe::Language::kKo);
    CHECK(safe.preset == fe::Preset::kEnhanced);

    // Every way of writing the file during the safe run writes nothing.
    fe::set_settings_write_blocked(true);
    CHECK(fe::save_settings(u8(dir), safe));
    CHECK(fe::save_settings(u8(dir), in));
    CHECK(slurp(dir / "settings.ini") == original);
    fe::set_settings_write_blocked(false);
    // The block was what stopped it: a normal run writes.
    CHECK(fe::save_settings(u8(dir), safe));
    CHECK(slurp(dir / "settings.ini") != original);
}

// --- a trapping synthetic guest
// ---------------------------------------------------------------------

TEST_CASE("crash report: a synthetic guest that traps writes the report, asks for the dialog and "
          "exits with the existing status") {
    const fs::path data = fresh_dir("guest_trap");
    const fs::path crashes = data / "crashes";
    fe::set_data_location_for_test(fe::data_location_at(u8(data)));
    fe::crash_report_start(u8(crashes), false); // no signal handlers in the test process
    fe::crash_report_set_header("version: synthetic\n");
    std::vector<std::string> dialogs;
    fe::set_crash_dialog_for_test([&](const std::string &path) { dialogs.push_back(path); });

    const fs::path slice = data / "slice";
    const std::vector<std::uint8_t> image = trapping_image();
    std::ofstream(slice, std::ios::binary)
        .write(reinterpret_cast<const char *>(image.data()),
               static_cast<std::streamsize>(image.size()));
    const mt_program program{k_descs, 1, g_table, nullptr, 0};
    g_platform_test_guest_addr = k_entry;
    g_platform_test_guest_fn = guest_trap;

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("crash", 480, 320, 0, &window, &renderer),
                    SDL_GetError());

    fe::GameOptions options;
    options.bundle_dir = u8(data);
    options.slice_path = u8(slice);
    options.audio = false;
    options.quit_at = 0; // the trap comes first
    fe::GameProgram game;
    game.program = &program;
    game.reservation = 0;
    const int status = fe::run_game(game, window, renderer, false, options);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    g_platform_test_guest_fn = nullptr;
    fe::set_crash_dialog_for_test(nullptr);
    fe::set_data_location_for_test(std::nullopt);
    mt_crash_set_sink(nullptr, nullptr); // the later tests' traps are not this report's

    CHECK(status == fe::kExitGuestEnded);
    const auto files = files_with(crashes, ".txt");
    REQUIRE(files.size() == 1);
    const std::string report = slurp(files[0]);
    CHECK(report.rfind("MapleThief crash report\nversion: synthetic\n", 0) == 0);
    CHECK(report.find("=== guest crash: udf, bkpt or svc ===") != std::string::npos);
    CHECK(report.find("registers (as last stored to mt_cpu):") != std::string::npos);
    CHECK(report.find("backtrace (r7 frame chain") != std::string::npos);
    CHECK(report.find("0x00001A11") != std::string::npos);
    CHECK(report.find("0x00001B21") != std::string::npos);
    CHECK(report.find("--- the last log lines ---") != std::string::npos);
    REQUIRE(dialogs.size() == 1);
    CHECK(dialogs[0] == u8(files[0]));
    CHECK(fe::crash_reported());
    CHECK(fe::last_crash_file() == u8(files[0]));
}

#if defined(MT_CRASH_REPORT_CHILD)

TEST_CASE("crash report: a host signal writes the report and starts the dialog helper, and the "
          "process still dies of it") {
    const fs::path dir = fresh_dir("host_signal");
    const fs::path dialog_log = dir / "dialog.log";
    const std::string cmd = "env MAPLETHIEF_CRASH_DIALOG_LOG='" + u8(dialog_log) + "' '" +
                            MT_CRASH_REPORT_CHILD + "' host '" + u8(dir) + "' 2>/dev/null";
    const int status = std::system(cmd.c_str());
    CHECK(status != 0); // dead of the signal (reported by the shell)
    const auto files = files_with(dir, ".txt");
    REQUIRE(files.size() == 1);
    const std::string report = slurp(files[0]);
    CHECK(report.find("=== guest crash: host crash (outside the guest) ===") != std::string::npos);
    CHECK(report.find("SIGSEGV") != std::string::npos);
    CHECK(report.find("the child's own log line") != std::string::npos);
    CHECK(report.find("version: child") != std::string::npos);
    // The marker survived: the next start offers a safe start.
    CHECK(fs::exists(dir / fe::kCrashMarkerName));
    // The helper runs on its own, a moment after the crash.
    std::string written;
    for (int i = 0; i < 100 && written.find("dialog: ") == std::string::npos; i++) {
        SDL_Delay(50);
        written = slurp(dialog_log);
    }
    CHECK(written == "dialog: " + u8(files[0]) + "\n");
}

#endif
