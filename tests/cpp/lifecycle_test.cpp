// The app lifecycle (#128, ADR 0063) with synthetic data only: Lifecycle's mapping of SDL's
// window and app events to the delegate's messages on the desktop and on iOS (the home gesture
// and Control Center, as SDL 3.4 sends them), the quits, the fixed-step mode's scripted
// events, game.cpp's scripts of --minimise, --background and --quit, and SDL events pushed on a
// dummy-driver window (SDL_PushEvent: the app events through the event watch, as on iOS) into a
// synthetic guest: a delegate that records its messages, and a GL view presenting through a
// present hook that pumps SDL's queue first, as game.cpp's does. Each message must arrive once
// and in order, and nothing may reach the window between DID_ENTER_BACKGROUND and
// WILL_ENTER_FOREGROUND, not even the frame whose present pumped the event.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "game.h"
#include "gles/gles_internal.h"
#include "lifecycle.h"
#include "maplethief/gles.h"
#include "maplethief/uikit.h"
#include "present.h"
#include "uikit/uikit_internal.h"

#include "gles_fixture.h"

namespace {

namespace fe = maplethief::frontend;
using fe::AppMessage;

// --- the delegate's messages, by event ----------------------------------------------------------

SDL_Event event(std::uint32_t type) {
    SDL_Event e{};
    e.type = type;
    return e;
}

constexpr AppMessage kResign = AppMessage::kWillResignActive;
constexpr AppMessage kActive = AppMessage::kDidBecomeActive;
constexpr AppMessage kTerminate = AppMessage::kWillTerminate;

// What a Lifecycle does with a sequence: the messages each event queues, and whether it is
// hidden after each.
struct Step {
    std::uint32_t type;
    std::vector<AppMessage> queued;
    bool hidden;
};

// A guest that only answers mt_uikit_lifecycle_event: a delegate recording its messages.
std::vector<std::string> g_messages;
u32 g_app_arg = 0;
void delegate_resign(mt_cpu *cpu) {
    g_messages.emplace_back("applicationWillResignActive:");
    g_app_arg = cpu->r[2];
}
void delegate_active(mt_cpu *cpu) {
    g_messages.emplace_back("applicationDidBecomeActive:");
    g_app_arg = cpu->r[2];
}
void delegate_terminate(mt_cpu *cpu) {
    g_messages.emplace_back("applicationWillTerminate:");
    g_app_arg = cpu->r[2];
}
const mt_objc_host_method k_delegate_methods[] = {
    {"applicationWillResignActive:", delegate_resign},
    {"applicationDidBecomeActive:", delegate_active},
    {"applicationWillTerminate:", delegate_terminate}};
const mt_objc_host_class k_lifecycle_delegate = {
    "LifecycleTestDelegate", "NSObject", 4, k_delegate_methods, 3, nullptr, 0};
// One that implements none of them.
const mt_objc_host_class k_mute_delegate = {
    "LifecycleMuteDelegate", "NSObject", 4, nullptr, 0, nullptr, 0};

struct Guest : Env {
    u32 app = 0, delegate = 0;
    explicit Guest(const mt_objc_host_class *cls = &k_lifecycle_delegate) {
        g_messages.clear();
        g_app_arg = 0;
        REQUIRE(mt_objc_add_host_class(rt, cls, 0, 0) == 0);
        mt_ui::State &st = mt_ui::state(&cpu);
        app = send(send(klass("UIApplication"), "alloc"), "init");
        st.app = app;
        delegate = send(send(klass(cls->name), "alloc"), "init");
        st.delegate = delegate;
    }
};

// Runs `steps` through a fresh Lifecycle on a Guest (scripted, so no event watch is needed),
// delivering after each, and checks each step's messages and hidden state.
void run_steps(bool minimise_resigns, const std::vector<Step> &steps, const char *what) {
    Guest g;
    fe::Lifecycle l(minimise_resigns);
    std::size_t sent = 0;
    for (std::size_t i = 0; i < steps.size(); i++) {
        const Step &s = steps[i];
        INFO(what << ", step " << i << ": " << fe::lifecycle_event_name(s.type));
        l.handle(event(s.type), true);
        CHECK(l.deliver(&g.cpu, static_cast<double>(i)) == static_cast<int>(s.queued.size()));
        const std::vector<fe::AppMessageRecord> d = l.delivered();
        REQUIRE(d.size() == sent + s.queued.size());
        for (std::size_t k = 0; k < s.queued.size(); k++) {
            CHECK(d[sent + k].message == s.queued[k]);
            CHECK(d[sent + k].cause == s.type);
        }
        sent = d.size();
        CHECK(l.hidden() == s.hidden);
        CHECK(l.presenting() == !s.hidden);
    }
    CHECK(g_messages.size() == sent);
    for (std::size_t k = 0; k < sent; k++) {
        CHECK(g_messages[k] == fe::app_message_selector(l.delivered()[k].message));
    }
    if (sent != 0) {
        CHECK(g_app_arg == g.app); // the message's argument is the UIApplication
    }
}

} // namespace

