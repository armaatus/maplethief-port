// The real boot (#43, ADR 0027) and launch (#54, ADR 0037): the recompiled game
// (maplethief_gen) starts in the runtime from work/slices/MapleThief.armv7, reaches
// UIApplicationMain, and UIKit's UIApplicationMain (mt_uikit_app_main) loads MainWindow.nib
// from work/ipa (-[EAGLView initWithCoder:] starts the engine), sends the app delegate
// -application:didFinishLaunchingWithOptions: and runs the main run loop for a few seconds.
// Local only: it needs work/gen (python -m recomp all), the slice and the extracted bundle,
// and exits 77 (skipped) without them.
//
// Checks, with the unimplemented-import policy "stop", so that any import without an
// implementation ends the run with a trap:
//   - the boot trace: binds applied -> the 2 static constructors (each registering its
//     destructor with __cxa_atexit) -> main -> UIApplicationMain -> nib -> app-delegate ->
//     stop (the test's turn hook ends the run), and nothing else
//   - UIApplicationMain(1, argv, nil, nil) is called from inside main
//   - the delegate got its launch callback with the nib's window: the delegate's window
//     outlet is the visible UIWindow (320x480) whose subview is the EAGLView, and the
//     EAGLViewController it made is in the global 0x11A97C
//   - the shell's views in their places (frames from the nib and the game's code), the GL
//     view's rectangle, and renders of the screen written to the test output directory
//     (build/test-output/..., never committed) for comparison with a device screenshot
//   - no import went to the default handler; the unimplemented report is empty
//   - __program_vars and the constructors' effects in guest memory
//
// OpenGL ES and EAGL are #58/#117's, audio (OpenAL, AudioToolbox, AVFoundation) M6's
// (#61, #63): the test stands them in (their imports through cfg.hle, EAGLContext's,
// AVAudioSession's and AVAudioPlayer's methods as host categories on their placeholder
// classes, the kEAGL... and AVAudioSession string constants written into their data imports)
// and counts the calls. Everything else the launch calls is the runtime's own implementation.
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

#include "foundation/foundation.h"
#include "golden.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/dispatch.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"

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

u32 rd(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, cpu->mem + addr, 4);
    return v;
}

// --- OpenGL ES and EAGL stand-ins (#58, #117) ----------------------------------------------------

const mt_program *g_prog = nullptr;
u32 g_gl_calls = 0, g_eagl_calls = 0;

// Every gl* import: nothing drawn. Names, ids and sizes the game reads back are answered as a
// device would: glGen*(n, ids) writes ids, glCheckFramebufferStatusOES is complete, the
// renderbuffer is 480x320.
void gl_stand_in(mt_cpu *cpu, u32 id) {
    g_gl_calls++;
    const char *name = g_prog->import_descs[id].name;
    mt_args a = mt_args_begin(cpu);
    static u32 next_name = 1;
    if (std::strncmp(name, "_glGen", 6) == 0) {
        const u32 n = mt_arg_u32(&a), out = mt_arg_u32(&a);
        for (u32 i = 0; i < n && i < 64; i++) {
            const u32 v = next_name++;
            std::memcpy(cpu->mem + out + 4 * i, &v, 4);
        }
        mt_ret_u32(cpu, 0);
    } else if (std::strcmp(name, "_glCheckFramebufferStatusOES") == 0) {
        mt_ret_u32(cpu, 0x8CD5); // GL_FRAMEBUFFER_COMPLETE_OES
    } else if (std::strcmp(name, "_glGetRenderbufferParameterivOES") == 0) {
        mt_arg_u32(&a);
        const u32 pname = mt_arg_u32(&a), out = mt_arg_u32(&a);
        const u32 v = pname == 0x8D42 ? 480u : pname == 0x8D43 ? 320u : 0u; // WIDTH, HEIGHT
        std::memcpy(cpu->mem + out, &v, 4);
        mt_ret_u32(cpu, 0);
    } else if (std::strcmp(name, "_glGetError") == 0 || std::strcmp(name, "_glIsTexture") == 0) {
        mt_ret_u32(cpu, 0);
    } else {
        mt_ret_u32(cpu, 0);
    }
}

