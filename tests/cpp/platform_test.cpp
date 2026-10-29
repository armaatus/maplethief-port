// The frontend's presentation chain and game start-up (#58, ADR 0042) with synthetic data only:
// the fit rectangle at several drawable sizes; the armv7 slice of fat and thin Mach-O headers,
// valid and hostile; the bundle's precedence; and the whole present path on a synthetic guest
// whose turned GL view (like the game's EAGLView) gets a 480x320 renderbuffer from its
// CAEAGLLayer: GL draws with the game's y-up ortho -> -presentRenderbuffer: -> the present hook
// (Screen::take_gl_frame) -> the compositor's GL slot (Screen::compose) -> the SDL streaming
// texture -> the window (SDL's dummy video driver), read back. An asymmetric scene (markers in
// three corners) proves the chain keeps the picture upright and unmirrored; a second frame that
// draws only one more marker proves the contents are retained, nothing cleared between frames.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game.h"
#include "gles/gles_internal.h"
#include "maplethief/gfx.h"
#include "maplethief/gles.h"
#include "maplethief/uikit.h"
#include "present.h"
#include "uikit/uikit_internal.h"

#include "gles_fixture.h"

#include "maplethief/recomp/calls.h"

// The boot (linked through game.cpp) refers to the dispatcher; there is no guest code here.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t target, std::uint32_t) noexcept {
    std::fprintf(stderr, "platform_test: mt_dispatch(0x%X)\n", static_cast<unsigned>(target));
    std::abort();
}

namespace {

namespace fe = maplethief::frontend;

enum : u32 {
    GL_SHORT = 0x1402,
    GL_TRIANGLE_STRIP = 0x0005,
    GL_VERTEX_ARRAY = 0x8074,
};

constexpr u32 X(int v) { return static_cast<u32>(v * 65536); }

using Rgb = std::array<u8, 3>;

// --- fit_rect -------------------------------------------------------------------------------

std::string text(const fe::Rect &r) {
    return std::to_string(r.x) + ", " + std::to_string(r.y) + ", " + std::to_string(r.w) + ", " +
           std::to_string(r.h);
}

void check_rect(const fe::Rect &r, float x, float y, float w, float h) {
    INFO("got (" << text(r) << ")");
    CHECK(r.x == x);
    CHECK(r.y == y);
    CHECK(r.w == w);
    CHECK(r.h == h);
}

} // namespace

TEST_CASE("fit: the screen image keeps its aspect ratio, centred between black bars") {
    // 3:2 outputs: no bars.
    check_rect(fe::fit_rect(480, 320, 480, 320), 0, 0, 480, 320);
    check_rect(fe::fit_rect(960, 640, 480, 320), 0, 0, 960, 640);
    check_rect(fe::fit_rect(1920, 1280, 960, 640), 0, 0, 1920, 1280); // HiDPI 960x640 window
    // Wider than 3:2: bars left and right.
    check_rect(fe::fit_rect(1920, 1080, 480, 320), 150, 0, 1620, 1080);
    check_rect(fe::fit_rect(1280, 800, 480, 320), 40, 0, 1200, 800);
    check_rect(fe::fit_rect(2560, 1600, 480, 320), 80, 0, 2400, 1600); // the same at 2x pixels
    check_rect(fe::fit_rect(1280, 800, 960, 640), 40, 0, 1200, 800);   // render scale 2
    // Taller than 3:2: bars above and below; the height rounds to whole pixels.
    check_rect(fe::fit_rect(800, 800, 480, 320), 0, 133, 800, 533);
    check_rect(fe::fit_rect(720, 640, 480, 320), 0, 80, 720, 480);
    // Smaller than the image: scaled down.
    check_rect(fe::fit_rect(240, 240, 480, 320), 0, 40, 240, 160);
    // Nothing to draw into, or nothing to draw.
    check_rect(fe::fit_rect(0, 640, 480, 320), 0, 0, 0, 0);
    check_rect(fe::fit_rect(960, -1, 480, 320), 0, 0, 0, 0);
    check_rect(fe::fit_rect(960, 640, 0, 320), 0, 0, 0, 0);
    // The seam: today the fit, nearest.
    const fe::Placement p = fe::present_rect(1920, 1080, 480, 320);
    check_rect(p.rect, 150, 0, 1620, 1080);
    CHECK(p.nearest);
}

// --- the slice and the bundle ---------------------------------------------------------------