TEST_CASE("lifecycle: on the desktop a minimise resigns the delegate and hides, a restore "
          "brings both back, once each") {
    run_steps(true,
              {
                  {SDL_EVENT_WINDOW_RESTORED, {}, false}, // not minimised: nothing
                  {SDL_EVENT_WINDOW_MINIMIZED, {kResign}, true},
                  {SDL_EVENT_WINDOW_MINIMIZED, {}, true}, // again: nothing more
                  {SDL_EVENT_WINDOW_RESTORED, {kActive}, false},
                  {SDL_EVENT_WINDOW_RESTORED, {}, false},
                  {SDL_EVENT_WINDOW_MINIMIZED, {kResign}, true},
                  {SDL_EVENT_WINDOW_MAXIMIZED, {kActive}, false}, // restored into maximised
                  {SDL_EVENT_WINDOW_FOCUS_LOST, {}, false},       // focus is FocusMute's
                  {SDL_EVENT_WINDOW_HIDDEN, {}, false},
              },
              "desktop");
}

TEST_CASE("lifecycle: SDL's iOS sequences: the home gesture and Control Center") {
    // SDL 3.4 (SDL_video.c, SDL_uikitappdelegate.m): sceneWillResignActive sends a minimise for
    // every window, then WILL_ENTER_BACKGROUND; sceneDidEnterBackground DID_ENTER_BACKGROUND;
    // sceneWillEnterForeground WILL_ENTER_FOREGROUND; sceneDidBecomeActive
    // DID_ENTER_FOREGROUND, then a restore for every window.
    const std::vector<Step> home_ios = {
        {SDL_EVENT_WINDOW_MINIMIZED, {}, false},
        {SDL_EVENT_WILL_ENTER_BACKGROUND, {kResign}, false},
        {SDL_EVENT_DID_ENTER_BACKGROUND, {}, true},
        {SDL_EVENT_WILL_ENTER_FOREGROUND, {}, false},
        {SDL_EVENT_DID_ENTER_FOREGROUND, {kActive}, false},
        {SDL_EVENT_WINDOW_RESTORED, {}, false},
    };
    run_steps(false, home_ios, "iOS: the home gesture and back");
    // Control Center (and an incoming call's banner): resigned, never in the background.
    run_steps(false,
              {
                  {SDL_EVENT_WINDOW_MINIMIZED, {}, false},
                  {SDL_EVENT_WILL_ENTER_BACKGROUND, {kResign}, false},
                  {SDL_EVENT_DID_ENTER_FOREGROUND, {kActive}, false},
                  {SDL_EVENT_WINDOW_RESTORED, {}, false},
              },
              "iOS: Control Center");
    // Back from the background without sceneWillEnterForeground (DID_ENTER_FOREGROUND alone
    // ends it too), and twice in a row.
    run_steps(false,
              {
                  {SDL_EVENT_WILL_ENTER_BACKGROUND, {kResign}, false},
                  {SDL_EVENT_DID_ENTER_BACKGROUND, {}, true},
                  {SDL_EVENT_DID_ENTER_FOREGROUND, {kActive}, false},
                  {SDL_EVENT_WILL_ENTER_BACKGROUND, {kResign}, false},
                  {SDL_EVENT_DID_ENTER_BACKGROUND, {}, true},
                  {SDL_EVENT_WILL_ENTER_FOREGROUND, {}, false},
                  {SDL_EVENT_DID_ENTER_FOREGROUND, {kActive}, false},
              },
              "iOS: twice");
    // The same sequences on the desktop's mapping (--background on a Mac): the minimise
    // resigns first, the restore reactivates; still one message each.
    run_steps(true,
              {
                  {SDL_EVENT_WINDOW_MINIMIZED, {kResign}, true},
                  {SDL_EVENT_WILL_ENTER_BACKGROUND, {}, true},
                  {SDL_EVENT_DID_ENTER_BACKGROUND, {}, true},
                  {SDL_EVENT_WILL_ENTER_FOREGROUND, {}, true}, // still minimised
                  {SDL_EVENT_DID_ENTER_FOREGROUND, {}, true},
                  {SDL_EVENT_WINDOW_RESTORED, {kActive}, false},
              },
              "desktop: the iOS home gesture");
}

