// The real boot (#43, ADR 0027), launch (#54, ADR 0037) and first GL frame (#117, ADR 0038):
// the recompiled game (maplethief_gen) starts in the runtime from work/slices/MapleThief.armv7,
// reaches UIApplicationMain, and UIKit's UIApplicationMain (mt_uikit_app_main) loads
// MainWindow.nib from work/ipa (-[EAGLView initWithCoder:] starts the engine and makes its EAGL
// context), sends the app delegate -application:didFinishLaunchingWithOptions: and runs the main
// run loop, whose CFRunLoopTimer ticks the engine until it presents its first GL frame. Every
// import and class on that path is the runtime's own: OpenGL ES and EAGL recorded, audio silent
// (ADR 0038), no test stand-in. Local only: it needs work/gen (python -m recomp all), the slice
// and the extracted bundle, and exits 77 (skipped) without them.
//
// Two runs, each with the unimplemented-import policy "stop", so that any import or message
// without an implementation ends the run with a trap:
//
//   boot_real_test first-frame   (runtime.boot_real, M4's exit criterion)
//     The run stops at the first -[EAGLContext presentRenderbuffer:] after GL draw calls (the
//     present hook of maplethief/gles.h). Checks: the boot trace (binds -> the 2 constructors
//     -> main -> UIApplicationMain -> EAGL context -> nib -> app-delegate -> first-frame ->
//     stop), an empty unimplemented report, the GL state at the first frame (a 480x320 RGBA8
//     renderbuffer from the CAEAGLLayer, complete framebuffer, textures, draws), the audio
//     start-up (44 OpenAL buffers and sources, the 26 effects opened), __program_vars and the
//     constructors' effects, the ObjC runtime's numbers.
//
//   boot_real_test launch        (runtime.launch_real, #54's launch and the epic's rechecks)
//     The run goes on for a few seconds of the main menu, with taps, then stops from the run
//     loop's turn hook. Checks: the delegate's window and the EAGLView in it, the main menu's
//     views, a composited render (build/test-output/..., never committed), Game Center
//     (+[GameCenterManager IsGameCenterAvailable] is YES, the menu's Game Center button presents
//     GKLeaderboardViewController, which finishes and is dismissed), and Play: with no saved
//     character the engine starts a new game on slot 0 at once (CStateMainMenu::OnKeyPressed
//     0x9462D: no slot -> LoadSaveData(0), closeMainMenu:, a CStateGame), writing theif0 and
//     quest0 to Documents/ in the data directory. No network access; nothing waits.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(MT_HAVE_GENERATED_CODE)

#include "golden.h"
#include "maplethief/audio.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/dispatch.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"

namespace {

using u32 = std::uint32_t;
namespace fs = std::filesystem;

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

bool has_line(const std::vector<std::string> &lines, const std::string &text) {
    for (const std::string &line : lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

u32 rd(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, cpu->mem + addr, 4);
    return v;
}

std::string class_of(u32 obj) {
    mt_objc *rt = mt_objc_current();
    return obj != 0 ? mt_objc_class_name(rt, mt_objc_class_of(rt, obj)) : "nil";
}

std::string rect_text(const float *f) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%g, %g, %g, %g)", f[0], f[1], f[2], f[3]);
    return buf;
}

// The frame of a view through -frame (objc_msgSend_stret into a scratch area of the guest
// stack, below sp).
bool view_frame(mt_cpu *cpu, u32 view, float out[4]) {
    if (view == 0) {
        return false;
    }
    const u32 result = (cpu->r[MT_SP] - 0x400u) & ~7u;
    std::memset(cpu->mem + result, 0, 16);
    mt_objc_msg_stret(cpu, result, view, mt_objc_sel(mt_objc_current(), "frame"), nullptr, 0);
    std::memcpy(out, cpu->mem + result, 16);
    return true;
}