namespace {

void put_be(std::vector<u8> &f, std::size_t at, u32 v) {
    f[at] = static_cast<u8>(v >> 24);
    f[at + 1] = static_cast<u8>(v >> 16);
    f[at + 2] = static_cast<u8>(v >> 8);
    f[at + 3] = static_cast<u8>(v);
}
void put_le(std::vector<u8> &f, std::size_t at, u32 v) {
    for (int i = 0; i < 4; i++) {
        f[at + static_cast<std::size_t>(i)] = static_cast<u8>(v >> (8 * i));
    }
}
// A 28-byte 32-bit Mach-O header for ARM subtype `sub` at `at`.
void thin_header(std::vector<u8> &f, std::size_t at, u32 sub) {
    put_le(f, at, 0xFEEDFACEu);
    put_le(f, at + 4, 12);
    put_le(f, at + 8, sub);
}
// A fat file like the game's executable: armv6 at 0x1000, armv7 at 0x2000, 0x100 bytes each.
std::vector<u8> fat_file() {
    std::vector<u8> f(0x2100, 0);
    put_be(f, 0, 0xCAFEBABEu);
    put_be(f, 4, 2);
    const u32 subs[2] = {6, 9};
    for (u32 i = 0; i < 2; i++) {
        const std::size_t a = 8 + 20 * i;
        put_be(f, a, 12);
        put_be(f, a + 4, subs[i]);
        put_be(f, a + 8, 0x1000 * (i + 1));
        put_be(f, a + 12, 0x100);
        put_be(f, a + 16, 12);
        thin_header(f, 0x1000 * (i + 1), subs[i]);
    }
    return f;
}

bool slice_of(const std::vector<u8> &f, std::size_t *off, std::size_t *len, std::string *err) {
    return fe::find_armv7_slice(f.data(), f.size(), off, len, err);
}

} // namespace