TEST_CASE("lifecycle: a quit sends applicationWillTerminate: once, and nothing after it") {
    run_steps(true,
              {
                  {SDL_EVENT_WINDOW_MINIMIZED, {kResign}, true},
                  {SDL_EVENT_TERMINATING, {kTerminate}, true},
                  {SDL_EVENT_QUIT, {}, true}, // the system's quit after it: once
                  {SDL_EVENT_WINDOW_CLOSE_REQUESTED, {}, true},
                  {SDL_EVENT_WINDOW_RESTORED, {}, false}, // terminated: no become-active
              },
              "terminating");
    for (const std::uint32_t quit : {SDL_EVENT_QUIT, SDL_EVENT_WINDOW_CLOSE_REQUESTED}) {
        fe::Lifecycle l;
        CHECK_FALSE(l.terminating());
        l.handle(event(quit)); // a polled quit counts (not only a scripted one)
        CHECK(l.terminating());
        CHECK(l.pending());
    }
    // A queued message after the terminate is never sent.
    Guest g;
    fe::Lifecycle l;
    l.handle(event(SDL_EVENT_QUIT));
    CHECK(l.deliver(&g.cpu, 0) == 1);
    l.handle(event(SDL_EVENT_WINDOW_MINIMIZED));
    CHECK(l.deliver(&g.cpu, 0) == 0);
    CHECK(g_messages == std::vector<std::string>{"applicationWillTerminate:"});
}

TEST_CASE("lifecycle: a delegate without the methods is told nothing, and the run goes on") {
    Guest g(&k_mute_delegate);
    fe::Lifecycle l;
    l.handle(event(SDL_EVENT_WINDOW_MINIMIZED));
    l.handle(event(SDL_EVENT_WINDOW_RESTORED));
    CHECK(l.deliver(&g.cpu, 0) == 2);
    CHECK(l.delivered().size() == 2);
    CHECK(g_messages.empty());
    // Not sent at all (mt_uikit_lifecycle_event asks respondsToSelector: first).
    CHECK_FALSE(logged("unrecognized selector"));
}

TEST_CASE("lifecycle: the fixed-step mode takes only the script's events, and quits") {
    fe::Lifecycle l;
    l.set_real(false);
    l.handle(event(SDL_EVENT_WINDOW_MINIMIZED)); // the window's own: ignored
    CHECK_FALSE(l.hidden());
    CHECK_FALSE(l.pending());
    CHECK(l.ignored() == 1);
    l.handle(event(SDL_EVENT_DID_ENTER_BACKGROUND)); // polled app events: the watch's
    CHECK(l.ignored() == 1);
    l.handle(event(SDL_EVENT_WINDOW_MINIMIZED), true); // the script's
    CHECK(l.hidden());
    CHECK_FALSE(l.active());
    l.handle(event(SDL_EVENT_WINDOW_RESTORED)); // the window's: ignored
    CHECK(l.hidden());
    CHECK(l.ignored() == 2);
    l.handle(event(SDL_EVENT_WINDOW_RESTORED), true);
    CHECK_FALSE(l.hidden());
    CHECK(l.active());
    l.handle(event(SDL_EVENT_QUIT)); // a quit always counts
    CHECK(l.terminating());
    // A polled app event is never applied (else the watch's and the queue's copy would count
    // twice), in either mode.
    fe::Lifecycle real;
    real.handle(event(SDL_EVENT_WILL_ENTER_BACKGROUND));
    real.handle(event(SDL_EVENT_DID_ENTER_BACKGROUND));
    CHECK(real.active());
    CHECK_FALSE(real.hidden());
    CHECK_FALSE(real.pending());
}