// "<depth>Class (x, y, w, h)" for each view of the tree.
void list_tree(mt_cpu *cpu, u32 view, int depth, std::vector<std::string> &out) {
    float f[4];
    if (depth > 12 || !view_frame(cpu, view, f)) {
        return;
    }
    out.push_back(std::string(static_cast<std::size_t>(depth) * 2, ' ') + class_of(view) + " " +
                  rect_text(f));
    const u32 subviews = mt_objc_send0(cpu, view, "subviews");
    const u32 count = mt_objc_send0(cpu, subviews, "count");
    for (u32 i = 0; i < count && i < 200; i++) {
        list_tree(cpu, mt_objc_send1(cpu, subviews, "objectAtIndex:", i), depth + 1, out);
    }
}

std::vector<std::uint8_t> read_file(const char *path) {
    std::vector<std::uint8_t> out;
#if defined(_MSC_VER)
    std::FILE *f = nullptr;
    if (fopen_s(&f, path, "rb") != 0) {
        return out;
    }
#else
    std::FILE *f = std::fopen(path, "rb");
#endif
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

// --- UIApplicationMain and the GL frames ---------------------------------------------------

mt_app_main_args g_app{};
bool g_app_called = false;

std::int32_t app_main(mt_process *p, mt_cpu *cpu, const mt_app_main_args *args, void *user) {
    g_app = *args;
    g_app_called = true;
    return mt_uikit_app_main(p, cpu, args, user);
}

struct Frames {
    bool stop_at_first = false, seen_first = false;
    std::vector<mt_gles_frame> frames;
    mt_gles_frame first{};
    mt_gles_stats at_first{};
    mt_audio_stats audio_at_first{};
};
Frames g_frames;

// -[EAGLContext presentRenderbuffer:]: the first frame with draws ends the first-frame run.
int present(void *, mt_cpu *, const mt_gles_frame *frame) {
    g_frames.frames.push_back(*frame);
    if (frame->draws == 0 || g_frames.seen_first) {
        return 0;
    }
    g_frames.seen_first = true;
    g_frames.first = *frame;
    g_frames.at_first = mt_gles_get_stats();
    g_frames.audio_at_first = mt_audio_get_stats();
    return g_frames.stop_at_first ? 1 : 0;
}

// --- the boot -------------------------------------------------------------------------------

struct Boot {
    mt_program prog = MT_PROGRAM_GENERATED;
    mt_process *p = nullptr;
    u32 outcome = 0;
    fs::path data_dir;
};

// Configures the runtime as a platform would, loads the slice and runs it until it ends.
bool run(Boot &b, const std::vector<std::uint8_t> &slice, mt_uikit_config *ui) {
    // The bundle (MainWindow.nib, Info.plist, images, sounds) and a scratch data directory for
    // the saves and gamecenter.json.
    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    b.data_dir = fs::temp_directory_path() / "maplethief-boot-real-test";
    fs::remove_all(b.data_dir);
    fs::create_directories(b.data_dir);
    mt_stubs_set_data_dir(b.data_dir.string().c_str());
    // The unimplemented report goes to the log (and is checked). UIKit's trace channel logs
    // touches, control actions and modal controllers (checked).
    mt_diag_config diag = mt_diag_config_default();
    diag.report_path = nullptr;
    static std::string trace = "uikit"; // MAPLETHIEF_TRACE overrides it
#if defined(_MSC_VER)
    char *env = nullptr;
    std::size_t env_len = 0;
    if (_dupenv_s(&env, &env_len, "MAPLETHIEF_TRACE") == 0 && env != nullptr) {
        trace = env;
    }
    std::free(env);
#else
    if (const char *env = std::getenv("MAPLETHIEF_TRACE")) {
        trace = env;
    }
#endif
    diag.trace = trace.c_str();
    mt_diag_configure(&diag);
    mt_gles_config gl{};
    gl.present = present;
    mt_gles_configure(&gl);

    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    cfg.imports.unimplemented = MT_UNIMPLEMENTED_STOP;
    cfg.imports.log = capture;
    cfg.app_main = app_main;
    cfg.app_user = ui;
    b.p = mt_process_load(&cfg, &b.prog, slice.data(), slice.size());
    if (b.p == nullptr) {
        std::printf("FAIL: the boot could not load the slice\n");
        return false;
    }
    b.outcome = mt_process_run(b.p);
    const mt_process_result *r = mt_process_result_of(b.p);
    if (b.outcome == MT_PROCESS_TRAPPED) {
        std::printf("trap kind %u at pc 0x%08X detail 0x%08X\n", r->trap_kind, r->trap_pc,
                    r->trap_detail);
    }
    std::printf("\n");
    return true;
}

void finish(Boot &b) {
    mt_process_destroy(b.p);
    mt_gles_configure(nullptr);
    mt_cf_configure(nullptr);
    mt_stubs_set_data_dir(nullptr);
    fs::remove_all(b.data_dir);
}

// The milestones as "name value -> ...", and whether they are `expected` (value 0: any).
bool check_trace(Boot &b, const std::vector<std::pair<std::string, u32>> &expected) {
    u32 n = 0;
    const mt_milestone *m = mt_process_milestones(b.p, &n);
    std::string trace;
    for (u32 i = 0; i < n; i++) {
        trace += std::string(i != 0 ? " -> " : "") + m[i].name + " " + hex(m[i].value);
    }
    std::printf("trace: %s\n", trace.c_str());
    bool same = n == expected.size();
    for (u32 i = 0; same && i < n; i++) {
        same = expected[i].first == m[i].name &&
               (expected[i].second == 0 || expected[i].second == m[i].value);
    }
    return same;
}

// The milestones of the boot up to the app delegate (value 0 is any value: the place count,
// the stack pointer and object addresses follow the runtime).
std::vector<std::pair<std::string, u32>> boot_milestones() {
    return {
        {"image", 0x27E0},
        {"link", 0},
        {"objc", 0},
        {"place", 111},
        {"binds", 1268},
        {"objc-image", 75},
        {"objc-overrides", 1},
        {"seal", 0},
        {"stack", 0},
        {"program-vars", 0xF1370},
        {"constructor", 0xA925},
        {"atexit", 0x6639},
        {"constructor", 0x9E041},
        {"atexit", 0x9DBB1},
        {"main", 0x280D},
        {"UIApplicationMain", 0},
        // -[EAGLView initWithCoder:] -> createGLes runs while the nib loads (program-flow.md,
        // step 3), so the context comes before the nib's milestone and the delegate's launch
        {"eagl-context", 0},
        {"nib", 23},
        {"app-delegate", 0},
        {"first-frame", 1},
    };
}

// What every run checks once it ended: no import or message without an implementation.
void check_report(Boot &b) {
    u32 unimplemented = 0;
    for (u32 id = 0; id < b.prog.import_count; id++) {
        if (mt_import_unimplemented_calls(id) != 0) {
            std::printf("unimplemented import hit: %s\n", b.prog.import_descs[id].name);
            unimplemented++;
        }
    }
    check(unimplemented == 0, "no unimplemented import is hit");
    char report[4096];
    mt_report_format(report, sizeof report);
    std::printf("%s", report);
    check(mt_report_get_stats().imports == 0 && mt_report_get_stats().selectors == 0 &&
              std::strstr(report, "unimplemented report: none") != nullptr,
          "the unimplemented report is empty (\"unimplemented report: none\")");
    check(mt_objc_undeliverable_count(mt_process_objc(b.p)) == 0, "no undeliverable message");
}

// --- first-frame: M4's exit criterion ----------------------------------------------------------

int first_frame(const std::vector<std::uint8_t> &slice) {
    g_frames = Frames{};
    g_frames.stop_at_first = true;
    mt_uikit_config ui = mt_uikit_config_default();
    // No turn hook needed: the run loop runs until the present hook ends the run (a hook that
    // waits a little keeps the 1 ms engine timer from spinning the host CPU).
    ui.turn = [](void *, mt_cpu *, double, double next) {
        const double wait = std::min(std::max(0.0, next - mt_cf_absolute_time()), 0.005);
        std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        return 0;
    };
    Boot b;
    if (!run(b, slice, &ui)) {
        return 1;
    }
    const mt_process_result *r = mt_process_result_of(b.p);
    mt_cpu *cpu = mt_process_cpu(b.p);
    const mt_process_args *a = mt_process_args_of(b.p);

    check(b.outcome == MT_PROCESS_STOPPED && r->stop_reason != nullptr &&
              std::strcmp(r->stop_reason, "presentRenderbuffer: the host ended the run") == 0,
          "the run stopped at the first frame's -presentRenderbuffer: (outcome " +
              std::to_string(b.outcome) + ", " + (r->stop_reason != nullptr ? r->stop_reason : "") +
              ")");

    // The boot trace, as expected milestones.
    std::vector<std::pair<std::string, u32>> expected = boot_milestones();
    expected.emplace_back("stop", 0);
    check(check_trace(b, expected),
          "trace: binds applied -> 2 constructors -> main -> UIApplicationMain -> EAGL context "
          "-> nib (23 objects) -> app-delegate -> first-frame -> stop");
    u32 n = 0;
    const mt_milestone *m = mt_process_milestones(b.p, &n);
    u32 context = 0;
    for (u32 i = 0; i < n; i++) {
        if (std::strcmp(m[i].name, "eagl-context") == 0) {
            context = m[i].value;
        }
    }
    check(g_app_called && mt_fn_containing(g_app.caller) != nullptr &&
              mt_fn_containing(g_app.caller)->addr == 0x280D && g_app.argc == 1 &&
              g_app.argv == a->argv && g_app.principal_class_name == 0 &&
              g_app.delegate_class_name == 0,
          "UIApplicationMain(1, argv, nil, nil) is called from inside main 0x280D (lr " +
              hex(g_app.caller) + ")");

    check_report(b);

    // The first frame and the GL state it was drawn with.
    const mt_gles_stats &gl = g_frames.at_first;
    const mt_gles_frame &f = g_frames.first;
    std::printf("frames: %zu presents; the first with draws: frame %u, %ux%u format 0x%X "
                "retained %u, %u GL calls, %u draws, %u uploads\n",
                g_frames.frames.size(), f.index, f.width, f.height, f.format, f.retained_backing,
                f.calls, f.draws, f.uploads);
    std::printf("GL state: %u calls, %u contexts, current 0x%08X, %u textures (%u bytes), "
                "framebuffer %u, renderbuffer %u, texture %u, viewport (%u, %u, %u, %u), %u "
                "misuses\n",
                gl.calls, gl.contexts, gl.current_context, gl.textures, gl.texture_bytes,
                gl.bound_framebuffer, gl.bound_renderbuffer, gl.bound_texture, gl.viewport[0],
                gl.viewport[1], gl.viewport[2], gl.viewport[3], gl.misuses);
    for (u32 op = 0; op < MT_GLES_OP_COUNT; op++) {
        std::printf("  %-32s %u\n", mt_gles_op_name(op), gl.op_calls[op]);
    }
    check(gl.first_frame == f.index && f.index >= 1 && f.draws > 0 && f.calls > f.draws,
          "the first frame is presented after GL draw calls (frame " + std::to_string(f.index) +
              ": " + std::to_string(f.draws) + " glDrawArrays of " + std::to_string(f.calls) +
              " calls)");
    check(f.width == 480 && f.height == 320 && f.format == 0x8058 && f.retained_backing == 0,
          "it shows the renderbuffer -renderbufferStorage:fromDrawable: made from the "
          "CAEAGLLayer: 480x320 (the turned view's bounds), RGBA8, no retained backing");
    check(gl.contexts == 1 && context != 0 && gl.current_context == context &&
              class_of(context) == "EAGLContext",
          "one EAGLContext (OpenGL ES 1.1) is made and current (" + hex(context) + ")");
    check(gl.op_calls[MT_GLES_CheckFramebufferStatusOES] >= 1 && gl.bound_framebuffer != 0 &&
              gl.bound_renderbuffer != 0 && gl.misuses == 0,
          "the framebuffer and renderbuffer are bound, the completeness check was answered, "
          "and no GL call was misused");
    check(gl.op_calls[MT_GLES_GetRenderbufferParameterivOES] == 2,
          "the backing size is read back (glGetRenderbufferParameterivOES width and height)");
    check(gl.textures > 100 && gl.texture_bytes > 0 && gl.op_calls[MT_GLES_TexImage2D] > 100 &&
              gl.bound_texture != 0,
          "textures are made and uploaded (" + std::to_string(gl.textures) + " textures, " +
              std::to_string(gl.texture_bytes) + " bytes)");
    check(gl.viewport[0] == 0 && gl.viewport[1] == 0 && gl.viewport[2] == 480 &&
              gl.viewport[3] == 320,
          "the viewport is the 480x320 backing");
    check(!has_line(g_log, "texture disapear") && !has_line(g_log, "failed to make complete"),
          "the engine finds every texture it made (no \"text exist draw direct But texture "
          "disapear\") and a complete framebuffer");

    // Audio at the first frame: CMSoundPlayer's start-up (docs/re/audio.md), silent.
    const mt_audio_stats &au = g_frames.audio_at_first;
    std::printf("audio: %u calls, %u buffers (%u filled), %u sources, %u plays, %u files "
                "opened, %u failed, %u players (%u playing), session active %u\n",
                au.calls, au.buffers, au.buffers_filled, au.sources, au.source_plays,
                au.files_opened, au.files_failed, au.players, au.players_playing,
                au.session_active);
    check(au.buffers == 44 && au.sources == 44 && au.files_opened == 26 && au.files_failed == 0 &&
              au.buffers_filled == 26,
          "audio start-up: 44 OpenAL buffers and sources, the 26 WAV effects opened through "
          "ExtAudioFile and buffered");

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
    const mt_atexit_entry *e = mt_process_atexit_entries(b.p, &n);
    check(n == 2 && e[0].func == 0x6639 && e[1].func == 0x9DBB1,
          "2 destructors registered with __cxa_atexit");

    // The Objective-C runtime (#47, ADR 0029): every image class realized, the Chartboost
    // override, and main's [[NSAutoreleasePool alloc] init] in place.
    mt_objc *objc = mt_process_objc(b.p);
    const mt_objc_stats st = mt_objc_get_stats(objc);
    check(st.image_classes == 75 && st.categories == 4 && has_line(g_log, ", 0 problems"),
          "75 image classes and 4 categories realized with 0 problems (" +
              std::to_string(st.placeholders) + " placeholder classes)");
    check(st.overrides == 1, "1 host override: +[Chartboost sharedChartboost]");
    check(mt_objc_class_is_placeholder(objc, mt_objc_class_named(objc, "EAGLContext")) == 0 &&
              mt_objc_class_is_placeholder(objc, mt_objc_class_named(objc, "AVAudioPlayer")) == 0 &&
              mt_objc_class_is_placeholder(objc, mt_objc_class_named(objc, "AVAudioSession")) == 0,
          "EAGLContext, AVAudioSession and AVAudioPlayer are host classes, not placeholders");
    const u32 token = mt_objc_pool_push(objc);
    std::printf("autorelease pool token %u\n", token);
    mt_objc_pool_pop(objc, token);

    finish(b);
    return 0;
}

// --- launch: #54's launch and the epic's rechecks --------------------------------------------

struct Launch {
    u32 turns = 0;
    double first = 0;
    u32 window = 0, delegate = 0, controller = 0, eagl_view = 0;
    u32 game_center_available = 0, game_center_flag = 0;
    std::vector<std::string> tree, tree_after;
    int step = 0;
    bool rendered_closed = false;
    double main_menu_covered = 0; // fraction of the main menu render that is not black
    std::vector<std::string> renders;
    std::vector<std::string> log_after_gc; // the log lines between the Game Center tap and Play
    double render_seconds = 0;             // wall time spent in render(), left out of the timeline
};
Launch g_launch;
constexpr double k_run_seconds = 6.0;

// Renders the screen at 960x640 into <test output>/uikit_real/<name>.png; returns the fraction
// of pixels that are not black (-1 if nothing could be rendered).
// Its wall time is added to Launch::render_seconds: unoptimized (an MSVC Debug build), a render
// takes seconds, which the game would otherwise lose from the run's frames and taps.
double render(mt_cpu *cpu, double now, const char *name) {
    const auto started = std::chrono::steady_clock::now();
    struct Timed {
        std::chrono::steady_clock::time_point started;
        ~Timed() {
            g_launch.render_seconds +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        }
    } timed{started};
    std::vector<std::uint8_t> pixels(960u * 640u * 4u);
    if (mt_uikit_composite(cpu, now, MT_UIKIT_COMPOSITE_FULL, nullptr, pixels.data(), 960, 640,
                           960 * 4) != 0) {
        return -1;
    }
    maplethief::golden::Image img(960, 640);
    std::size_t lit = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        lit += pixels[i] + pixels[i + 1] + pixels[i + 2] > 24 ? 1u : 0u;
        const unsigned alpha = pixels[i + 3];
        for (int c = 0; c < 3; c++) {
            img.bytes()[i + c] =
                alpha == 0 ? 0
                           : static_cast<std::uint8_t>(
                                 std::min(255u, (pixels[i + c] * 255u + alpha / 2) / alpha));
        }
        img.bytes()[i + 3] = static_cast<std::uint8_t>(alpha);
    }
    const fs::path path =
        maplethief::golden::test_output_dir() / "uikit_real" / (std::string(name) + ".png");
    if (maplethief::golden::write_png(path, img)) {
        g_launch.renders.push_back(path.string());
    }
    return static_cast<double>(lit) / (960.0 * 640.0);
}