TEST_CASE("slice: the armv7 slice of a fat file, a thin armv7 file, and hostile headers") {
    std::size_t off = 0, len = 0;
    std::string err;
    std::vector<u8> f = fat_file();
    REQUIRE(slice_of(f, &off, &len, &err));
    CHECK(off == 0x2000);
    CHECK(len == 0x100);
    // The capability bits of the subtype are ignored.
    put_be(f, 8 + 20 + 4, 0x80000009u);
    REQUIRE(slice_of(f, &off, &len, &err));
    CHECK(off == 0x2000);

    // A thin armv7 file is its own slice; armv6 is not one.
    std::vector<u8> thin(64, 0);
    thin_header(thin, 0, 9);
    REQUIRE(slice_of(thin, &off, &len, &err));
    CHECK(off == 0);
    CHECK(len == 64);
    thin_header(thin, 0, 6);
    CHECK_FALSE(slice_of(thin, &off, &len, &err));

    struct Bad {
        const char *what;
        void (*edit)(std::vector<u8> &);
    };
    const Bad bad[] = {
        {"no armv7 entry", [](std::vector<u8> &v) { put_be(v, 8 + 20 + 4, 11); }},
        {"no architectures", [](std::vector<u8> &v) { put_be(v, 4, 0); }},
        {"too many architectures", [](std::vector<u8> &v) { put_be(v, 4, 0x10000); }},
        {"a table past the end", [](std::vector<u8> &v) { v.resize(30); }},
        {"a slice past the end", [](std::vector<u8> &v) { put_be(v, 8 + 20 + 12, 0x101); }},
        {"an offset past the end", [](std::vector<u8> &v) { put_be(v, 8 + 20 + 8, 0x3000); }},
        {"an offset + size that wraps",
         [](std::vector<u8> &v) {
             put_be(v, 8 + 20 + 8, 0xFFFFFFF0u);
             put_be(v, 8 + 20 + 12, 0x20);
         }},
        {"an entry that is no Mach-O", [](std::vector<u8> &v) { put_le(v, 0x2000, 0); }},
        {"an entry of another subtype", [](std::vector<u8> &v) { put_le(v, 0x2008, 6); }},
        {"a slice shorter than a header", [](std::vector<u8> &v) { put_be(v, 8 + 20 + 12, 27); }},
        {"another magic", [](std::vector<u8> &v) { put_be(v, 0, 0xCAFEBABFu); }},
    };
    for (const Bad &b : bad) {
        std::vector<u8> v = fat_file();
        b.edit(v);
        err.clear();
        INFO(b.what);
        CHECK_FALSE(slice_of(v, &off, &len, &err));
        CHECK_FALSE(err.empty());
    }
    CHECK_FALSE(fe::find_armv7_slice(nullptr, 0, &off, &len, &err));
    const u8 tiny[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    CHECK_FALSE(fe::find_armv7_slice(tiny, sizeof tiny, &off, &len, &err));
}

TEST_CASE("slice: the bundle's executable gives the bootstrap's armv7 slice (local, real assets)") {
    const fs::path exe = fs::path(MT_REPO_DIR) / "work/ipa/Payload/MapleThief.app/MapleThief";
    const fs::path armv7 = fs::path(MT_REPO_DIR) / "work/slices/MapleThief.armv7";
    if (!fs::is_regular_file(exe) || !fs::is_regular_file(armv7)) {
        MESSAGE("skipped: no work/ipa or work/slices (scripts/bootstrap)");
        return;
    }
    auto read = [](const fs::path &p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<u8>(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
    };
    const std::vector<u8> fat = read(exe), thin = read(armv7);
    std::size_t off = 0, len = 0;
    std::string err;
    REQUIRE_MESSAGE(slice_of(fat, &off, &len, &err), err);
    REQUIRE(len == thin.size());
    CHECK(std::equal(thin.begin(), thin.end(), fat.begin() + static_cast<std::ptrdiff_t>(off)));
}

TEST_CASE("bundle: --bundle, then MAPLETHIEF_BUNDLE_DIR, then the build's default") {
    std::error_code ec;
    fs::create_directories(test_dir(), ec);
    const std::string dir = test_dir().string();
    const std::string missing = (test_dir() / "no-such-bundle").string();
    std::string why;
    CHECK(fe::choose_bundle_dir(dir, missing.c_str(), missing.c_str(), &why) == dir);
    CHECK(fe::choose_bundle_dir("", dir.c_str(), missing.c_str(), &why) == dir);
    CHECK(fe::choose_bundle_dir("", "", dir.c_str(), &why) == dir);
    CHECK(fe::choose_bundle_dir("", nullptr, dir.c_str(), &why) == dir);
    // The first one given decides, even if it is wrong.
    why.clear();
    CHECK(fe::choose_bundle_dir(missing, dir.c_str(), dir.c_str(), &why).empty());
    CHECK(why.find("--bundle") != std::string::npos);
    why.clear();
    CHECK(fe::choose_bundle_dir("", missing.c_str(), dir.c_str(), &why).empty());
    CHECK(why.find("MAPLETHIEF_BUNDLE_DIR") != std::string::npos);
    why.clear();
    CHECK(fe::choose_bundle_dir("", nullptr, nullptr, &why).empty());
    CHECK(why.find("--bundle") != std::string::npos);
}

// --- the present path -----------------------------------------------------------------------

namespace {

// The fixture's guest as the game sets it up: a UIApplication with a 320x480 portrait window
// whose one subview is a GL view turned to landscape like -[EAGLView createGLes] (transform
// (0, -1, 1, 0), bounds 480x320), an EAGL context, a framebuffer with a colour renderbuffer
// whose storage comes from the view's CAEAGLLayer, the game's viewport and y-up ortho. The
// present hook goes to a frontend Screen at `scale`.
struct Game : Env {
    fe::Screen screen;
    u32 window = 0, view = 0, context = 0;
    u32 presents = 0;

    explicit Game(u32 scale = 1) : screen(scale) {
        mt_ui::State &st = mt_ui::state(&cpu);
        st.app = send(send(klass("UIApplication"), "alloc"), "init");
        window = send(send(klass("UIWindow"), "alloc"),
                      "initWithFrame:", {fb(0), fb(0), fb(320), fb(480)});
        st.windows.push_back(window);
        view = send(send(klass("GlesTestView"), "alloc"),
                    "initWithFrame:", {fb(0), fb(0), fb(320), fb(480)});
        send(view, "setTransform:", {fb(0), fb(-1), fb(1), fb(0), fb(0), fb(0)});
        send(view, "setBounds:", {fb(0), fb(0), fb(480), fb(320)});
        send(window, "addSubview:", {view});

        mt_gles_config gl{};
        gl.present = [](void *user, mt_cpu *, const mt_gles_frame *frame) {
            Game &g = *static_cast<Game *>(user);
            g.presents++;
            CHECK(g.screen.take_gl_frame(*frame));
            return 0;
        };
        gl.user = this;
        gl.render_scale = scale;
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
    // An opaque untextured GL_SHORT rectangle (x0, y0)-(x1, y1) in GL's y-up screen space, in
    // colour c, as the game draws: a 4-vertex strip.
    void rect(int x0, int y0, int x1, int y1, Rgb c) {
        const std::int16_t v[8] = {static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y0),
                                   static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y0),
                                   static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y1),
                                   static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y1)};
        const u32 addr = bytes(v, sizeof v);
        // 8-bit colour as the game passes it: shifted left by 8 (0xFF00 is 254, so 255 is
        // passed as 1.0).
        auto x = [](u8 b) { return b == 255 ? X(1) : static_cast<u32>(b) << 8; };
        call(mt_hle_glColor4x, {x(c[0]), x(c[1]), x(c[2]), X(1)});
        call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, addr});
        call(mt_hle_glDrawArrays, {GL_TRIANGLE_STRIP, 0, 4});
    }
    void present() { REQUIRE(send(context, "presentRenderbuffer:", {GL_RENDERBUFFER_OES}) == 1); }
};