// OpenAL, AudioSession and ExtAudioFile (M6): silence. alcOpenDevice and alcCreateContext
// give a non-null handle, alGen* names; ExtAudioFileOpenURL fails (no sound is loaded).
u32 g_audio_calls = 0;
void audio_stand_in(mt_cpu *cpu, u32 id) {
    g_audio_calls++;
    const char *name = g_prog->import_descs[id].name;
    mt_args a = mt_args_begin(cpu);
    static u32 next_name = 1;
    if (std::strcmp(name, "_alcOpenDevice") == 0 || std::strcmp(name, "_alcCreateContext") == 0 ||
        std::strcmp(name, "_alcGetCurrentContext") == 0 ||
        std::strcmp(name, "_alcGetContextsDevice") == 0) {
        mt_ret_u32(cpu, 0x5A5A0000u);
    } else if (std::strcmp(name, "_alcMakeContextCurrent") == 0) {
        mt_ret_u32(cpu, 1);
    } else if (std::strncmp(name, "_alGen", 6) == 0) {
        const u32 n = mt_arg_u32(&a), out = mt_arg_u32(&a);
        for (u32 i = 0; i < n && i < 256; i++) {
            const u32 v = next_name++;
            std::memcpy(cpu->mem + out + 4 * i, &v, 4);
        }
        mt_ret_u32(cpu, 0);
    } else if (std::strcmp(name, "_ExtAudioFileOpenURL") == 0) {
        mt_ret_u32(cpu, static_cast<u32>(-43)); // fnfErr
    } else {
        mt_ret_u32(cpu, 0); // AL_NO_ERROR, noErr, NULL
    }
}

// CC_MD5(data, len, md) (CommonCrypto, not in the runtime yet; #117): a fixed digest, for the
// device identifier of -[UIDevice(IdentifierAddition) uniqueGlobalDeviceIdentifier].
u32 g_md5_calls = 0;
void md5_stand_in(mt_cpu *cpu, u32) {
    g_md5_calls++;
    mt_args a = mt_args_begin(cpu);
    mt_arg_u32(&a);
    mt_arg_u32(&a);
    const u32 md = mt_arg_u32(&a);
    std::memset(cpu->mem + md, 0x5A, 16);
    mt_ret_u32(cpu, md);
}

bool is_audio_import(const char *name) {
    return std::strncmp(name, "_al", 3) == 0 || std::strncmp(name, "_Audio", 6) == 0 ||
           std::strncmp(name, "_ExtAudio", 9) == 0;
}

void av_shared(mt_cpu *cpu) {
    static u32 session = 0;
    static mt_objc *owner = nullptr;
    g_audio_calls++;
    if (owner != mt_objc_current()) {
        owner = mt_objc_current();
        session = mt_objc_create_instance(owner, mt_objc_self(cpu));
    }
    mt_ret_u32(cpu, session);
}
void av_self(mt_cpu *cpu) {
    g_audio_calls++;
    mt_ret_u32(cpu, mt_objc_self(cpu));
}
void av_yes(mt_cpu *cpu) {
    g_audio_calls++;
    mt_ret_u32(cpu, 1);
}
const mt_objc_host_method k_av_session_methods[] = {
    {"setCategory:error:", av_yes},
    {"setActive:error:", av_yes},
    {"setDelegate:", av_yes},
};
const mt_objc_host_method k_av_session_class_methods[] = {
    {"sharedInstance", av_shared},
};
const mt_objc_host_category k_av_session = {"AVAudioSession", k_av_session_methods, 3,
                                            k_av_session_class_methods, 1};
const mt_objc_host_method k_av_player_methods[] = {
    {"initWithContentsOfURL:error:", av_self},
    {"prepareToPlay", av_yes},
    {"setNumberOfLoops:", av_yes},
    {"setVolume:", av_yes},
    {"setDelegate:", av_yes},
    {"play", av_yes},
    {"pause", av_yes},
    {"stop", av_yes},
    {"isPlaying", av_yes},
    {"setCurrentTime:", av_yes},
};
const mt_objc_host_category k_av_player = {
    "AVAudioPlayer", k_av_player_methods,
    sizeof k_av_player_methods / sizeof k_av_player_methods[0], nullptr, 0};

void eagl_self(mt_cpu *cpu) {
    g_eagl_calls++;
    mt_ret_u32(cpu, mt_objc_self(cpu));
}
void eagl_yes(mt_cpu *cpu) {
    g_eagl_calls++;
    mt_ret_u32(cpu, 1);
}
const mt_objc_host_method k_eagl_methods[] = {
    {"initWithAPI:", eagl_self},
    {"renderbufferStorage:fromDrawable:", eagl_yes},
    {"presentRenderbuffer:", eagl_yes},
};
const mt_objc_host_method k_eagl_class_methods[] = {
    {"setCurrentContext:", eagl_yes},
};
const mt_objc_host_category k_eagl = {"EAGLContext", k_eagl_methods, 3, k_eagl_class_methods, 1};

// --- UIApplicationMain ---------------------------------------------------------------------