// A tap at screen point (x, y): down now, up `steps` later (see turn()).
void touch(u32 phase, float x, float y) { mt_uikit_touch(1, phase, x, y); }

// After every turn of the main run loop: checks the launch at the first turn, then taps through
// the main menu on a timeline, and ends the run after k_run_seconds.
int turn(void *, mt_cpu *cpu, double now, double next) {
    Launch &l = g_launch;
    if (l.turns++ == 0) {
        l.first = now;
        const u32 app = mt_objc_send0(cpu, mt_objc_class_named(mt_objc_current(), "UIApplication"),
                                      "sharedApplication");
        l.delegate = mt_objc_send0(cpu, app, "delegate");
        l.window = mt_objc_send0(cpu, l.delegate, "window");
        const u32 subviews = mt_objc_send0(cpu, l.window, "subviews");
        l.eagl_view = mt_objc_send0(cpu, subviews, "count") == 1
                          ? mt_objc_send1(cpu, subviews, "objectAtIndex:", 0)
                          : 0u;
        l.controller = rd(cpu, 0x11A97C);
        // Game Center (#52): available on the real path, so -initWithCoder: made the manager
        // and the main menu offers the leaderboard (EAGLView.getGameCenter).
        l.game_center_available =
            mt_objc_send0(cpu, mt_objc_class_named(mt_objc_current(), "GameCenterManager"),
                          "IsGameCenterAvailable") &
            0xFFu;
        l.game_center_flag = mt_objc_send0(cpu, l.eagl_view, "getGameCenter") & 0xFFu;
        render(cpu, now, "turn1");
    }
    const double t = now - l.first - l.render_seconds; // the run's time, renders left out
    // The timeline (screen points): the main menu is up after its 2 s slide-in. Each step is a
    // tap: down at its time, up 0.1 s later. First the promo banner's close button (frame
    // (445, 210, 35, 35)): the banner (0, 190, 480, 130) lies over the Game Center button, as
    // on the device. Then Game Center (MainMenuGameCenter.png, (376, 262, 57, 57); More Games is at
    // (436, 262)), then Play
    // ((46, 148, 120, 48)).
    struct Tap {
        double at;
        float x, y;
    };
    const Tap taps[] = {{2.0, 462, 227}, {2.4, 404, 290}, {3.0, 106, 165}};
    constexpr int k_tap_count = static_cast<int>(sizeof taps / sizeof taps[0]);
    if (t >= 2.0 && l.tree.empty()) {
        list_tree(cpu, l.window, 0, l.tree);
        l.main_menu_covered = render(cpu, now, "main_menu");
    }
    // The main menu without the promo banner, for the comparison with the device (#54).
    if (t >= 2.95 && !l.rendered_closed) {
        render(cpu, now, "main_menu_closed");
        l.rendered_closed = true;
    }
    if (l.step < 2 * k_tap_count) {
        const Tap &tap = taps[l.step / 2];
        const bool down = l.step % 2 == 0;
        if (t >= tap.at + (down ? 0.0 : 0.1)) {
            if (down && l.step / 2 == 2) {
                l.log_after_gc = g_log; // before Play: the Game Center part of the log
            }
            touch(down ? MT_UIKIT_TOUCH_BEGAN : MT_UIKIT_TOUCH_ENDED, tap.x, tap.y);
            l.step++;
        }
    }
    if (t >= k_run_seconds) {
        list_tree(cpu, l.window, 0, l.tree_after);
        render(cpu, now, "after_play");
        return 1;
    }
    const double wait = std::min(std::max(0.0, next - mt_cf_absolute_time()), 0.02);
    std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    return 0;
}