// The game's 8-bit colour c through glColor4x(c << 8): round(c * 256 / 65536 * 255).
u8 gl8(u8 c) { return c == 255 ? 255 : static_cast<u8>((c * 256u * 255u + 32768u) >> 16); }
Rgb gl(Rgb c) { return {gl8(c[0]), gl8(c[1]), gl8(c[2])}; }

constexpr Rgb kGrey = {64, 64, 64};
constexpr Rgb kRed = {255, 0, 0};
constexpr Rgb kGreen = {0, 255, 0};
constexpr Rgb kBlue = {0, 0, 255};
constexpr Rgb kYellow = {255, 255, 0};

// The asymmetric scene, in GL's y-up space: a grey screen, red top left, green top right, blue
// bottom left; 48x32 markers. Upright and unmirrored, the screen shows red at its top left.
void draw_scene(Game &g) {
    g.rect(0, 0, 480, 320, kGrey);
    g.rect(0, 288, 48, 320, kRed);
    g.rect(432, 288, 480, 320, kGreen);
    g.rect(0, 0, 48, 32, kBlue);
}

Rgb at(const std::vector<u8> &px, u32 width, u32 x, u32 y) {
    const u8 *p = px.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    return {p[0], p[1], p[2]};
}

// Where each marker must be in a top-row-first image of the screen at `scale` (screen points
// x scale): the centre of each corner's marker, and the bottom right stays grey.
void check_corners(const std::vector<u8> &px, u32 width, u32 scale, const char *what) {
    INFO(what);
    CHECK(at(px, width, 24 * scale, 16 * scale) == gl(kRed));    // top left
    CHECK(at(px, width, 456 * scale, 16 * scale) == gl(kGreen)); // top right
    CHECK(at(px, width, 24 * scale, 304 * scale) == gl(kBlue));  // bottom left
    CHECK(at(px, width, 456 * scale, 304 * scale) == gl(kGrey)); // bottom right
    CHECK(at(px, width, 240 * scale, 160 * scale) == gl(kGrey)); // middle
    // The markers' edges: red covers exactly the first 48x32 points.
    CHECK(at(px, width, 47 * scale, 0) == gl(kRed));
    CHECK(at(px, width, 48 * scale, 0) == gl(kGrey));
    CHECK(at(px, width, 0, 31 * scale) == gl(kRed));
    CHECK(at(px, width, 0, 32 * scale) == gl(kGrey));
}

// A dummy-driver SDL window (headless) of the given size with a software renderer.
struct Window {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    Window(int w, int h) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("platform_test", w, h, 0, &window, &renderer),
                        SDL_GetError());
    }
    ~Window() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    Rgb pixel(SDL_Surface *s, float x, float y) {
        Uint8 r = 0, g = 0, b = 0, a = 0;
        CHECK(SDL_ReadSurfacePixel(s, static_cast<int>(x), static_cast<int>(y), &r, &g, &b, &a));
        return {r, g, b};
    }
};

} // namespace