mt_app_main_args g_app{};
bool g_app_called = false;

struct Launch {
    u32 turns = 0;
    double first = 0;
    bool checked = false;
    u32 window = 0, delegate = 0, controller = 0, eagl_view = 0;
    std::vector<std::string> tree, tree_after;
    int tap = 0;
    double main_menu_covered = 0; // fraction of the main menu render that is not black
    std::vector<std::string> renders;
};
Launch g_launch;
constexpr double k_run_seconds = 4.0;

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

std::string class_of(mt_cpu *cpu, u32 obj) {
    (void)cpu;
    mt_objc *rt = mt_objc_current();
    return obj != 0 ? mt_objc_class_name(rt, mt_objc_class_of(rt, obj)) : "nil";
}

// "<depth>Class (x, y, w, h)" for each view of the tree.
void list_tree(mt_cpu *cpu, u32 view, int depth, std::vector<std::string> &out) {
    float f[4];
    if (depth > 12 || !view_frame(cpu, view, f)) {
        return;
    }
    out.push_back(std::string(static_cast<std::size_t>(depth) * 2, ' ') + class_of(cpu, view) +
                  " " + rect_text(f));
    const u32 subviews = mt_objc_send0(cpu, view, "subviews");
    const u32 count = mt_objc_send0(cpu, subviews, "count");
    for (u32 i = 0; i < count && i < 200; i++) {
        list_tree(cpu, mt_objc_send1(cpu, subviews, "objectAtIndex:", i), depth + 1, out);
    }
}

// Renders the screen at 960x640 into <test output>/uikit_real/<name>.png; returns the fraction
// of pixels that are not black (-1 if nothing could be rendered).
double render(mt_cpu *cpu, double now, const char *name) {
    std::vector<std::uint8_t> pixels(960u * 640u * 4u);
    if (mt_uikit_composite(cpu, now, MT_UIKIT_COMPOSITE_FULL, nullptr, pixels.data(), 960, 640,
                           960 * 4) != 0) {
        return -1;
    }
    maplethief::golden::Image img(960, 640);
    std::size_t lit = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        lit += pixels[i] + pixels[i + 1] + pixels[i + 2] > 24 ? 1u : 0u;
        const unsigned a = pixels[i + 3];
        for (int c = 0; c < 3; c++) {
            img.bytes()[i + c] =
                a == 0
                    ? 0
                    : static_cast<std::uint8_t>(std::min(255u, (pixels[i + c] * 255u + a / 2) / a));
        }
        img.bytes()[i + 3] = static_cast<std::uint8_t>(a);
    }
    const std::filesystem::path path =
        maplethief::golden::test_output_dir() / "uikit_real" / (std::string(name) + ".png");
    if (maplethief::golden::write_png(path, img)) {
        g_launch.renders.push_back(path.string());
    }
    return static_cast<double>(lit) / (960.0 * 640.0);
}