int launch(const std::vector<std::uint8_t> &slice) {
    g_frames = Frames{};
    g_launch = Launch{};
    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = turn;
    Boot b;
    const auto started = std::chrono::steady_clock::now();
    if (!run(b, slice, &ui)) {
        return 1;
    }
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const mt_process_result *r = mt_process_result_of(b.p);
    mt_cpu *cpu = mt_process_cpu(b.p);

    check(b.outcome == MT_PROCESS_STOPPED && r->stop_reason != nullptr &&
              std::strcmp(r->stop_reason, "UIApplicationMain: the host ended the run loop") == 0,
          "the run stopped in UIApplicationMain's run loop after " +
              std::to_string(g_launch.turns) + " turns (outcome " + std::to_string(b.outcome) +
              ", " + (r->stop_reason != nullptr ? r->stop_reason : "") + ")");
    std::vector<std::pair<std::string, u32>> expected = boot_milestones();
    expected.emplace_back("stop", 0);
    check(check_trace(b, expected),
          "trace: ... -> app-delegate -> first-frame -> stop (the turn hook)");
    check_report(b);
    check(wall < k_run_seconds + 20, "nothing waits: " + std::to_string(k_run_seconds) +
                                         " s of game time took " + std::to_string(wall) + " s");

    // The launch (#54): the delegate's window is the nib's, with the EAGLView in it.
    const Launch &l = g_launch;
    float f[4] = {0, 0, 0, 0};
    std::printf("delegate %s 0x%08X, window %s 0x%08X, view %s 0x%08X, controller %s 0x%08X\n",
                class_of(l.delegate).c_str(), l.delegate, class_of(l.window).c_str(), l.window,
                class_of(l.eagl_view).c_str(), l.eagl_view, class_of(l.controller).c_str(),
                l.controller);
    check(class_of(l.delegate) == "MapleThiefAppDelegate",
          "the application's delegate is the nib's MapleThiefAppDelegate");
    check(class_of(l.window) == "UIWindow" && view_frame(cpu, l.window, f) && f[0] == 0 &&
              f[1] == 0 && f[2] == 320 && f[3] == 480,
          "its window outlet is the nib's 320x480 UIWindow " + rect_text(f));
    check(class_of(l.eagl_view) == "EAGLView" && view_frame(cpu, l.eagl_view, f) && f[0] == 0 &&
              f[1] == 0 && f[2] == 320 && f[3] == 480,
          "the window's one subview is the EAGLView, turned to landscape " + rect_text(f));
    check(class_of(l.controller) == "EAGLViewController",
          "-application:didFinishLaunchingWithOptions: stored its EAGLViewController in 0x11A97C");
    float gl_rect[4] = {0, 0, 0, 0};
    u32 dw = 0, dh = 0;
    check(mt_uikit_gl_rect(gl_rect, &dw, &dh) == 1 && gl_rect[0] == 0 && gl_rect[1] == 0 &&
              gl_rect[2] == 480 && gl_rect[3] == 320 && dw == 480 && dh == 320,
          "the GL view fills the 480x320 landscape screen " + rect_text(gl_rect));
    const mt_gles_stats gl = mt_gles_get_stats();
    check(gl.frames > 50 && gl.misuses == 0,
          "the engine keeps presenting frames (" + std::to_string(gl.frames) + " in " +
              std::to_string(k_run_seconds) + " s) with no GL misuse");
    std::printf("the view tree after 2 s:\n");
    for (const std::string &line : l.tree) {
        std::printf("  %s\n", line.c_str());
    }
    std::printf("the view tree after Play:\n");
    for (const std::string &line : l.tree_after) {
        std::printf("  %s\n", line.c_str());
    }
    for (const std::string &path : l.renders) {
        std::printf("render: %s\n", path.c_str());
    }
    // The shell's views in their places: the frames come from -[EAGLView startApp] 0x4501
    // (TouchKeyPadView and MainMenuView fill the landscape screen) and the main menu's
    // -drawRect: (its 480x320 MainMenuBackground.png, the Play and Game Center buttons).
    check(has_line(l.tree, "  EAGLView (0, 0, 320, 480)") &&
              has_line(l.tree, "    TouchKeyPadView (0, 0, 480, 320)") &&
              has_line(l.tree, "    MainMenuView (0, 0, 480, 320)") &&
              has_line(l.tree, "      UIImageView (0, 0, 480, 320)") &&
              has_line(l.tree, "      UIButton (46, 148, 120, 48)") &&
              has_line(l.tree, "      UIButton (376, 262, 57, 57)"),
          "after 2 s the main menu is up: TouchKeyPadView and MainMenuView fill the screen, "
          "its background, the Play and the Game Center buttons in place");
    check(l.main_menu_covered > 0.9,
          "the composited main menu covers the screen (" +
              std::to_string(static_cast<int>(l.main_menu_covered * 100)) + "% not black)");

    // Recheck (#52, #54): Game Center on the real path.
    check(l.game_center_available == 1 && l.game_center_flag == 1,
          "+[GameCenterManager IsGameCenterAvailable] is YES and the EAGLView made its manager "
          "(-getGameCenter YES)");
    check(has_line(l.log_after_gc, "uikit: action -[MainMenuView clickGameCenter:]") &&
              has_line(l.log_after_gc, "presentModalViewController:GKLeaderboardViewController") &&
              has_line(l.log_after_gc,
                       "dismissModalViewControllerAnimated: GKLeaderboardViewController"),
          "the Game Center button presents GKLeaderboardViewController modally; it gets "
          "-viewDidAppear:, finishes, and the EAGLView's delegate method dismisses it");

    // Play with no saved character: a new game on slot 0 at once.
    check(has_line(g_log, "uikit: action -[MainMenuView clickPlay:]"),
          "a tap at (106, 165) reaches the Play button and sends -[MainMenuView clickPlay:]");
    check(!l.tree_after.empty() && !has_line(l.tree_after, "MainMenuView"),
          "with no saved character the engine starts a new game: it closes the main menu "
          "(-[EAGLView closeMainMenu:]) and changes to CStateGame");
    const fs::path documents = b.data_dir / "Documents";
    std::error_code ec;
    check(fs::file_size(documents / "theif0", ec) == 1124 &&
              fs::file_size(documents / "quest0", ec) == 4781,
          "slot 0's new character is saved: Documents/theif0 (1,124 bytes) and quest0 (4,781 "
          "bytes) in the data directory");
    for (const auto &entry : fs::recursive_directory_iterator(b.data_dir, ec)) {
        std::error_code size_ec;
        const std::uintmax_t size = entry.is_regular_file(size_ec) ? entry.file_size(size_ec) : 0;
        std::printf("data: %s (%ju bytes)\n", entry.path().string().c_str(), size);
    }
    finish(b);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    const std::vector<std::uint8_t> slice = read_file(MT_ARMV7_SLICE);
    if (slice.empty() || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s is missing (run scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    const std::string mode = argc > 1 ? argv[1] : "first-frame";
    int rc = 1;
    if (mode == "first-frame") {
        rc = first_frame(slice);
    } else if (mode == "launch") {
        rc = launch(slice);
    } else {
        std::printf("usage: boot_real_test [first-frame | launch]\n");
        return 2;
    }
    std::printf("%d failure(s)\n", g_failures);
    return rc == 0 && g_failures == 0 ? 0 : 1;
}

#else

int main() {
    std::printf("skipped: built without the generated code (work/gen; python -m recomp all)\n");
    return 77;
}

#endif