TEST_CASE("present: the game's GL frame reaches the window upright and unmirrored") {
    Game g;
    draw_scene(g);
    g.present();
    REQUIRE(g.presents == 1);

    // The renderbuffer is the turned view's 480x320; the GL slot is the whole screen.
    float r[4] = {};
    u32 dw = 0, dh = 0;
    REQUIRE(mt_uikit_gl_rect(r, &dw, &dh) == 1);
    CHECK(r[0] == 0);
    CHECK(r[1] == 0);
    CHECK(r[2] == 480);
    CHECK(r[3] == 320);
    CHECK(dw == 480);
    CHECK(dh == 320);
    REQUIRE(g.screen.gl_width() == 480);
    REQUIRE(g.screen.gl_height() == 320);
    CHECK(g.screen.gl_frame() == 1);
    CHECK_FALSE(logged("not a CAEAGLLayer"));
    CHECK(mt_gles_get_stats().misuses == 0);

    // 1. The present hook's copy of the target (mt_gfx: top row first, clip y = +1 on top).
    check_corners(g.screen.gl_pixels(), 480, 1, "the GL frame");
    // Opaque, as the CAEAGLLayer shows it.
    CHECK(g.screen.gl_pixels()[3] == 255);

    // 2. The compositor with the frame in the GL view's slot: the screen is the frame, pixel
    //    for pixel (the turned view's local space is the upright screen).
    REQUIRE(g.screen.compose(&g.cpu, 1000));
    REQUIRE(g.screen.width() == 480);
    REQUIRE(g.screen.height() == 320);
    check_corners(g.screen.pixels(), 480, 1, "the composited screen");
    CHECK(g.screen.pixels() == g.screen.gl_pixels());

    // 3. The SDL texture in a 720x640 window: the 480x320 screen at 1.5x, 720x480 at y 80, with
    //    black bars above and below.
    Window w(720, 640);
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, g.screen.width(), g.screen.height()));
    REQUIRE(p.upload(g.screen.pixels().data(), g.screen.width() * 4));
    REQUIRE(p.draw());
    check_rect(p.placement().rect, 0, 80, 720, 480);
    std::string report;
    CHECK_MESSAGE(p.check_readback(g.screen.pixels().data(), g.screen.width() * 4, &report) == 0,
                  report);
    SDL_Surface *s = SDL_RenderReadPixels(w.renderer, nullptr);
    REQUIRE(s != nullptr);
    CHECK(s->w == 720);
    CHECK(s->h == 640);
    CHECK(w.pixel(s, 24 * 1.5f, 80 + 16 * 1.5f) == gl(kRed));
    CHECK(w.pixel(s, 456 * 1.5f, 80 + 16 * 1.5f) == gl(kGreen));
    CHECK(w.pixel(s, 24 * 1.5f, 80 + 304 * 1.5f) == gl(kBlue));
    CHECK(w.pixel(s, 456 * 1.5f, 80 + 304 * 1.5f) == gl(kGrey));
    CHECK(w.pixel(s, 360, 40) == Rgb{0, 0, 0});  // the top bar
    CHECK(w.pixel(s, 360, 600) == Rgb{0, 0, 0}); // the bottom bar
    CHECK(w.pixel(s, 0, 79) == Rgb{0, 0, 0});
    CHECK(w.pixel(s, 0, 80) == gl(kRed));
    SDL_DestroySurface(s);

    // The read-back check notices a wrong picture (a mirrored one).
    std::vector<u8> mirrored(g.screen.pixels().size());
    for (u32 y = 0; y < 320; y++) {
        for (u32 x = 0; x < 480; x++) {
            std::memcpy(&mirrored[(y * 480 + x) * 4], &g.screen.pixels()[(y * 480 + 479 - x) * 4],
                        4);
        }
    }
    CHECK(p.check_readback(mirrored.data(), 480 * 4, &report) > 0);

    // A PNG of the read-back.
    const std::string png = (test_dir() / "readback.png").string();
    REQUIRE(p.save_readback(png));
    std::ifstream in(png, std::ios::binary);
    char sig[8] = {};
    in.read(sig, 8);
    CHECK(std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0);
    p.stop();
}