TEST_CASE("lifecycle: --minimise, --background and --quit as scripted events, by time") {
    fe::GameOptions o;
    o.suspends.push_back(fe::Suspend{6, 4, false});
    o.suspends.push_back(fe::Suspend{2, 1.5, true});
    o.quit_at = 12;
    const std::vector<std::pair<double, std::uint32_t>> s = fe::lifecycle_script(o);
    const std::vector<std::pair<double, std::uint32_t>> expected = {
        {2, SDL_EVENT_WINDOW_MINIMIZED},
        {2, SDL_EVENT_WILL_ENTER_BACKGROUND},
        {2, SDL_EVENT_DID_ENTER_BACKGROUND},
        {3.5, SDL_EVENT_WILL_ENTER_FOREGROUND},
        {3.5, SDL_EVENT_DID_ENTER_FOREGROUND},
        {3.5, SDL_EVENT_WINDOW_RESTORED},
        {6, SDL_EVENT_WINDOW_MINIMIZED},
        {10, SDL_EVENT_WINDOW_RESTORED},
        {12, SDL_EVENT_QUIT},
    };
    CHECK(s == expected);
    CHECK(fe::lifecycle_script(fe::GameOptions{}).empty());
}

// --- pushed SDL events into a synthetic guest that presents -----------------------------------

namespace {

enum : u32 {
    GL_SHORT = 0x1402,
    GL_TRIANGLE_STRIP = 0x0005,
    GL_VERTEX_ARRAY = 0x8074,
};
constexpr u32 X(int v) { return static_cast<u32>(v * 65536); }
using Rgb = std::array<u8, 3>;

// The guest of platform_test.cpp's present tests, with the recording delegate: a UIApplication,
// a 320x480 window with a GL view turned to landscape, an EAGL context and renderbuffer. Its
// present hook is game.cpp's in miniature: copy the frame, push what the test asks to arrive
// during this present, pump SDL's queue into the Lifecycle, then upload, draw and present the
// window only while the Lifecycle presents. turn() is the turn hook's part: pump, deliver.
struct Rig : Guest {
    fe::Lifecycle lifecycle;
    fe::Screen screen{1};
    fe::Presenter presenter;
    SDL_Window *sdl_window = nullptr;
    SDL_Renderer *renderer = nullptr;
    u32 context = 0;
    u32 gl_presents = 0;                        // the guest's presentRenderbuffer: calls
    u32 window_presents = 0;                    // the window's
    std::vector<std::uint32_t> push_in_present; // pushed inside the next present hook
    double t = 0;

    explicit Rig(bool minimise_resigns) : lifecycle(minimise_resigns) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        REQUIRE_MESSAGE(
            SDL_CreateWindowAndRenderer("lifecycle_test", 480, 320, 0, &sdl_window, &renderer),
            SDL_GetError());
        REQUIRE(presenter.start(renderer, 480, 320));
        presenter.set_scaling(fe::ScaleMode::kFit, fe::Color{});
        REQUIRE(lifecycle.watch());