bool has_line(const std::vector<std::string> &lines, const std::string &text) {
    for (const std::string &line : lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// After every turn of the main run loop: checks the launch at the first turn, renders the
// screen now and then, and ends the run after k_run_seconds.
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
        render(cpu, now, "turn1");
    }
    const double t = now - l.first;
    if (t >= 2.0 && l.tree.empty()) {
        list_tree(cpu, l.window, 0, l.tree);
        l.main_menu_covered = render(cpu, now, "main_menu");
    }
    // A tap on the main menu's Play button (screen points; its frame is (46, 148, 120, 48)).
    if (t >= 2.2 && l.tap == 0) {
        mt_uikit_touch(1, MT_UIKIT_TOUCH_BEGAN, 106, 165);
        l.tap = 1;
    } else if (t >= 2.3 && l.tap == 1) {
        mt_uikit_touch(1, MT_UIKIT_TOUCH_ENDED, 106, 165);
        l.tap = 2;
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

std::int32_t app_main(mt_process *p, mt_cpu *cpu, const mt_app_main_args *args, void *user) {
    g_app = *args;
    g_app_called = true;
    return mt_uikit_app_main(p, cpu, args, user);
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
    if (slice.empty() || !std::filesystem::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s is missing (run scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }

    // The bundle (MainWindow.nib, Info.plist, images) and a scratch data directory for the
    // stubs' gamecenter.json.
    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    const std::filesystem::path data_dir =
        std::filesystem::temp_directory_path() / "maplethief-boot-real-test";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);
    mt_stubs_set_data_dir(data_dir.string().c_str());
    // The unimplemented report goes to the log (and is checked below).
    mt_diag_config diag = mt_diag_config_default();
    diag.report_path = nullptr;
    // UIKit's trace channel logs touches and control actions (checked below).
    diag.trace =
        std::getenv("MAPLETHIEF_TRACE") != nullptr ? std::getenv("MAPLETHIEF_TRACE") : "uikit";
    mt_diag_configure(&diag);

    mt_program prog = MT_PROGRAM_GENERATED;
    g_prog = &prog;
    static std::vector<mt_hle_entry> gl;
    for (u32 id = 0; id < prog.import_count; id++) {
        const char *name = prog.import_descs[id].name;
        if (std::strncmp(name, "_gl", 3) == 0) {
            gl.push_back(mt_hle_entry{name, gl_stand_in, nullptr});
        } else if (is_audio_import(name) &&
                   (prog.import_descs[id].flags & MT_IMPORT_FUNCTION) != 0) {
            gl.push_back(mt_hle_entry{name, audio_stand_in, nullptr});
        } else if (std::strcmp(name, "_CC_MD5") == 0) {
            gl.push_back(mt_hle_entry{name, md5_stand_in, nullptr});
        }
    }
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    cfg.imports.unimplemented = MT_UNIMPLEMENTED_STOP;
    cfg.imports.log = capture;
    cfg.hle = gl.data();
    cfg.hle_count = static_cast<u32>(gl.size());
    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = turn;
    cfg.app_main = app_main;
    cfg.app_user = &ui;
    mt_process *p = mt_process_load(&cfg, &prog, slice.data(), slice.size());
    if (p == nullptr) {
        std::printf("FAIL: the boot could not load the slice\n");
        return 1;
    }
    check(mt_objc_add_host_category(mt_process_objc(p), &k_eagl) == 0 &&
              mt_objc_add_host_category(mt_process_objc(p), &k_av_session) == 0 &&
              mt_objc_add_host_category(mt_process_objc(p), &k_av_player) == 0,
          "EAGLContext's, AVAudioSession's and AVAudioPlayer's stand-in methods are added to "
          "their placeholder classes");
    // kEAGLDrawablePropertyColorFormat etc.: NSString constants (their names without "_k").
    u32 eagl_constants = 0;
    for (u32 id = 0; id < prog.import_count; id++) {
        const char *name = prog.import_descs[id].name;
        const u32 addr = mt_process_import_addr(p, id);
        if ((std::strncmp(name, "_kEAGL", 6) == 0 ||
             std::strncmp(name, "_AVAudioSessionCategory", 23) == 0) &&
            addr != 0) {
            const u32 str = mt_ns::string_from_utf8(mt_process_cpu(p), name + 2);
            std::memcpy(mt_process_cpu(p)->mem + addr, &str, 4);
            eagl_constants++;
        }
    }
    check(eagl_constants == 4, "the 3 kEAGL... constants and AVAudioSessionCategoryAmbient "
                               "are stood in");
    const u32 outcome = mt_process_run(p);
    const mt_process_result *r = mt_process_result_of(p);
    mt_cpu *cpu = mt_process_cpu(p);
    const mt_process_args *a = mt_process_args_of(p);

    std::printf("\n");
    check(outcome == MT_PROCESS_STOPPED && g_app_called && r->stop_reason != nullptr &&
              std::strcmp(r->stop_reason, "UIApplicationMain: the host ended the run loop") == 0,
          "the run stopped in UIApplicationMain's run loop after " +
              std::to_string(g_launch.turns) + " turns (outcome " + std::to_string(outcome) + ", " +
              (r->stop_reason != nullptr ? r->stop_reason : "") + ")");
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
        {"nib", 23},
        {"app-delegate", 0},
        {"stop", 0},
    };
    bool same = n == expected.size();
    for (u32 i = 0; same && i < n; i++) {
        same = expected[i].first == m[i].name &&
               (expected[i].second == 0 || expected[i].second == m[i].value);
    }
    check(same, "trace: binds applied -> 2 constructors -> main -> UIApplicationMain -> nib "
                "(23 objects) -> app-delegate -> stop");

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
    check(unimplemented == 0, "no unimplemented import is hit");
    char report[4096];
    mt_report_format(report, sizeof report);
    std::printf("%s", report);
    check(mt_report_get_stats().imports == 0 && mt_report_get_stats().selectors == 0,
          "the unimplemented report is empty");

    // The launch (#54): the delegate's window is the nib's, with the EAGLView in it.
    const Launch &l = g_launch;
    float f[4] = {0, 0, 0, 0};
    std::printf("delegate %s 0x%08X, window %s 0x%08X, view %s 0x%08X, controller %s 0x%08X\n",
                class_of(cpu, l.delegate).c_str(), l.delegate, class_of(cpu, l.window).c_str(),
                l.window, class_of(cpu, l.eagl_view).c_str(), l.eagl_view,
                class_of(cpu, l.controller).c_str(), l.controller);
    check(class_of(cpu, l.delegate) == "MapleThiefAppDelegate",
          "the application's delegate is the nib's MapleThiefAppDelegate");
    check(class_of(cpu, l.window) == "UIWindow" && view_frame(cpu, l.window, f) && f[0] == 0 &&
              f[1] == 0 && f[2] == 320 && f[3] == 480,
          "its window outlet is the nib's 320x480 UIWindow " + rect_text(f));
    check(class_of(cpu, l.eagl_view) == "EAGLView" && view_frame(cpu, l.eagl_view, f) &&
              f[0] == 0 && f[1] == 0 && f[2] == 320 && f[3] == 480,
          "the window's one subview is the EAGLView, turned to landscape " + rect_text(f));
    check(class_of(cpu, l.controller) == "EAGLViewController",
          "-application:didFinishLaunchingWithOptions: stored its EAGLViewController in 0x11A97C");
    float gl_rect[4] = {0, 0, 0, 0};
    u32 dw = 0, dh = 0;
    check(mt_uikit_gl_rect(gl_rect, &dw, &dh) == 1 && gl_rect[0] == 0 && gl_rect[1] == 0 &&
              gl_rect[2] == 480 && gl_rect[3] == 320 && dw == 480 && dh == 320,
          "the GL view fills the 480x320 landscape screen " + rect_text(gl_rect));
    check(g_gl_calls > 0 && g_eagl_calls > 0, "the stand-ins saw " + std::to_string(g_gl_calls) +
                                                  " gl*, " + std::to_string(g_eagl_calls) +
                                                  " EAGLContext and " +
                                                  std::to_string(g_audio_calls) + " audio calls");
    std::printf("the view tree after 2 s:\n");
    for (const std::string &line : l.tree) {
        std::printf("  %s\n", line.c_str());
    }
    std::printf("the view tree after the tap on Play:\n");
    for (const std::string &line : l.tree_after) {
        std::printf("  %s\n", line.c_str());
    }
    for (const std::string &path : l.renders) {
        std::printf("render: %s\n", path.c_str());
    }
    // The shell's views in their places: the frames come from -[EAGLView startApp] 0x4501
    // (TouchKeyPadView and MainMenuView fill the landscape screen) and the main menu's
    // -drawRect: (its 480x320 MainMenuBackground.png, the Play button).
    check(has_line(l.tree, "  EAGLView (0, 0, 320, 480)") &&
              has_line(l.tree, "    TouchKeyPadView (0, 0, 480, 320)") &&
              has_line(l.tree, "    MainMenuView (0, 0, 480, 320)") &&
              has_line(l.tree, "      UIImageView (0, 0, 480, 320)") &&
              has_line(l.tree, "      UIButton (46, 148, 120, 48)"),
          "after 2 s the main menu is up: TouchKeyPadView and MainMenuView fill the screen, "
          "its background and Play button in place");
    check(l.main_menu_covered > 0.9,
          "the composited main menu covers the screen (" +
              std::to_string(static_cast<int>(l.main_menu_covered * 100)) + "% not black)");
    check(has_line(g_log, "uikit: -[UIButton") &&
              has_line(g_log, "uikit: action -[MainMenuView clickPlay:]"),
          "a tap at (106, 165) reaches the Play button and sends -[MainMenuView clickPlay:]");
    check(!l.tree_after.empty() && !has_line(l.tree_after, "MainMenuView"),
          "after it the engine closed the main menu (-[EAGLView closeMainMenu:])");

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
    // placeholder classes, the Chartboost override, and main's [[NSAutoreleasePool alloc] init]
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
    check(st.overrides == 1, "1 host override: +[Chartboost sharedChartboost] (StoreKit and "
                             "GameKit are host classes, #52)");
    check(mt_objc_undeliverable_count(objc) == 0, "no undeliverable message");
    const u32 token = mt_objc_pool_push(objc);
    check(token == 2, "main's NSAutoreleasePool is in place (a new pool is the 2nd, token " +
                          std::to_string(token) + ")");
    mt_objc_pool_pop(objc, token);

    mt_process_destroy(p);
    mt_cf_configure(nullptr);
    mt_stubs_set_data_dir(nullptr);
    std::filesystem::remove_all(data_dir);
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

#else

int main() {
    std::printf("skipped: built without the generated code (work/gen; python -m recomp all)\n");
    return 77;
}

#endif