TEST_CASE("present: retained backing: what was drawn stays until it is drawn over") {
    Game g;
    g.rect(0, 0, 480, 320, kGrey);
    g.rect(0, 288, 48, 320, kRed);
    g.present();
    const std::vector<u8> first = g.screen.gl_pixels();
    CHECK(at(first, 480, 456, 16) == gl(kGrey));

    // Frame 2 draws only the green marker; frame 1's pixels are still there.
    g.rect(432, 288, 480, 320, kGreen);
    // Drawn but not presented yet: the copy of frame 1 is unchanged.
    CHECK(g.screen.gl_pixels() == first);
    g.present();
    CHECK(g.screen.gl_frame() == 2);
    const std::vector<u8> &second = g.screen.gl_pixels();
    CHECK(at(second, 480, 24, 16) == gl(kRed));
    CHECK(at(second, 480, 456, 16) == gl(kGreen));
    CHECK(at(second, 480, 240, 160) == gl(kGrey));
    // Every other pixel is as frame 1 left it.
    std::size_t changed = 0;
    for (std::size_t i = 0; i < first.size(); i += 4) {
        changed += std::memcmp(&first[i], &second[i], 4) != 0 ? 1u : 0u;
    }
    CHECK(changed == 48u * 32u);
    // Frame 3 draws nothing: the same picture again.
    g.present();
    CHECK(g.screen.gl_frame() == 3);
    CHECK(g.screen.gl_pixels() == second);
    REQUIRE(g.screen.compose(&g.cpu, 1000));
    CHECK(g.screen.pixels() == second);
    // Not black from frame 1 on.
    CHECK(g.screen.first_lit_gl_frame() == 1);
}

TEST_CASE("present: render scale 2 composites the 960x640 target at 960x640") {
    Game g(2);
    draw_scene(g);
    g.present();
    REQUIRE(g.screen.gl_width() == 960);
    REQUIRE(g.screen.gl_height() == 640);
    check_corners(g.screen.gl_pixels(), 960, 2, "the GL frame at 2x");
    REQUIRE(g.screen.compose(&g.cpu, 1000));
    REQUIRE(g.screen.width() == 960);
    REQUIRE(g.screen.height() == 640);
    check_corners(g.screen.pixels(), 960, 2, "the composited screen at 2x");
    CHECK(g.screen.pixels() == g.screen.gl_pixels());

    // In a 1280x800 window: 1200x800 at x 40.
    Window w(1280, 800);
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 960, 640));
    REQUIRE(p.upload(g.screen.pixels().data(), 960 * 4));
    REQUIRE(p.draw());
    check_rect(p.placement().rect, 40, 0, 1200, 800);
    std::string report;
    CHECK_MESSAGE(p.check_readback(g.screen.pixels().data(), 960 * 4, &report) == 0, report);
}

TEST_CASE("present: UIKit views above the GL view are composited over the GL frame") {
    Game g;
    draw_scene(g);
    g.present();
    // A yellow 40x40 view in the GL view at (100, 50) of the landscape screen, like the main
    // menu's buttons in the EAGLView.
    const u32 button = g.send(g.send(g.klass("UIView"), "alloc"),
                              "initWithFrame:", {fb(100), fb(50), fb(40), fb(40)});
    const u32 yellow =
        g.send(g.klass("UIColor"), "colorWithRed:green:blue:alpha:", {fb(1), fb(1), fb(0), fb(1)});
    g.send(button, "setBackgroundColor:", {yellow});
    g.send(g.view, "addSubview:", {button});
    REQUIRE(g.screen.compose(&g.cpu, 1000));
    check_corners(g.screen.pixels(), 480, 1, "GL under the view");
    CHECK(at(g.screen.pixels(), 480, 120, 70) == kYellow);
    CHECK(at(g.screen.pixels(), 480, 99, 70) == gl(kGrey));
    CHECK(at(g.screen.pixels(), 480, 100, 50) == kYellow);
    CHECK(at(g.screen.pixels(), 480, 140, 70) == gl(kGrey));
}

TEST_CASE("present: no GL frame yet composites black in the GL slot, a black frame is black") {
    Game g;
    REQUIRE(g.screen.compose(&g.cpu, 1000));
    CHECK(g.screen.gl_frame() == 0);
    CHECK(fe::is_black(g.screen.pixels().data(), 480, 320, 480 * 4));
    // The game's first frame: its fade-in, a black quad over the screen.
    g.rect(0, 0, 480, 320, {0, 0, 0});
    g.present();
    CHECK(g.screen.first_lit_gl_frame() == 0);
    draw_scene(g);
    g.present();
    CHECK(g.screen.first_lit_gl_frame() == 2);
    // is_black allows 1% of lit pixels.
    std::vector<u8> img(100 * 4, 0);
    img[0] = 255;
    CHECK(fe::is_black(img.data(), 100, 1, 400));
    img[4] = 255;
    CHECK_FALSE(fe::is_black(img.data(), 100, 1, 400));
}