        mt_ui::State &st = mt_ui::state(&cpu);
        const u32 window = send(send(klass("UIWindow"), "alloc"),
                                "initWithFrame:", {fb(0), fb(0), fb(320), fb(480)});
        st.windows.push_back(window);
        const u32 view = send(send(klass("GlesTestView"), "alloc"),
                              "initWithFrame:", {fb(0), fb(0), fb(320), fb(480)});
        send(view, "setTransform:", {fb(0), fb(-1), fb(1), fb(0), fb(0), fb(0)});
        send(view, "setBounds:", {fb(0), fb(0), fb(480), fb(320)});
        send(window, "addSubview:", {view});
        mt_gles_config gl{};
        gl.present = [](void *user, mt_cpu *c, const mt_gles_frame *frame) {
            return static_cast<Rig *>(user)->on_present(c, frame);
        };
        gl.user = this;
        gl.render_scale = 1;
        gl.trace_file = "";
        mt_gles_configure(&gl);
        const u32 layer = send(view, "layer");
        const u32 no = send(klass("NSNumber"), "numberWithBool:", {0});
        const u32 props =
            send(klass("NSDictionary"), "dictionaryWithObjectsAndKeys:",
                 {no, string("EAGLDrawablePropertyRetainedBacking"), string("EAGLColorFormatRGBA8"),
                  string("EAGLDrawablePropertyColorFormat"), 0u});
        send(layer, "setDrawableProperties:", {props});
        context = send(send(klass("EAGLContext"), "alloc"), "initWithAPI:", {1u});
        REQUIRE(context != 0);
        REQUIRE(send(klass("EAGLContext"), "setCurrentContext:", {context}) == 1);
        const u32 names = alloc(8);
        call(mt_hle_glGenFramebuffersOES, {1, names});
        call(mt_hle_glGenRenderbuffersOES, {1, names + 4});
        call(mt_hle_glBindFramebufferOES, {GL_FRAMEBUFFER_OES, get32(names)});
        call(mt_hle_glBindRenderbufferOES, {GL_RENDERBUFFER_OES, get32(names + 4)});
        REQUIRE(send(context, "renderbufferStorage:fromDrawable:", {GL_RENDERBUFFER_OES, layer}) ==
                1);
        call(mt_hle_glFramebufferRenderbufferOES,
             {GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, get32(names + 4)});
        call(mt_hle_glViewport, {0, 0, 480, 320});
        call(mt_hle_glMatrixMode, {GL_PROJECTION});
        call(mt_hle_glLoadIdentity, {});
        call(mt_hle_glOrthox, {0, X(480), 0, X(320), static_cast<u32>(X(-1)), X(1)});
        call(mt_hle_glMatrixMode, {GL_MODELVIEW});
        call(mt_hle_glLoadIdentity, {});
        call(mt_hle_glEnableClientState, {GL_VERTEX_ARRAY});
    }
    ~Rig() {
        lifecycle.unwatch();
        mt_gles_configure(nullptr);
        presenter.stop();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
    }
    Rig(const Rig &) = delete;
    Rig &operator=(const Rig &) = delete;

    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            lifecycle.handle(e);
        }
    }
    int on_present(mt_cpu *c, const mt_gles_frame *frame) {
        gl_presents++;
        CHECK(screen.take_gl_frame(*frame));
        for (const std::uint32_t type : push_in_present) {
            push(type);
        }
        push_in_present.clear();
        pump();
        if (lifecycle.presenting()) {
            REQUIRE(screen.compose(c, 1000));
            REQUIRE(presenter.upload(screen.pixels().data(), screen.width() * 4));
            REQUIRE(presenter.draw());
            SDL_RenderPresent(renderer);
            window_presents++;
        }
        return 0;
    }
    void push(std::uint32_t type) {
        SDL_Event e{};
        e.type = type;
        if (type >= SDL_EVENT_WINDOW_FIRST && type <= SDL_EVENT_WINDOW_LAST) {
            e.window.windowID = SDL_GetWindowID(sdl_window);
        }
        REQUIRE(SDL_PushEvent(&e));
    }
    // The guest draws the whole screen in one colour and presents.
    void frame(Rgb c) {
        const std::int16_t v[8] = {0, 0, 480, 0, 0, 320, 480, 320};
        const u32 addr = bytes(v, sizeof v);
        auto x = [](u8 b) { return b == 255 ? X(1) : static_cast<u32>(b) << 8; };
        call(mt_hle_glColor4x, {x(c[0]), x(c[1]), x(c[2]), X(1)});
        call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, addr});
        call(mt_hle_glDrawArrays, {GL_TRIANGLE_STRIP, 0, 4});
        REQUIRE(send(context, "presentRenderbuffer:", {GL_RENDERBUFFER_OES}) == 1);
    }
    // The turn hook's part: pump, then the messages.
    int turn() {
        pump();
        t += 1;
        return lifecycle.deliver(&cpu, t);
    }
    // The window's centre pixel, read back.
    Rgb shown() {
        SDL_Surface *s = SDL_RenderReadPixels(renderer, nullptr);
        REQUIRE(s != nullptr);
        Uint8 r = 0, g = 0, b = 0, a = 0;
        CHECK(SDL_ReadSurfacePixel(s, s->w / 2, s->h / 2, &r, &g, &b, &a));
        SDL_DestroySurface(s);
        return {r, g, b};
    }
};

constexpr Rgb kGreen = {0, 255, 0};
constexpr Rgb kRed = {255, 0, 0};
constexpr Rgb kBlue = {0, 0, 255};

} // namespace

TEST_CASE("lifecycle: pushed SDL app events reach the delegate once each, in order, and "
          "nothing is presented in the background") {
    for (const bool desktop : {false, true}) {
        INFO((desktop ? "the desktop's mapping" : "iOS's mapping"));
        Rig rig(desktop);
        // Active: the frames reach the window.
        rig.frame(kGreen);
        CHECK(rig.turn() == 0);
        CHECK(rig.window_presents == 1);
        CHECK(rig.shown() == kGreen);

        // The home gesture, SDL's events pushed (the app events reach the Lifecycle through
        // its event watch at once; the window event when the queue is pumped) during the
        // guest's present, as UIKit's callbacks run inside the pump of game.cpp's present
        // hook: that frame is not presented any more.
        rig.push_in_present = {SDL_EVENT_WINDOW_MINIMIZED, SDL_EVENT_WILL_ENTER_BACKGROUND,
                               SDL_EVENT_DID_ENTER_BACKGROUND};
        rig.frame(kRed);
        CHECK(rig.lifecycle.hidden());
        CHECK(rig.window_presents == 1);
        CHECK(g_messages.empty()); // queued: the guest is inside its present
        CHECK(rig.turn() == 1);    // the turn hook sends it
        CHECK(g_messages == std::vector<std::string>{"applicationWillResignActive:"});
        // More frames in the background (the guest waits in game.cpp; drawing anyway): none
        // reaches the window, which still shows the last frame before it.
        for (int i = 0; i < 3; i++) {
            rig.frame(kRed);
            CHECK(rig.turn() == 0);
        }
        CHECK(rig.gl_presents == 5);
        CHECK(rig.window_presents == 1);
        CHECK(rig.shown() == kGreen);

        // Back: WILL_ENTER_FOREGROUND allows presenting again; DID_ENTER_FOREGROUND and the
        // restore make the delegate active.
        rig.push(SDL_EVENT_WILL_ENTER_FOREGROUND);
        CHECK(rig.lifecycle.presenting() == !desktop); // the desktop: still minimised
        rig.push(SDL_EVENT_DID_ENTER_FOREGROUND);
        rig.push(SDL_EVENT_WINDOW_RESTORED);
        CHECK(rig.turn() == 1);
        CHECK(rig.lifecycle.presenting());
        rig.frame(kBlue);
        CHECK(rig.window_presents == 2);
        CHECK(rig.shown() == kBlue);

        // Control Center: resigned and active again, presenting all the while.
        rig.push(SDL_EVENT_WINDOW_MINIMIZED);
        rig.push(SDL_EVENT_WILL_ENTER_BACKGROUND);
        rig.frame(kGreen);
        CHECK(rig.turn() == 1);
        rig.push(SDL_EVENT_DID_ENTER_FOREGROUND);
        rig.push(SDL_EVENT_WINDOW_RESTORED);
        rig.frame(kRed);
        CHECK(rig.turn() == 1);
        // iOS presents both frames (only resigned); the desktop's mapping has the minimise
        // hide the window until the restore, which the present's pump sees first.
        CHECK(rig.window_presents == (desktop ? 3u : 4u));
        CHECK(rig.shown() == kRed);

        // Terminated by the system, then SDL's quit: one applicationWillTerminate:, and
        // nothing after it.
        rig.push(SDL_EVENT_TERMINATING);
        rig.push(SDL_EVENT_QUIT);
        CHECK(rig.lifecycle.terminating());
        CHECK(rig.turn() == 1);
        rig.push(SDL_EVENT_WINDOW_MINIMIZED);
        rig.push(SDL_EVENT_WILL_ENTER_BACKGROUND);
        CHECK(rig.turn() == 0);

        CHECK(g_messages == std::vector<std::string>{
                                "applicationWillResignActive:", "applicationDidBecomeActive:",
                                "applicationWillResignActive:", "applicationDidBecomeActive:",
                                "applicationWillTerminate:"});
        CHECK(g_app_arg == rig.app);
        const std::vector<fe::AppMessageRecord> d = rig.lifecycle.delivered();
        REQUIRE(d.size() == 5);
        // The causes. Each resign is WILL_ENTER_BACKGROUND's in both mappings: the watch
        // applies it when it is pushed, before the pump reaches the minimise queued ahead of
        // it. iOS's become-active is DID_ENTER_FOREGROUND's; the desktop's waits for the
        // restore.
        CHECK(d[0].cause == SDL_EVENT_WILL_ENTER_BACKGROUND);
        CHECK(d[1].cause == (desktop ? SDL_EVENT_WINDOW_RESTORED : SDL_EVENT_DID_ENTER_FOREGROUND));
        CHECK(d[2].cause == SDL_EVENT_WILL_ENTER_BACKGROUND);
        CHECK(d[4].cause == SDL_EVENT_TERMINATING);
        CHECK_FALSE(d[0].scripted);
    }
}
