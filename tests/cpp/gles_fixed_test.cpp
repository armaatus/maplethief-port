// GL ES 1.1's fixed-function pipeline on mt_gfx (#56, ADR 0040) on a real guest address space
// with synthetic data only, every call through the HLE functions as the game makes it: GLfixed
// conversions, each matrix operation against hand-computed matrices, the stacks' overflow and
// underflow, the client arrays' types, sizes, strides and bounds, the colour's clamping and
// rounding, enables, blend functions, the texture objects' mt_gfx textures, the render targets,
// GL's error flag, the once-per-kind logging and the GL trace file; and three golden images
// (an ortho quad, rotated sprites, blend modes) drawn by GL call sequences, exact on every
// platform (the pipeline and this file are compiled without floating-point contraction).
// The game's texture path (#57, ADR 0041): its loader FUN_00009534 through the CoreGraphics HLE
// (every colour x alpha pair premultiplied as round(c * a / 255), NoneSkipLast's 0xFF, the zero
// padding), its filter sequences (GL_LINEAR; FUN_0000949c's image-less texture draws
// untextured), its sprite draw FUN_00007ccc (1:1 hits texel centres, so the padding never
// bleeds in; magnified, GL_REPEAT wraps it in), and a fourth golden of premultiplied edges under
// GL_SRC_ALPHA blending with hand-computed pixels (gles_premultiplied_edges).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_hle.h"
#include "cg/cg_objects.h"
#include "gfx/gfx_internal.h"
#include "gles/gles_internal.h"
#include "golden.h"
#include "maplethief/gfx.h"
#include "maplethief/gles.h"

#include "gles_fixture.h"

namespace {

namespace golden = maplethief::golden;

// GL enums of these tests (the fixture has the tracker's).
enum : u32 {
    GL_NO_ERROR = 0,
    GL_INVALID_ENUM = 0x0500,
    GL_INVALID_VALUE = 0x0501,
    GL_INVALID_OPERATION = 0x0502,
    GL_STACK_OVERFLOW = 0x0503,
    GL_STACK_UNDERFLOW = 0x0504,
    GL_INVALID_FRAMEBUFFER_OPERATION_OES = 0x0506,
    GL_TEXTURE = 0x1702,
    GL_BYTE = 0x1400,
    GL_SHORT = 0x1402,
    GL_FLOAT = 0x1406,
    GL_FIXED = 0x140C,
    GL_LUMINANCE = 0x1909,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033,
    GL_LINES = 0x0001,
    GL_TRIANGLE_STRIP = 0x0005,
    GL_TRIANGLE_FAN = 0x0006,
    GL_VERTEX_ARRAY = 0x8074,
    GL_COLOR_ARRAY = 0x8076,
    GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_BLEND = 0x0BE2,
    GL_ALPHA_TEST = 0x0BC0,
    GL_CULL_FACE = 0x0B44,
    GL_DITHER = 0x0BD0,
    GL_ZERO = 0x0000,
    GL_ONE = 0x0001,
    GL_SRC_COLOR = 0x0300,
    GL_ONE_MINUS_SRC_COLOR = 0x0301,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_DST_COLOR = 0x0306,
    GL_ONE_MINUS_DST_COLOR = 0x0307,
    GL_SRC_ALPHA_SATURATE = 0x0308,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_REPEAT = 0x2901,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_GENERATE_MIPMAP = 0x8191,
};

// GLfixed of an integer, of a double (exact for the multiples of 1/65536 used here).
constexpr u32 X(int v) { return static_cast<u32>(v * 65536); }
u32 XD(double v) { return static_cast<u32>(static_cast<std::int32_t>(std::lround(v * 65536.0))); }

// The pipeline's state of the current process.
mt_gles::Fixed &fixed() { return mt_gles::state().fixed; }
const float *top(u32 mode) {
    mt_gles::State &s = mt_gles::state();
    const u32 *d = s.depth;
    return mode == GL_PROJECTION ? s.fixed.projection[d[1] - 1].m
           : mode == GL_TEXTURE  ? s.fixed.texture[d[2] - 1].m
                                 : s.fixed.modelview[d[0] - 1].m;
}
void check_matrix(const float *m, std::initializer_list<float> column_major) {
    int i = 0;
    for (float v : column_major) {
        INFO("element " << i);
        CHECK(m[i] == v);
        i++;
    }
}
void check_matrix_near(const float *m, std::initializer_list<double> column_major) {
    int i = 0;
    for (double v : column_major) {
        INFO("element " << i);
        CHECK(m[i] == doctest::Approx(v).epsilon(1e-6));
        i++;
    }
}

golden::Image to_image(const mt_gfx_target *t) {
    golden::Image img(static_cast<int>(mt_gfx_target_pixel_width(t)),
                      static_cast<int>(mt_gfx_target_pixel_height(t)));
    std::memcpy(img.bytes().data(), mt_gfx_target_cpixels(t), img.bytes().size());
    return img;
}
// Image pixel (x, y), top row first.
golden::Rgba px(const mt_gfx_target *t, u32 x, u32 y) {
    const std::uint8_t *p = mt_gfx_target_cpixels(t) +
                            (static_cast<std::size_t>(y) * mt_gfx_target_pixel_width(t) + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

// The fixture's guest with GL set up as the game does it (-[EAGLView createGLes], then a
// 480x320 viewport, an ortho projection with y up and an identity modelview), and helpers that
// put client arrays into guest memory.
struct Gl : Env {
    explicit Gl(bool setup = true, float w = 480, float h = 320) {
        if (!setup) {
            return;
        }
        context = create_gl("EAGLColorFormatRGBA8", nullptr, w, h);
        call(mt_hle_glViewport, {0, 0, static_cast<u32>(w), static_cast<u32>(h)});
        call(mt_hle_glMatrixMode, {GL_PROJECTION});
        call(mt_hle_glLoadIdentity, {});
        call(mt_hle_glOrthox,
             {0, X(static_cast<int>(w)), 0, X(static_cast<int>(h)), static_cast<u32>(X(-1)), X(1)});
        call(mt_hle_glMatrixMode, {GL_MODELVIEW});
        call(mt_hle_glLoadIdentity, {});
        call(mt_hle_glEnableClientState, {GL_VERTEX_ARRAY});
    }
    u32 context = 0;

    const mt_gfx_target *target() { return mt_gles::draw_target(mt_gles::state()); }

    template <typename T> u32 array(std::initializer_list<T> values) {
        const std::vector<T> v(values);
        return bytes(v.data(), static_cast<u32>(v.size() * sizeof(T)));
    }
    // A GL_SHORT rectangle (x0, y0)-(x1, y1) as a strip, with GL_FIXED texture coordinates
    // (s0, t0)-(s1, t1), drawn with the current state, the texture coordinate array enabled
    // around the draw as the game does it. Its arrays stay valid.
    void rect(int x0, int y0, int x1, int y1, double s0 = 0, double t0 = 0, double s1 = 1,
              double t1 = 1) {
        const u32 v =
            array<std::int16_t>({static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y0),
                                 static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y0),
                                 static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y1),
                                 static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y1)});
        const u32 t = array<u32>({XD(s0), XD(t0), XD(s1), XD(t0), XD(s0), XD(t1), XD(s1), XD(t1)});
        call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, v});
        call(mt_hle_glTexCoordPointer, {2, GL_FIXED, 0, t});
        call(mt_hle_glEnableClientState, {GL_TEXTURE_COORD_ARRAY});
        call(mt_hle_glDrawArrays, {GL_TRIANGLE_STRIP, 0, 4});
        call(mt_hle_glDisableClientState, {GL_TEXTURE_COORD_ARRAY});
    }
    void color(double r, double g, double b, double a) {
        call(mt_hle_glColor4x, {XD(r), XD(g), XD(b), XD(a)});
    }
    // A texture object with an RGBA8 image (rows bottom up, t = 0 first) and filters.
    u32 texture(u32 w, u32 h, const std::vector<std::uint8_t> &rgba, u32 filter = GL_NEAREST) {
        const u32 names = alloc(4);
        call(mt_hle_glGenTextures, {1, names});
        const u32 name = get32(names);
        call(mt_hle_glBindTexture, {GL_TEXTURE_2D, name});
        call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter});
        call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter});
        const u32 pixels = bytes(rgba.data(), static_cast<u32>(rgba.size()));
        call(mt_hle_glTexImage2D,
             {GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels});
        return name;
    }
};

// --- conversions ----------------------------------------------------------------------------

TEST_CASE("gles fixed: GLfixed is value / 65536, rounded once to float") {
    CHECK(mt_gles::fixed_to_float(X(1)) == 1.0f);
    CHECK(mt_gles::fixed_to_float(static_cast<u32>(X(-3))) == -3.0f);
    CHECK(mt_gles::fixed_to_float(0x8000u) == 0.5f);
    CHECK(mt_gles::fixed_to_float(1u) == 1.0f / 65536.0f);
    CHECK(mt_gles::fixed_to_float(0xFFFFFFFFu) == -1.0f / 65536.0f);
    CHECK(mt_gles::fixed_to_float(0x80000000u) == -32768.0f);
    CHECK(mt_gles::fixed_to_float(0x7FFFFFFFu) == 32768.0f); // 32767.99998 rounds to 32768
    CHECK(mt_gles::fixed_to_float(X(480)) == 480.0f);
    CHECK(mt_gles::fixed_to_float(0x01E08000u) == 480.5f);
}

TEST_CASE("gles fixed: glColor4x clamps to [0, 1] and rounds v * 255 to 8 bits") {
    Gl e(false);
    e.call(mt_hle_glColor4x, {static_cast<u32>(X(-1)), 0x8000u, 0xFF00u, X(2)});
    CHECK(fixed().color[0] == 0);
    CHECK(fixed().color[1] == 128); // 127.5 rounds up
    CHECK(fixed().color[2] == 254); // 0xFF00 (the game's 255 << 8) is 254.004
    CHECK(fixed().color[3] == 255);
    for (u32 v = 0; v <= 0x10000u; v += 0x101u) {
        e.call(mt_hle_glColor4x, {v, v, v, v});
        const auto expected = static_cast<std::uint8_t>(std::floor(v * 255.0 / 65536.0 + 0.5));
        REQUIRE(fixed().color[0] == expected);
    }
    e.call(mt_hle_glColor4x, {X(1), X(1), X(1), X(1)});
    CHECK(fixed().color[3] == 255);
    CHECK(mt_gles_get_stats().misuses == 0);
}

// --- matrices -------------------------------------------------------------------------------

TEST_CASE("gles fixed: glOrthox, glTranslatex and glScalef against hand-computed matrices") {
    Gl e(false);
    // the game's projection: (0, 480, 0, 320, -1, 1)
    e.call(mt_hle_glMatrixMode, {GL_PROJECTION});
    e.call(mt_hle_glOrthox, {0, X(480), 0, X(320), static_cast<u32>(X(-1)), X(1)});
    check_matrix(top(GL_PROJECTION), {static_cast<float>(2.0 / 480), 0, 0, 0, //
                                      0, static_cast<float>(2.0 / 320), 0, 0, //
                                      0, 0, -1, 0,                            //
                                      -1, -1, -0.0f, 1});
    // y down, a depth range of (0, 10)
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glOrthox, {0, X(480), X(320), 0, 0, X(10)});
    check_matrix(top(GL_PROJECTION), {static_cast<float>(2.0 / 480), 0, 0, 0,  //
                                      0, static_cast<float>(-2.0 / 320), 0, 0, //
                                      0, 0, -0.2f, 0,                          //
                                      -1, 1, -1, 1});
    // degenerate volumes are GL_INVALID_VALUE and change nothing
    e.call(mt_hle_glOrthox, {X(5), X(5), 0, X(1), 0, X(1)});
    CHECK(mt_gles_get_error() == GL_INVALID_VALUE);
    CHECK(top(GL_PROJECTION)[10] == -0.2f);

    // the modelview stack is separate; translate, then scale: M = T x S
    e.call(mt_hle_glMatrixMode, {GL_MODELVIEW});
    check_matrix(top(GL_MODELVIEW), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    e.call(mt_hle_glTranslatex, {X(10), static_cast<u32>(X(-20)), 0x8000u});
    check_matrix(top(GL_MODELVIEW), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, -20, 0.5f, 1});
    e.call(mt_hle_glScalef, {fb(2.0f), fb(-0.5f), fb(3.0f)});
    check_matrix(top(GL_MODELVIEW), {2, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 3, 0, 10, -20, 0.5f, 1});
    // a second translate is scaled by the first scale (it multiplies on the right)
    e.call(mt_hle_glTranslatex, {X(1), X(2), X(1)});
    check_matrix(top(GL_MODELVIEW), {2, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 3, 0, 12, -21, 3.5f, 1});
    // projection x modelview of the game's ortho, as a draw computes it
    const float o = static_cast<float>(2.0 / 480);
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glMatrixMode, {GL_PROJECTION});
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glOrthox, {0, X(480), 0, X(320), static_cast<u32>(X(-1)), X(1)});
    e.call(mt_hle_glOrthox, {0, X(1), 0, X(1), static_cast<u32>(X(-1)), X(1)}); // [0,1] -> [-1,1]
    check_matrix(top(GL_PROJECTION),
                 {o * 2, 0, 0, 0, 0, static_cast<float>(2.0 / 320) * 2, 0, 0, 0, 0, 1, 0,
                  o * -1 + -1, static_cast<float>(2.0 / 320) * -1 + -1, 0, 1});
    CHECK(mt_gles_get_stats().misuses == 1);
}

TEST_CASE("gles fixed: glRotatex in degrees about any axis, exact at right angles") {
    Gl e(false);
    // 90 degrees about z: x -> y, y -> -x
    e.call(mt_hle_glRotatex, {X(90), 0, 0, X(1)});
    check_matrix(top(GL_MODELVIEW), {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    // another 90 about z: 180 degrees; then 180 about an unnormalized z (0, 0, 7): identity
    e.call(mt_hle_glRotatex, {X(90), 0, 0, X(1)});
    check_matrix(top(GL_MODELVIEW), {-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    e.call(mt_hle_glRotatex, {X(180), 0, 0, X(7)});
    check_matrix(top(GL_MODELVIEW), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    // angles wrap: 450 and -270 are 90; -90 about x: y -> -z, z -> y
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glRotatex, {X(450), 0, 0, X(1)});
    check_matrix(top(GL_MODELVIEW), {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glRotatex, {static_cast<u32>(X(-270)), 0, 0, X(1)});
    check_matrix(top(GL_MODELVIEW), {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glRotatex, {static_cast<u32>(X(-90)), X(1), 0, 0});
    check_matrix(top(GL_MODELVIEW), {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    // 30 degrees about z: cos 30, sin 30
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glRotatex, {X(30), 0, 0, X(1)});
    const double c30 = std::sqrt(3.0) / 2;
    check_matrix_near(top(GL_MODELVIEW), {c30, 0.5, 0, 0, -0.5, c30, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    CHECK(top(GL_MODELVIEW)[1] == 0.5f);
    // 120 degrees about (1, 1, 1): x -> y -> z -> x
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glRotatex, {X(120), X(1), X(1), X(1)});
    check_matrix_near(top(GL_MODELVIEW), {0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1});
    // 45 degrees about y
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glRotatex, {X(45), 0, X(3), 0});
    const double h = std::sqrt(0.5);
    check_matrix_near(top(GL_MODELVIEW), {h, 0, -h, 0, 0, 1, 0, 0, h, 0, h, 0, 0, 0, 0, 1});
    // the zero axis: undefined in GL ES, left as it is (logged once)
    e.call(mt_hle_glRotatex, {X(45), 0, 0, 0});
    e.call(mt_hle_glRotatex, {X(45), 0, 0, 0});
    check_matrix_near(top(GL_MODELVIEW), {h, 0, -h, 0, 0, 1, 0, 0, h, 0, h, 0, 0, 0, 0, 1});
    std::size_t notes = 0;
    for (const auto &l : g_log) {
        notes += l.find("glRotatex with the axis (0, 0, 0)") != std::string::npos ? 1u : 0u;
    }
    CHECK(notes == 1);
    CHECK(mt_gles_get_stats().misuses == 0);
}

TEST_CASE("gles fixed: the sine and cosine of glRotatex are exact at right angles, near libm") {
    for (int k = -8; k <= 8; k++) {
        double s = 2, c = 2;
        mt_gles::sincos_fixed_degrees(X(90 * k), &s, &c);
        const int q = ((k % 4) + 4) % 4;
        CHECK(s == (q == 1 ? 1.0 : q == 3 ? -1.0 : 0.0));
        CHECK(c == (q == 0 ? 1.0 : q == 2 ? -1.0 : 0.0));
    }
    double worst = 0;
    for (std::int64_t a = -720LL * 65536; a <= 720LL * 65536; a += 24331) {
        double s = 0, c = 0;
        mt_gles::sincos_fixed_degrees(static_cast<u32>(static_cast<std::int32_t>(a)), &s, &c);
        const double rad = static_cast<double>(a) / 65536.0 * 3.14159265358979323846 / 180.0;
        worst =
            std::max(worst, std::max(std::fabs(s - std::sin(rad)), std::fabs(c - std::cos(rad))));
    }
    CHECK(worst < 1e-14);
}

TEST_CASE("gles fixed: matrix stacks push copies, pop restores, over- and underflow") {
    Gl e(false);
    e.call(mt_hle_glTranslatex, {X(5), 0, 0});
    e.call(mt_hle_glPushMatrix, {});
    CHECK(top(GL_MODELVIEW)[12] == 5.0f); // a copy of the top
    e.call(mt_hle_glTranslatex, {X(5), 0, 0});
    CHECK(top(GL_MODELVIEW)[12] == 10.0f);
    e.call(mt_hle_glPopMatrix, {});
    CHECK(top(GL_MODELVIEW)[12] == 5.0f);
    // each mode has its own stack: 16 deep for the modelview, 2 for projection and texture
    for (int i = 0; i < 15; i++) {
        e.call(mt_hle_glPushMatrix, {});
    }
    CHECK(mt_gles_get_error() == GL_NO_ERROR);
    e.call(mt_hle_glTranslatex, {X(1), 0, 0});
    e.call(mt_hle_glPushMatrix, {}); // the 17th
    CHECK(mt_gles_get_error() == GL_STACK_OVERFLOW);
    CHECK(top(GL_MODELVIEW)[12] == 6.0f);
    for (int i = 0; i < 15; i++) {
        e.call(mt_hle_glPopMatrix, {});
    }
    CHECK(top(GL_MODELVIEW)[12] == 5.0f);
    e.call(mt_hle_glPopMatrix, {});
    CHECK(mt_gles_get_error() == GL_STACK_UNDERFLOW);
    for (u32 mode : {static_cast<u32>(GL_PROJECTION), static_cast<u32>(GL_TEXTURE)}) {
        e.call(mt_hle_glMatrixMode, {mode});
        e.call(mt_hle_glScalef, {fb(3.0f), fb(1.0f), fb(1.0f)});
        e.call(mt_hle_glPushMatrix, {});
        e.call(mt_hle_glLoadIdentity, {});
        e.call(mt_hle_glPushMatrix, {});
        CHECK(mt_gles_get_error() == GL_STACK_OVERFLOW);
        e.call(mt_hle_glPopMatrix, {});
        CHECK(top(mode)[0] == 3.0f);
        e.call(mt_hle_glPopMatrix, {});
        CHECK(mt_gles_get_error() == GL_STACK_UNDERFLOW);
    }
    CHECK(top(GL_MODELVIEW)[12] == 5.0f);
    CHECK(mt_gles_get_stats().misuses == 6);
}

// --- client arrays and draws ----------------------------------------------------------------

TEST_CASE("gles fixed: client arrays of every type, size and stride, read at glDrawArrays") {
    Gl e;
    // identity projection and modelview: the clip coordinates are the array's values
    e.call(mt_hle_glMatrixMode, {GL_PROJECTION});
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glMatrixMode, {GL_MODELVIEW});
    const auto check_vertex = [](u32 i, float x, float y, float z, float w) {
        const mt_gfx_vertex &v = fixed().vertices.at(i);
        INFO("vertex " << i);
        CHECK(v.x == x);
        CHECK(v.y == y);
        CHECK(v.z == z);
        CHECK(v.w == w);
    };
    // GL_BYTE, size 3, stride 4 (one pad byte), from element 1
    const u32 bytes3 = e.array<std::int8_t>({9, 9, 9, 0, 1, -2, 3, 0, -128, 127, 0, 0, 4, 5, 6, 0});
    e.call(mt_hle_glVertexPointer, {3, GL_BYTE, 4, bytes3});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 1, 3});
    REQUIRE(fixed().vertices.size() == 3);
    check_vertex(0, 1, -2, 3, 1);
    check_vertex(1, -128, 127, 0, 1);
    check_vertex(2, 4, 5, 6, 1);
    // GL_SHORT, size 2, packed (stride 0)
    const u32 shorts = e.array<std::int16_t>({-300, 400, 32767, -32768, 0, 1});
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, shorts});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    check_vertex(0, -300, 400, 0, 1);
    check_vertex(1, 32767, -32768, 0, 1);
    check_vertex(2, 0, 1, 0, 1);
    // GL_FIXED, size 4, interleaved with texture coordinates: stride 24
    const u32 inter = e.array<u32>({X(1), 0x8000u, X(-2), X(2), XD(0.25), XD(0.75), //
                                    X(3), X(4), X(5), X(1), XD(1.5), XD(-1),        //
                                    0xFFFFFFFFu, 0, 0, X(1), 0, X(1)});
    e.call(mt_hle_glVertexPointer, {4, GL_FIXED, 24, inter});
    e.call(mt_hle_glTexCoordPointer, {2, GL_FIXED, 24, inter + 16});
    e.call(mt_hle_glEnableClientState, {GL_TEXTURE_COORD_ARRAY});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    check_vertex(0, 1, 0.5f, -2, 2);
    check_vertex(1, 3, 4, 5, 1);
    check_vertex(2, -1.0f / 65536, 0, 0, 1);
    CHECK(fixed().vertices[0].s == 0.25f);
    CHECK(fixed().vertices[0].t == 0.75f);
    CHECK(fixed().vertices[1].s == 1.5f);
    CHECK(fixed().vertices[1].t == -1.0f);
    // GL_FLOAT, size 2, texture coordinates of size 4 divided by q, through the texture matrix
    const u32 floats = e.array<float>({0.5f, -0.25f, 1e3f, 2.0f, -1.0f, 0.0f});
    const u32 tex4 = e.array<float>({1, 2, 0, 2, 3, 3, 0, 1, 0, 0, 0, 1});
    e.call(mt_hle_glVertexPointer, {2, GL_FLOAT, 0, floats});
    e.call(mt_hle_glTexCoordPointer, {4, GL_FLOAT, 0, tex4});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    check_vertex(0, 0.5f, -0.25f, 0, 1);
    check_vertex(1, 1e3f, 2, 0, 1);
    CHECK(fixed().vertices[0].s == 0.5f);
    CHECK(fixed().vertices[0].t == 1.0f);
    CHECK(fixed().vertices[1].s == 3.0f);
    e.call(mt_hle_glMatrixMode, {GL_TEXTURE});
    e.call(mt_hle_glTranslatex, {X(1), 0, 0});
    e.call(mt_hle_glScalef, {fb(0.5f), fb(2.0f), fb(1.0f)});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    // (s, t, r, q) = (1, 2, 0, 2): T x = (0.5 + 2, 4, 0, 2) -> (1.25, 2)
    CHECK(fixed().vertices[0].s == 1.25f);
    CHECK(fixed().vertices[0].t == 2.0f);
    CHECK(fixed().vertices[2].s == 1.0f); // (0, 0, 0, 1) -> (1, 0)
    // without a texture coordinate array: the current coordinates (0, 0, 0, 1), transformed
    e.call(mt_hle_glDisableClientState, {GL_TEXTURE_COORD_ARRAY});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    CHECK(fixed().vertices[1].s == 1.0f);
    CHECK(fixed().vertices[1].t == 0.0f);
    // through projection x modelview: the game's ortho maps (240, 160) to the centre
    e.call(mt_hle_glMatrixMode, {GL_PROJECTION});
    e.call(mt_hle_glOrthox, {0, X(480), 0, X(320), static_cast<u32>(X(-1)), X(1)});
    e.call(mt_hle_glMatrixMode, {GL_MODELVIEW});
    e.call(mt_hle_glTranslatex, {X(200), X(100), 0});
    const u32 pts = e.array<std::int16_t>({40, 60, -200, -100, 280, 220});
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, pts});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    const auto near = [](u32 i, float x, float y) {
        const mt_gfx_vertex &v = fixed().vertices.at(i);
        INFO("vertex " << i);
        CHECK(v.x == doctest::Approx(x).epsilon(1e-6));
        CHECK(v.y == doctest::Approx(y).epsilon(1e-6));
        CHECK(v.w == 1.0f);
    };
    near(0, 0, 0);
    near(1, -1, -1);
    near(2, 1, 1);
    const mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.misuses == 0);
    CHECK(st.draws_drawn == 7);
    CHECK(st.vertices == 21);
}

TEST_CASE("gles fixed: bad pointer arguments and arrays outside guest memory are misuse") {
    Gl e;
    const u32 ok = e.array<std::int16_t>({0, 0, 10, 0, 0, 10});
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, ok});
    e.call(mt_hle_glVertexPointer, {1, GL_SHORT, 0, 0x5000});
    CHECK(mt_gles_get_error() == GL_INVALID_VALUE);
    e.call(mt_hle_glVertexPointer, {5, GL_SHORT, 0, 0x5000});
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, static_cast<u32>(-4), 0x5000});
    e.call(mt_hle_glTexCoordPointer, {2, GL_UNSIGNED_BYTE, 0, 0x5000});
    CHECK(mt_gles_get_error() == GL_INVALID_VALUE); // the first error sticks
    CHECK(fixed().vertex.pointer == ok);
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    CHECK(mt_gles_get_stats().draws_drawn == 1);
    const u32 misuses = mt_gles_get_stats().misuses;
    // a null pointer, page zero, an unmapped image address, beyond the heap's top, past the end
    // of guest memory: logged, the draw skipped, no GL error (the device would crash or draw
    // garbage), never a fault
    for (u32 p : {0u, 0x10u, 0x00800000u, 0x1E000000u, 0xFFFFFFF0u}) {
        e.call(mt_hle_glVertexPointer, {2, GL_FLOAT, 0, p});
        e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    }
    // a range that starts on the heap and runs past its top
    const u32 near_top = e.alloc(16);
    e.call(mt_hle_glVertexPointer, {2, GL_FLOAT, 0x01000000, near_top});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    // the texture coordinate array is checked as well
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, ok});
    e.call(mt_hle_glTexCoordPointer, {2, GL_FIXED, 0, 0x20u});
    e.call(mt_hle_glEnableClientState, {GL_TEXTURE_COORD_ARRAY});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    const mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.misuses == misuses + 7);
    CHECK(st.draws_skipped == 7);
    CHECK(st.draws_drawn == 1);
    CHECK(st.error == GL_NO_ERROR);
    CHECK(logged("gl: glDrawArrays: the vertex array is outside mapped guest memory: pointer "
                 "0x00000000"));
    CHECK(logged("gl: glDrawArrays: the texture coordinate array is outside mapped guest memory"));
}

TEST_CASE("gles fixed: draws without an array, a count or a framebuffer draw nothing") {
    Gl e;
    const u32 ok = e.array<std::int16_t>({0, 0, 10, 0, 0, 10});
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, ok});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 0}); // count 0
    e.call(mt_hle_glDisableClientState, {GL_VERTEX_ARRAY});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3}); // no vertex array: no error
    e.call(mt_hle_glEnableClientState, {GL_VERTEX_ARRAY});
    CHECK(mt_gles_get_stats().misuses == 0);
    e.call(mt_hle_glDrawArrays, {7, 0, 3}); // no such mode
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, static_cast<u32>(-1), 3});
    CHECK(mt_gles_get_error() == GL_INVALID_VALUE);
    e.call(mt_hle_glDrawArrays, {GL_LINES, 0, 2}); // counted by mt_gfx, not drawn
    // no framebuffer bound: GL_INVALID_FRAMEBUFFER_OPERATION_OES
    e.call(mt_hle_glBindFramebufferOES, {GL_FRAMEBUFFER_OES, 0});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 3});
    CHECK(mt_gles_get_error() == GL_INVALID_FRAMEBUFFER_OPERATION_OES);
    CHECK(logged("gl: glDrawArrays: the bound framebuffer is not complete"));
    const mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.draws_drawn == 0);
    CHECK(st.draws_skipped == 4);
    CHECK(st.misuses == 3);
}

// --- enables and blending -------------------------------------------------------------------

TEST_CASE("gles fixed: enables, client states and blend functions as GL ES 1.1 takes them") {
    Gl e;
    CHECK_FALSE(fixed().texture_2d);
    CHECK_FALSE(fixed().blend);
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.call(mt_hle_glEnable, {GL_BLEND});
    e.call(mt_hle_glEnable, {GL_ALPHA_TEST}); // kept: with no glAlphaFunc it passes everything
    e.call(mt_hle_glDisable, {GL_DITHER});
    CHECK(fixed().texture_2d);
    CHECK(fixed().blend);
    CHECK(fixed().caps.at(GL_ALPHA_TEST));
    e.call(mt_hle_glDisable, {GL_BLEND});
    CHECK_FALSE(fixed().blend);
    CHECK(mt_gles_get_stats().misuses == 0);
    e.call(mt_hle_glEnable, {0x1234});
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    CHECK(logged("gl: glEnable: unknown capability: 0x1234"));
    e.call(mt_hle_glEnableClientState, {0x8077});
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    // GL ES 1.1's factor lists: SRC_COLOR only as the destination, DST_COLOR and
    // SRC_ALPHA_SATURATE only as the source
    e.call(mt_hle_glBlendFunc, {GL_SRC_ALPHA_SATURATE, GL_ONE_MINUS_SRC_COLOR});
    CHECK(fixed().src_factor == GL_SRC_ALPHA_SATURATE);
    CHECK(fixed().dst_factor == GL_ONE_MINUS_SRC_COLOR);
    e.call(mt_hle_glBlendFunc, {GL_SRC_COLOR, GL_ZERO});
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    e.call(mt_hle_glBlendFunc, {GL_ONE, GL_DST_COLOR});
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    CHECK(fixed().src_factor == GL_SRC_ALPHA_SATURATE);
    e.call(mt_hle_glBlendFunc, {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    CHECK(fixed().src_factor == GL_SRC_ALPHA);
    // a draw with an enable that would change pixels and is not emulated is logged once
    e.call(mt_hle_glEnable, {GL_CULL_FACE});
    e.call(mt_hle_glEnableClientState, {GL_COLOR_ARRAY});
    e.rect(0, 0, 4, 4);
    e.rect(0, 0, 4, 4);
    std::size_t cull = 0, arrays = 0;
    for (const auto &l : g_log) {
        cull += l.find("cap 0x0B44 enabled: not emulated") != std::string::npos ? 1u : 0u;
        arrays += l.find("a colour, normal or point-size array") != std::string::npos ? 1u : 0u;
    }
    CHECK(cull == 1);
    CHECK(arrays == 1);
    CHECK(mt_gles_get_stats().draws_drawn == 2);
}

// --- textures -------------------------------------------------------------------------------

TEST_CASE("gles fixed: texture objects have mt_gfx textures from the first bind to the delete") {
    Gl e;
    const u32 textures_before = mt_gfx_get_stats().textures;
    const u32 names = e.alloc(8);
    e.call(mt_hle_glGenTextures, {2, names});
    const u32 a = e.get32(names), b = e.get32(names + 4);
    CHECK(mt_gfx_get_stats().textures == textures_before); // names only
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, a});
    const u32 handle = mt_gles::state().textures.at(a).gfx;
    CHECK(handle != 0);
    CHECK(mt_gfx_get_stats().textures == textures_before + 1);
    // level 0 into the bound texture; filters and wraps onto mt_gfx's texture
    const std::vector<std::uint8_t> texels = {1, 2,  3,  4,  5,  6,  7,  8,
                                              9, 10, 11, 12, 13, 14, 15, 16};
    e.call(mt_hle_glTexImage2D, {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 e.bytes(texels.data(), 16)});
    u32 w = 0, h = 0;
    CHECK(mt_gfx_texture_size(handle, &w, &h) == 0);
    CHECK(w == 2);
    CHECK(h == 2);
    CHECK(std::memcmp(mt_gfx_texture_texels(handle, nullptr, nullptr), texels.data(), 16) == 0);
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE});
    const mt_gfx::Texture *t = mt_gfx::find_texture(handle);
    REQUIRE(t != nullptr);
    CHECK(t->min_filter == GL_LINEAR);
    CHECK(t->mag_filter == GL_NEAREST);
    CHECK(t->wrap_s == GL_CLAMP_TO_EDGE);
    CHECK(t->wrap_t == GL_REPEAT);
    CHECK(mt_gles_get_stats().misuses == 0);
    // invalid parameters and values are GL_INVALID_ENUM and change nothing
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, 0x1234, GL_LINEAR});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_LINEAR});
    e.call(mt_hle_glTexParameteri, {0x1234, GL_TEXTURE_MAG_FILTER, GL_LINEAR});
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    CHECK(t->mag_filter == GL_NEAREST);
    CHECK(t->wrap_t == GL_REPEAT);
    CHECK(mt_gles_get_stats().misuses == 4);
    // GL ES 1.1's format rules for glTexImage2D
    e.call(mt_hle_glTexImage2D, {GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    CHECK(mt_gles_get_error() == GL_INVALID_OPERATION);
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4, 0});
    CHECK(mt_gles_get_error() == GL_INVALID_OPERATION);
    e.call(mt_hle_glTexImage2D, {GL_TEXTURE_2D, 0, 0x1234, 2, 2, 0, 0x1234, GL_UNSIGNED_BYTE, 0});
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    CHECK(mt_gfx_texture_size(handle, &w, &h) == 0);
    CHECK(w == 2);
    // every other GL ES 1.1 pair is converted too (#57): the texture gets its texels, nothing
    // is logged
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, e.alloc(32)});
    REQUIRE(mt_gfx_texture_texels(handle, nullptr, nullptr) != nullptr);
    CHECK(mt_gfx_texture_texels(handle, nullptr, nullptr)[3] == 255);
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_LUMINANCE, 4, 4, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, e.alloc(16)});
    CHECK(mt_gfx_texture_texels(handle, nullptr, nullptr) != nullptr);
    CHECK_FALSE(logged("could not store"));
    CHECK(mt_gles::state().textures.at(a).width == 4);
    // pixels outside guest memory: a misuse, the texture transparent black of its size
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0x00800000u});
    CHECK(logged("gl: glTexImage2D: pixels outside mapped guest memory: pixels 0x00800000, 64"));
    CHECK(mt_gfx_texture_size(handle, &w, &h) == 0);
    CHECK(w == 4);
    CHECK(mt_gfx_texture_texels(handle, nullptr, nullptr)[3] == 0);
    // deleting frees the mt_gfx texture; the other name never became one
    e.call(mt_hle_glDeleteTextures, {2, names});
    CHECK(mt_gfx_texture_size(handle, nullptr, nullptr) == -1);
    CHECK(mt_gfx_get_stats().textures == textures_before);
    (void)b;
    // a new process frees what the old one left
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, 9});
    CHECK(mt_gfx_get_stats().textures == textures_before + 1);
    mt_gles_reset();
    CHECK(mt_gfx_get_stats().textures == textures_before);
}

TEST_CASE("gles fixed: texturing needs GL_TEXTURE_2D and a complete texture") {
    Gl e;
    const std::vector<std::uint8_t> red = {255, 0, 0, 255};
    e.texture(1, 1, red); // 1 x 1: complete with any filter
    const u32 names = e.alloc(4);
    e.call(mt_hle_glGenTextures, {1, names});
    e.color(0, 1, 0, 1);
    e.rect(0, 0, 10, 10); // GL_TEXTURE_2D off: the colour
    CHECK(px(e.target(), 5, 315) == golden::Rgba{0, 255, 0, 255});
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.color(1, 1, 1, 1);
    e.rect(0, 0, 10, 10); // red, modulated by white
    CHECK(px(e.target(), 5, 315) == golden::Rgba{255, 0, 0, 255});
    // a 2 x 2 texture with the initial minification filter (GL_NEAREST_MIPMAP_LINEAR) and no
    // mipmaps is incomplete: drawn as if texturing were off, logged once
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, e.get32(names)});
    const std::vector<std::uint8_t> blue(16, 255);
    e.call(mt_hle_glTexImage2D, {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 e.bytes(blue.data(), 16)});
    e.color(0, 0, 1, 1);
    e.rect(20, 0, 30, 10);
    e.rect(20, 0, 30, 10);
    CHECK(px(e.target(), 25, 315) == golden::Rgba{0, 0, 255, 255});
    std::size_t notes = 0;
    for (const auto &l : g_log) {
        notes += l.find("an incomplete texture") != std::string::npos ? 1u : 0u;
    }
    CHECK(notes == 1);
    // GL_GENERATE_MIPMAP makes it complete (mt_gfx samples level 0)
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_GENERATE_MIPMAP, 1});
    e.color(0.5, 0.5, 0.5, 1);
    e.rect(20, 0, 30, 10);
    CHECK(px(e.target(), 25, 315) == golden::Rgba{128, 128, 128, 255});
    CHECK(mt_gles_get_stats().misuses == 0);
}

// --- targets, frames, errors, trace ---------------------------------------------------------

TEST_CASE("gles fixed: renderbuffer storage makes the target, at the render scale; presents") {
    struct Seen {
        std::vector<mt_gles_frame> frames;
    } seen;
    mt_gles_config cfg{};
    cfg.user = &seen;
    cfg.render_scale = 2;
    cfg.present = [](void *user, mt_cpu *, const mt_gles_frame *f) {
        static_cast<Seen *>(user)->frames.push_back(*f);
        return 0;
    };
    Gl e(false);
    mt_gles_configure(&cfg);
    const u32 context = e.create_gl();
    const mt_gfx_target *t = mt_gles::draw_target(mt_gles::state());
    REQUIRE(t != nullptr);
    CHECK(mt_gfx_target_width(t) == 480);
    CHECK(mt_gfx_target_height(t) == 320);
    CHECK(mt_gfx_target_scale(t) == 2);
    CHECK(mt_gfx_target_pixel_width(t) == 960);
    // GL keeps seeing the logical size
    const u32 out = e.alloc(4);
    e.call(mt_hle_glGetRenderbufferParameterivOES,
           {GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, out});
    CHECK(e.get32(out) == 480);
    const std::uint64_t gfx_frames = mt_gfx_get_stats().frames;
    CHECK(e.send(context, "presentRenderbuffer:", {GL_RENDERBUFFER_OES}) == 1);
    REQUIRE(seen.frames.size() == 1);
    CHECK(seen.frames[0].target == t);
    CHECK(mt_gfx_get_stats().frames == gfx_frames + 1); // mt_gfx_end_frame at the present
    // new storage replaces the target (scale 1 now)
    cfg.render_scale = 0;
    mt_gles_configure(&cfg);
    u32 layer = 0;
    e.create_gl("EAGLColorFormatRGBA8", &layer, 64, 32);
    const mt_gfx_target *t2 = mt_gles::draw_target(mt_gles::state());
    REQUIRE(t2 != nullptr);
    CHECK(mt_gfx_target_scale(t2) == 1);
    CHECK(mt_gfx_target_width(t2) == 64);
    CHECK(mt_gfx_target_pixel_height(t2) == 32);
    CHECK(mt_gles_get_stats().misuses == 0);
}

TEST_CASE("gles fixed: GL's error flag keeps the first error; misuse is logged once per kind") {
    Gl e;
    CHECK(mt_gles_get_error() == GL_NO_ERROR);
    for (int i = 0; i < 12; i++) {
        e.call(mt_hle_glBindTexture, {0x1234, 1});                  // GL_INVALID_ENUM
        e.call(mt_hle_glViewport, {0, 0, static_cast<u32>(-1), 1}); // GL_INVALID_VALUE
    }
    const mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.misuses == 24);
    CHECK(st.error == GL_INVALID_ENUM);
    CHECK(mt_gles_get_stats().error == GL_INVALID_ENUM); // reading the stats keeps it
    CHECK(mt_gles_get_error() == GL_INVALID_ENUM);
    CHECK(mt_gles_get_error() == GL_NO_ERROR);
    CHECK(mt_gles_get_stats().error == GL_NO_ERROR);
    std::size_t bind = 0, viewport = 0;
    for (const auto &l : g_log) {
        bind += l.find("glBindTexture: target is not GL_TEXTURE_2D") != std::string::npos ? 1u : 0u;
        viewport += l.find("glViewport: negative size") != std::string::npos ? 1u : 0u;
    }
    CHECK(bind == 2); // the first time and the 10th
    CHECK(viewport == 2);
    CHECK(logged("gl: glBindTexture: target is not GL_TEXTURE_2D (GL_INVALID_ENUM: the device "
                 "ignores the call)"));
    CHECK(logged("gl: glBindTexture: target is not GL_TEXTURE_2D (GL_INVALID_ENUM: the device "
                 "ignores the call; 10 times so far)"));
}

TEST_CASE("gles fixed: the GL trace file has every call, decoded, each draw and present") {
    const fs::path path = test_dir() / "gl-trace.txt";
    const std::string path_text = path.string();
    mt_gles_config cfg{};
    cfg.trace_file = path_text.c_str();
    Gl e(false);
    mt_gles_configure(&cfg);
    const u32 context = e.create_gl();
    e.call(mt_hle_glViewport, {0, 0, 480, 320});
    e.call(mt_hle_glOrthox, {0, X(480), 0, X(320), static_cast<u32>(X(-1)), X(1)});
    e.call(mt_hle_glEnableClientState, {GL_VERTEX_ARRAY});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, 3});
    const u32 texel = e.alloc(4);
    e.cpu.r[MT_LR] = 0x9883; // returning into the texture loader, after its BLX at 0x987E
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel});
    e.cpu.r[MT_LR] = 0;
    e.call(mt_hle_glBlendFunc, {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    e.call(mt_hle_glScalef, {fb(1.5f), fb(-2.0f), fb(1.0f)});
    e.call(mt_hle_glColor4x, {X(1), 0x8000u, 0, 0xFF00u});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_GENERATE_MIPMAP, 1});
    const u32 v = e.alloc(16);
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, v});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLE_STRIP, 0, 4});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLE_FAN, 0, 0});
    e.call(mt_hle_glIsTexture, {3});
    e.call(mt_hle_glBindTexture, {0x1234, 1});
    CHECK(e.send(context, "presentRenderbuffer:", {GL_RENDERBUFFER_OES}) == 1);
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string trace = text.str();
    INFO(trace);
    for (const char *line : {
             "# new GL state\n",
             "glGenFramebuffersOES(1, 0x",
             "glBindFramebufferOES(GL_FRAMEBUFFER_OES, 1)\n",
             "glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) -> "
             "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_OES\n",
             "renderbuffer 1: storage 480x320, format 0x8058, retained backing NO, target scale "
             "1\n",
             "glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, "
             "GL_RENDERBUFFER_OES, 1)\n",
             "glViewport(0, 0, 480, 320)\n",
             "glOrthox(0, 480, 0, 320, -1, 1)\n",
             "glEnableClientState(GL_VERTEX_ARRAY)\n",
             "glBindTexture(GL_TEXTURE_2D, 3)\n",
             "glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0x",
             ") from 0x987E\n",
             "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)\n",
             "glScalef(1.5, -2, 1)\n",
             "glColor4x(1, 0.5, 0, 0.99609375)\n",
             "glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)\n",
             "glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE)\n",
             "glVertexPointer(2, GL_SHORT, 0, 0x",
             "glDrawArrays(GL_TRIANGLE_STRIP, 0, 4): 4 vertices, drawn\n",
             "glDrawArrays(GL_TRIANGLE_FAN, 0, 0): skipped: count 0\n",
             "glIsTexture(3) -> GL_TRUE\n",
             "# glBindTexture: target is not GL_TEXTURE_2D (GL_INVALID_ENUM",
             "glBindTexture(0x1234, 1)\n",
             "present frame 1: ",
         }) {
        CHECK_MESSAGE(trace.find(line) != std::string::npos, line);
    }
    // "" turns it off (and closes the file)
    cfg.trace_file = "";
    mt_gles_configure(&cfg);
    e.call(mt_hle_glLoadIdentity, {});
    in.close();
    std::ifstream again(path);
    std::stringstream after;
    after << again.rdbuf();
    CHECK(after.str().find("glLoadIdentity") == std::string::npos);
    mt_gles_configure(nullptr);
}

// --- golden images --------------------------------------------------------------------------

void check_golden(const mt_gfx_target *t, const char *name) {
    const fs::path path = fs::path(MT_GOLDEN_DATA_DIR) / (std::string(name) + ".png");
    const auto r = golden::check_golden(to_image(t), path, std::string("gles/") + name); // exact
    CHECK_MESSAGE(r.passed, r.message);
}

// A synthetic sprite: a w x h RGBA8 image with a coloured gradient, a darker border and a
// round alpha mask with a soft edge, the bottom row (t = 0) marked red to show orientation.
std::vector<std::uint8_t> sprite(u32 w, u32 h) {
    std::vector<std::uint8_t> p(static_cast<std::size_t>(w) * h * 4);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            std::uint8_t *q = &p[(static_cast<std::size_t>(y) * w + x) * 4];
            const int dx = 2 * static_cast<int>(x) + 1 - static_cast<int>(w);
            const int dy = 2 * static_cast<int>(y) + 1 - static_cast<int>(h);
            const int r2 = dx * dx + dy * dy, rmax = static_cast<int>(w * w);
            const bool border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
            q[0] = static_cast<std::uint8_t>(y == 0 ? 255 : 40 + 200 * x / w);
            q[1] = static_cast<std::uint8_t>(y == 0 ? 0 : 60 + 180 * y / h);
            q[2] = static_cast<std::uint8_t>(y == 0 ? 0 : border ? 30 : 200);
            q[3] =
                static_cast<std::uint8_t>(r2 >= rmax           ? 64
                                          : r2 >= rmax * 3 / 4 ? 64 + 191 * (rmax - r2) / (rmax / 4)
                                                               : 255);
        }
    }
    return p;
}

// A 4 x 4 checkerboard of two colours.
std::vector<std::uint8_t> checker(std::uint8_t a0, std::uint8_t a1) {
    std::vector<std::uint8_t> p(64);
    for (u32 i = 0; i < 16; i++) {
        const bool odd = ((i % 4) + (i / 4)) % 2 != 0;
        p[i * 4 + 0] = odd ? 230 : 30;
        p[i * 4 + 1] = odd ? 220 : 60;
        p[i * 4 + 2] = odd ? 200 : 110;
        p[i * 4 + 3] = odd ? a1 : a0;
    }
    return p;
}

TEST_CASE("gles fixed: golden image of ortho quads (gles_ortho_quad.png)") {
    Gl e;
    // an untextured background, then textured rectangles of the game's kind: GL_SHORT positions,
    // GL_FIXED texture coordinates, triangle strips, y up
    e.color(0.1, 0.15, 0.3, 1);
    e.rect(0, 0, 480, 320);
    const u32 checks = e.texture(4, 4, checker(255, 255));
    const u32 sp = e.texture(16, 16, sprite(16, 16));
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, checks});
    e.color(1, 1, 1, 1);
    e.rect(16, 16, 144, 144);             // 32 x magnified, nearest
    e.rect(160, 16, 224, 80, 0, 0, 4, 4); // repeated 4 x 4 times
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE});
    e.rect(240, 16, 368, 144);                      // bilinear
    e.rect(384, 16, 464, 96, -0.5, -0.5, 1.5, 1.5); // clamped
    // the sprite: bottom row red (t = 0 is the bottom in GL), nearest, opaque and blended, tinted
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, sp});
    e.rect(16, 176, 112, 272);
    e.call(mt_hle_glEnable, {GL_BLEND});
    e.call(mt_hle_glBlendFunc, {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    e.rect(128, 176, 224, 272);
    e.color(1, 0.5, 0.25, 0.75);
    e.rect(240, 176, 336, 272);
    // flipped by the modelview (glScalef -1 about the rectangle's centre) and translated
    e.color(1, 1, 1, 1);
    e.call(mt_hle_glPushMatrix, {});
    e.call(mt_hle_glTranslatex, {X(400), X(224), 0});
    e.call(mt_hle_glScalef, {fb(-1.0f), fb(1.0f), fb(1.0f)});
    e.rect(-48, -48, 48, 48);
    e.call(mt_hle_glPopMatrix, {});
    e.call(mt_hle_glDisable, {GL_BLEND});
    e.call(mt_hle_glDisable, {GL_TEXTURE_2D});
    // half-pixel edges, a thin untextured bar
    e.color(1, 1, 0, 1);
    const u32 halves = e.array<u32>(
        {XD(16.5), XD(296.5), XD(463.5), XD(296.5), XD(16.5), XD(299.5), XD(463.5), XD(299.5)});
    e.call(mt_hle_glVertexPointer, {2, GL_FIXED, 0, halves});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLE_STRIP, 0, 4});
    const mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.misuses == 0);
    CHECK(st.draws_drawn == 10);
    check_golden(e.target(), "gles_ortho_quad");
}

TEST_CASE("gles fixed: golden image of rotated sprites (gles_rotated_sprite.png)") {
    Gl e;
    e.color(0.2, 0.2, 0.2, 1);
    e.rect(0, 0, 480, 320);
    const u32 sp = e.texture(32, 32, sprite(32, 32), GL_LINEAR);
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.call(mt_hle_glEnable, {GL_BLEND});
    e.call(mt_hle_glBlendFunc, {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, sp});
    e.color(1, 1, 1, 1);
    // sprites turned about their centres: translate, rotate, scale, then the rectangle
    struct Turn {
        int x, y, degrees;
        float scale;
    };
    const Turn turns[] = {{60, 240, 0, 1.5f},     {170, 240, 30, 1.5f},      {280, 240, 90, 1.5f},
                          {390, 240, -135, 1.5f}, {60, 90, 45, 2.0f},        {200, 90, 200, 2.5f},
                          {340, 90, -15, 1.0f},   {430, 90, 720 + 60, 1.25f}};
    for (const Turn &t : turns) {
        e.call(mt_hle_glPushMatrix, {});
        e.call(mt_hle_glTranslatex, {X(t.x), X(t.y), 0});
        e.call(mt_hle_glRotatex, {static_cast<u32>(X(t.degrees)), 0, 0, X(1)});
        e.call(mt_hle_glScalef, {fb(t.scale), fb(t.scale), fb(1.0f)});
        e.rect(-16, -16, 16, 16);
        e.call(mt_hle_glPopMatrix, {});
    }
    // one turned through the texture matrix instead (about the texture's centre), clamped
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE});
    e.call(mt_hle_glMatrixMode, {GL_TEXTURE});
    e.call(mt_hle_glTranslatex, {0x8000u, 0x8000u, 0});
    e.call(mt_hle_glRotatex, {X(60), 0, 0, X(1)});
    e.call(mt_hle_glTranslatex, {static_cast<u32>(-0x8000), static_cast<u32>(-0x8000), 0});
    e.call(mt_hle_glMatrixMode, {GL_MODELVIEW});
    e.rect(400, 150, 464, 214);
    // a sprite turned 60 degrees about x: its y becomes depth, and the ortho projection's depth
    // range [-1, 1] clips all of it but a thin band about its middle row (GL clips against near
    // and far; the game never turns about x)
    e.call(mt_hle_glMatrixMode, {GL_TEXTURE});
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glMatrixMode, {GL_MODELVIEW});
    e.call(mt_hle_glPushMatrix, {});
    e.call(mt_hle_glTranslatex, {X(320), X(182), 0});
    e.call(mt_hle_glRotatex, {X(60), X(1), 0, 0});
    e.rect(-32, -32, 32, 32);
    e.call(mt_hle_glPopMatrix, {});
    CHECK(mt_gles_get_stats().misuses == 0);
    check_golden(e.target(), "gles_rotated_sprite");
}

TEST_CASE("gles fixed: golden image of blend modes (gles_blend_modes.png)") {
    Gl e;
    // a background of horizontal colour bands with an alpha ramp from left to right
    std::vector<std::uint8_t> ramp(16 * 4 * 4);
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 16; x++) {
            std::uint8_t *q = &ramp[(y * 16 + x) * 4];
            q[0] = static_cast<std::uint8_t>(y == 0 || y == 3 ? 220 : 40);
            q[1] = static_cast<std::uint8_t>(y == 1 || y == 3 ? 200 : 50);
            q[2] = static_cast<std::uint8_t>(y == 2 || y == 3 ? 230 : 60);
            q[3] = static_cast<std::uint8_t>(x * 17);
        }
    }
    const u32 bg = e.texture(16, 4, ramp, GL_LINEAR);
    const u32 sp = e.texture(16, 16, sprite(16, 16), GL_NEAREST);
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, bg});
    e.color(1, 1, 1, 1);
    e.rect(0, 0, 480, 320);
    // the factor pairs: the game's (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) and the other common ones,
    // each tile a tinted, half-transparent sprite over the background
    const u32 pairs[][2] = {
        {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA},
        {GL_ONE, GL_ONE},
        {GL_SRC_ALPHA, GL_ONE},
        {GL_ONE, GL_ONE_MINUS_SRC_ALPHA},
        {GL_DST_COLOR, GL_ZERO},
        {GL_ZERO, GL_SRC_COLOR},
        {GL_ONE_MINUS_DST_COLOR, GL_ONE},
        {GL_SRC_ALPHA_SATURATE, GL_ONE},
        {GL_ZERO, GL_ONE_MINUS_SRC_COLOR},
        {GL_ONE, GL_ZERO},
    };
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, sp});
    e.call(mt_hle_glEnable, {GL_BLEND});
    int i = 0;
    for (const auto &p : pairs) {
        e.call(mt_hle_glBlendFunc, {p[0], p[1]});
        const int x = 16 + (i % 5) * 92, y = i < 5 ? 176 : 32;
        e.color(1, 1, 1, 1);
        e.rect(x, y + 64, x + 64, y + 128);
        e.color(0.9, 0.6, 0.3, 0.5);
        e.rect(x, y, x + 64, y + 64);
        i++;
    }
    // blending off: the fragment replaces the target (alpha included)
    e.call(mt_hle_glDisable, {GL_BLEND});
    e.call(mt_hle_glDisable, {GL_TEXTURE_2D});
    e.color(1, 1, 1, 0.25);
    e.rect(456, 4, 476, 316);
    CHECK(mt_gles_get_stats().misuses == 0);
    check_golden(e.target(), "gles_blend_modes");
}

// --- the game's texture path (#57, ADR 0041) -------------------------------------------------
//
// The game's uploads, call by call: the texture loader FUN_00009534 (CoreGraphics draws the
// decoded PNG into a premultiplied power-of-two context, which is uploaded as GL_RGBA /
// GL_UNSIGNED_BYTE), its sprite draw FUN_00007ccc (texture coordinates over the power-of-two
// size, a 1:1 quad), the text texture of FUN_00007ad8 / FUN_00007558, and FUN_0000949c's texture
// without an image. The device blends these premultiplied texels with GL_SRC_ALPHA,
// GL_ONE_MINUS_SRC_ALPHA; the port does the same, stage by stage.

// The game's power of two (FUN_00006558): n if it is one, else 2 << floor(log2 n).
u32 game_pot(u32 n) {
    if (static_cast<std::int32_t>(n) <= 0 || (n & (n - 1)) == 0) {
        return n;
    }
    u32 bits = 0;
    for (u32 v = n; (v >>= 1) != 0;) {
        bits++;
    }
    return 2u << bits;
}

std::uint8_t premul(std::uint8_t c, std::uint8_t a) {
    return static_cast<std::uint8_t>((c * a + 127) / 255);
}

// A PNG of straight RGBA pixels (top row first).
std::vector<std::uint8_t> png_of(const std::vector<std::uint8_t> &rgba, u32 w, u32 h,
                                 u32 channels = 4) {
    std::vector<std::uint8_t> png;
    REQUIRE(mt_cg::png_encode(rgba.data(), w, h, channels, png));
    return png;
}

struct Loaded {
    u32 name = 0, w = 0, h = 0, pot_w = 0, pot_h = 0;
    u32 pixels = 0; // the context's buffer: what glTexImage2D got
};

// FUN_00009534: -[UIImage initWithData:], CGImageGet*, a calloc'd power-of-two bitmap context
// (alpha info 1..4: kCGImageAlphaPremultipliedLast, else kCGImageAlphaNoneSkipLast, both
// | kCGBitmapByteOrder32Big), TranslateCTM(0, H), ScaleCTM(1, -1), DrawImage((0, H - h),
// (w, h)), then glGenTextures, glBindTexture, glTexImage2D(GL_RGBA, W, H, GL_RGBA,
// GL_UNSIGNED_BYTE), MIN and MAG filter GL_LINEAR (after the upload, as the game does).
Loaded load_texture(Gl &e, const std::vector<std::uint8_t> &png) {
    Loaded t;
    const u32 data = mt_cf::data_create(&e.cpu, png.data(), static_cast<u32>(png.size()), false);
    const u32 image = e.send(e.send(e.klass("UIImage"), "alloc"), "initWithData:", {data});
    REQUIRE(image != 0);
    const u32 cg = e.send(image, "CGImage");
    t.w = e.call(mt_hle_CGImageGetWidth, {cg});
    t.h = e.call(mt_hle_CGImageGetHeight, {cg});
    t.pot_w = game_pot(t.w);
    t.pot_h = game_pot(t.h);
    const u32 alpha = e.call(mt_hle_CGImageGetAlphaInfo, {cg});
    const u32 info = (alpha >= 1 && alpha <= 4 ? 1u : 5u) | 0x4000u;
    t.pixels = e.alloc(t.pot_w * t.pot_h * 4);
    const u32 space = e.call(mt_hle_CGColorSpaceCreateDeviceRGB, {});
    const u32 ctx = e.call(mt_hle_CGBitmapContextCreate,
                           {t.pixels, t.pot_w, t.pot_h, 8, t.pot_w * 4, space, info});
    REQUIRE(ctx != 0);
    e.call(mt_hle_CGContextTranslateCTM, {ctx, fb(0), fb(static_cast<float>(t.pot_h))});
    e.call(mt_hle_CGContextScaleCTM, {ctx, fb(1), fb(-1)});
    e.call(mt_hle_CGContextDrawImage,
           {ctx, fb(0), fb(static_cast<float>(t.pot_h - t.h)), fb(static_cast<float>(t.w)),
            fb(static_cast<float>(t.h)), cg});
    e.call(mt_hle_CGContextRelease, {ctx});
    e.call(mt_hle_CGColorSpaceRelease, {space});
    const u32 names = e.alloc(4);
    e.call(mt_hle_glGenTextures, {1, names});
    t.name = e.get32(names);
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, t.name});
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGBA, t.pot_w, t.pot_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.pixels});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR});
    e.send(image, "release");
    return t;
}

// FUN_00007ccc: the image's source rectangle (sx, sy, w, h) (y down from its top) at screen
// point (x, y) (y down from the top: FUN_00006088's quad, case 5), texture coordinates as
// truncated 16.16 quotients (___divsi3) over the power-of-two size, the colour (1, 1, 1,
// alpha << 8) with GL_BLEND and GL_ALPHA_TEST around the draw, or (1, 1, 1, 1) unblended for
// alpha -1. `color` replaces the colour (FUN_00007558's text colour, r << 8 ...) when given.
void game_sprite(Gl &e, const Loaded &t, int x, int y, int sx, int sy, int w, int h,
                 int alpha = 255, const u32 *color = nullptr) {
    const int H = static_cast<int>(mt_gfx_target_height(e.target()));
    auto s16 = [](int v) { return static_cast<std::int16_t>(v); };
    const u32 v = e.array<std::int16_t>({s16(x), s16(H - y), s16(x), s16(H - (y + h)), s16(x + w),
                                         s16(H - y), s16(x + w), s16(H - (y + h))});
    const int pw = static_cast<int>(t.pot_w), ph = static_cast<int>(t.pot_h);
    const auto s0 = static_cast<u32>((sx << 16) / pw), s1 = static_cast<u32>(((sx + w) << 16) / pw);
    const auto t0 = static_cast<u32>(((ph - sy) << 16) / ph);
    const auto t1 = static_cast<u32>(((ph - (sy + h)) << 16) / ph);
    const u32 tc = e.array<u32>({s0, t0, s0, t1, s1, t0, s1, t1});
    if (alpha == -1) {
        e.call(mt_hle_glColor4x, {X(1), X(1), X(1), X(1)});
    } else {
        e.call(mt_hle_glEnable, {GL_BLEND});
        e.call(mt_hle_glEnable, {GL_ALPHA_TEST});
        if (color != nullptr) {
            e.call(mt_hle_glColor4x, {color[0], color[1], color[2], color[3]});
        } else {
            e.call(mt_hle_glColor4x, {X(1), X(1), X(1), static_cast<u32>(alpha) << 8});
        }
    }
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.call(mt_hle_glEnableClientState, {GL_TEXTURE_COORD_ARRAY});
    e.call(mt_hle_glPushMatrix, {});
    e.call(mt_hle_glLoadIdentity, {});
    e.call(mt_hle_glScalef, {fb(1), fb(1), fb(1)});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, t.name});
    e.call(mt_hle_glTexCoordPointer, {2, GL_FIXED, 0, tc});
    e.call(mt_hle_glVertexPointer, {2, GL_SHORT, 0, v});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLE_STRIP, 0, 4});
    e.call(mt_hle_glPopMatrix, {});
    e.call(mt_hle_glDisable, {GL_TEXTURE_2D});
    e.call(mt_hle_glDisableClientState, {GL_TEXTURE_COORD_ARRAY});
    if (alpha != -1) {
        e.call(mt_hle_glDisable, {GL_BLEND});
        e.call(mt_hle_glDisable, {GL_ALPHA_TEST});
    }
}

TEST_CASE("gles texture: the loader premultiplies like CoreGraphics, every colour x alpha pair") {
    Gl e(true, 32, 32);
    // A straight-alpha image of every (colour, alpha) pair: x is the colour, y the alpha.
    std::vector<std::uint8_t> all(256 * 256 * 4);
    for (u32 a = 0; a < 256; a++) {
        for (u32 c = 0; c < 256; c++) {
            std::uint8_t *p = &all[(a * 256 + c) * 4];
            p[0] = static_cast<std::uint8_t>(c);
            p[1] = static_cast<std::uint8_t>(255 - c);
            p[2] = static_cast<std::uint8_t>(c * 7);
            p[3] = static_cast<std::uint8_t>(a);
        }
    }
    const Loaded t = load_texture(e, png_of(all, 256, 256));
    REQUIRE(t.pot_w == 256);
    // The texels are the context's bytes, unchanged: image row y in texel row 255 - y, each
    // colour round(c * a / 255) (macOS CoreGraphics gives the same for all 65,536 pairs; ADR
    // 0041), the alpha as decoded.
    u32 tw = 0, th = 0;
    const std::uint8_t *texels =
        mt_gfx_texture_texels(mt_gles::state().textures.at(t.name).gfx, &tw, &th);
    REQUIRE(texels != nullptr);
    CHECK(std::memcmp(texels, e.cpu.mem + t.pixels, 256 * 256 * 4) == 0);
    u32 wrong = 0;
    for (u32 y = 0; y < 256; y++) {
        for (u32 x = 0; x < 256; x++) {
            const std::uint8_t *s = &all[(y * 256 + x) * 4];
            const std::uint8_t *q = texels + ((255 - y) * 256 + x) * 4;
            wrong += q[0] != premul(s[0], s[3]) || q[1] != premul(s[1], s[3]) ||
                     q[2] != premul(s[2], s[3]) || q[3] != s[3];
        }
    }
    CHECK(wrong == 0);
    // Hand-computed: colour 128 at alpha 51 is 25.6 -> 26; 255 at 1 is 1; 1 at 128 is 0.5 -> 1.
    CHECK(texels[((255 - 51) * 256 + 128) * 4] == 26);
    CHECK(texels[((255 - 1) * 256 + 255) * 4] == 1);
    CHECK(texels[((255 - 128) * 256 + 1) * 4] == 1);

    // A PNG without alpha (RGB): alpha info kCGImageAlphaNoneSkipLast, a NoneSkipLast context.
    // CoreGraphics writes 0xFF into the skip byte, which GL reads as alpha: opaque, while the
    // padding the image does not cover stays transparent black (calloc).
    std::vector<std::uint8_t> rgb(5 * 3 * 3);
    for (std::size_t i = 0; i < rgb.size(); i++) {
        rgb[i] = static_cast<std::uint8_t>(i * 5);
    }
    const Loaded o = load_texture(e, png_of(rgb, 5, 3, 3));
    CHECK(o.pot_w == 8);
    CHECK(o.pot_h == 4);
    const std::uint8_t *ot =
        mt_gfx_texture_texels(mt_gles::state().textures.at(o.name).gfx, nullptr, nullptr);
    REQUIRE(ot != nullptr);
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 8; x++) {
            const std::uint8_t *q = ot + (y * 8 + x) * 4;
            INFO("texel " << x << ", " << y);
            if (x < 5 && y >= 1) { // image row 3 - y (bottom row of texels is the padding)
                const std::uint8_t *s = &rgb[((3 - y) * 5 + x) * 3];
                CHECK(golden::Rgba{q[0], q[1], q[2], q[3]} == golden::Rgba{s[0], s[1], s[2], 255});
            } else {
                CHECK(golden::Rgba{q[0], q[1], q[2], q[3]} == golden::Rgba{0, 0, 0, 0});
            }
        }
    }
    CHECK(mt_gles_get_stats().misuses == 0);
}

TEST_CASE("gles texture: the game's parameter sequences give GL_LINEAR, complete textures") {
    Gl e(true, 32, 32);
    // FUN_00009534 and FUN_00007ad8: the image first (while the minification filter is still
    // GL_NEAREST_MIPMAP_LINEAR), then MIN and MAG GL_LINEAR. Completeness is decided at the
    // draw, so the texture is complete and both filters are linear.
    std::vector<std::uint8_t> img(16 * 16 * 4, 200);
    const Loaded t = load_texture(e, png_of(img, 16, 16));
    const mt_gfx::Texture *g = mt_gfx::find_texture(mt_gles::state().textures.at(t.name).gfx);
    REQUIRE(g != nullptr);
    CHECK(g->min_filter == GL_LINEAR);
    CHECK(g->mag_filter == GL_LINEAR);
    CHECK(g->wrap_s == GL_REPEAT);
    CHECK(g->wrap_t == GL_REPEAT);
    e.call(mt_hle_glBlendFunc, {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    e.color(0, 0, 0, 1);
    e.rect(0, 0, 32, 32);
    game_sprite(e, t, 0, 0, 0, 0, 16, 16, -1);
    CHECK(px(e.target(), 5, 5) == golden::Rgba{157, 157, 157, 200}); // 200 * 200 / 255 = 156.9
    CHECK_FALSE(logged("incomplete texture"));

    // FUN_0000949c (CStateTitle::Enter): a texture object with only GL_TEXTURE_MAG_FILTER set
    // and no image. Bound and drawn it is incomplete (no image, and the default mipmapped
    // minification filter): the draw is untextured, as on the device.
    const u32 names = e.alloc(4);
    e.call(mt_hle_glGenTextures, {1, names});
    const u32 bare = e.get32(names);
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, bare});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR});
    const mt_gfx::Texture *b = mt_gfx::find_texture(mt_gles::state().textures.at(bare).gfx);
    REQUIRE(b != nullptr);
    CHECK(b->min_filter == 0x2702u); // GL_NEAREST_MIPMAP_LINEAR, GL's initial value
    CHECK(b->texels.empty());
    Loaded bare_t;
    bare_t.name = bare;
    bare_t.pot_w = bare_t.pot_h = 16;
    const u32 green[4] = {0, X(1), 0, X(1)};
    game_sprite(e, bare_t, 16, 16, 0, 0, 16, 16, 255, green);
    CHECK(px(e.target(), 20, 20) == golden::Rgba{0, 255, 0, 255});
    CHECK(mt_gles_get_stats().misuses == 0);
}

TEST_CASE("gles texture: 1:1 sprites sample texel centres, so the POT padding never bleeds in") {
    Gl e;
    // A 20 x 12 image in a 32 x 16 texture, laid out as the loader leaves it (image row y in
    // texel row 15 - y, columns from 0), with the padding opaque magenta: a texel of padding
    // that bled into a sprite would show. Every pixel of FUN_00007ccc's 1:1 quads must be
    // exactly its texel (colour 1, no blending), for every source rectangle and position.
    const u32 w = 20, h = 12, pw = 32, ph = 16;
    std::vector<std::uint8_t> texels(pw * ph * 4);
    auto image = [](u32 x, u32 y) {
        return golden::Rgba{static_cast<std::uint8_t>(x * 12), static_cast<std::uint8_t>(y * 20),
                            static_cast<std::uint8_t>((x * 7 + y * 13) & 255),
                            static_cast<std::uint8_t>(255 - x * 3)};
    };
    for (u32 ty = 0; ty < ph; ty++) {
        for (u32 tx = 0; tx < pw; tx++) {
            std::uint8_t *q = &texels[(ty * pw + tx) * 4];
            const bool inside = tx < w && ty >= ph - h;
            const golden::Rgba c = inside ? image(tx, ph - 1 - ty) : golden::Rgba{255, 0, 255, 255};
            q[0] = c.r;
            q[1] = c.g;
            q[2] = c.b;
            q[3] = c.a;
        }
    }
    Loaded t;
    t.name = e.texture(pw, ph, texels, GL_LINEAR);
    t.w = w;
    t.h = h;
    t.pot_w = pw;
    t.pot_h = ph;
    struct Case {
        int x, y, sx, sy, cw, ch;
    };
    const Case cases[] = {
        {0, 0, 0, 0, 20, 12},     {7, 3, 0, 0, 20, 12},    {101, 57, 0, 0, 20, 12},
        {13, 200, 3, 5, 17, 7},   {250, 31, 1, 1, 19, 11}, {333, 111, 5, 0, 9, 12},
        {460, 300, 0, 2, 20, 10}, {41, 77, 19, 11, 1, 1},  {200, 160, 6, 4, 13, 5},
    };
    u32 wrong = 0, magenta = 0;
    for (const Case &c : cases) {
        e.color(0, 0, 0, 1);
        e.call(mt_hle_glDisable, {GL_TEXTURE_2D});
        e.rect(0, 0, 480, 320);
        game_sprite(e, t, c.x, c.y, c.sx, c.sy, c.cw, c.ch, -1);
        for (int j = -1; j <= c.ch; j++) {
            for (int i = -1; i <= c.cw; i++) {
                const int x = c.x + i, y = c.y + j;
                if (x < 0 || y < 0 || x >= 480 || y >= 320) {
                    continue;
                }
                const golden::Rgba got = px(e.target(), static_cast<u32>(x), static_cast<u32>(y));
                const bool inside = i >= 0 && j >= 0 && i < c.cw && j < c.ch;
                const golden::Rgba want =
                    inside ? image(static_cast<u32>(c.sx + i), static_cast<u32>(c.sy + j))
                           : golden::Rgba{0, 0, 0, 255};
                wrong += got == want ? 0u : 1u;
                magenta += got.r == 255 && got.b == 255 && got.g == 0 ? 1u : 0u;
                if (got != want && wrong <= 5) {
                    INFO("case at " << c.x << ", " << c.y << ": pixel " << i << ", " << j);
                    CHECK(got == want);
                }
            }
        }
    }
    CHECK(wrong == 0);
    CHECK(magenta == 0);
}

TEST_CASE("gles texture: magnified, GL_LINEAR with GL_REPEAT mixes in what wraps around") {
    // Not the game's 1:1 draws: what GL defines (and the device does) if a padded texture is
    // stretched. At the left edge the sample wraps to the last column, the padding (transparent
    // black); at the top, with the image as high as the texture, to the image's bottom row.
    Gl e(true, 32, 32);
    // 6 x 4 in 8 x 4: column 0 opaque white at the image's top row, (128, 255, 64) at its
    // bottom row.
    std::vector<std::uint8_t> img(6 * 4 * 4, 255);
    for (u32 x = 0; x < 6; x++) {
        std::uint8_t *q = &img[(3 * 6 + x) * 4];
        q[0] = 128;
        q[1] = 255;
        q[2] = 64;
    }
    const Loaded t = load_texture(e, png_of(img, 6, 4));
    REQUIRE(t.pot_w == 8);
    REQUIRE(t.pot_h == 4);
    e.color(0, 0, 0, 1);
    e.rect(0, 0, 32, 32);
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, t.name});
    e.color(1, 1, 1, 1);
    // 4x: the image (s 0..6/8, t 0..1) over 24 x 16 pixels at the top left of the target
    e.rect(0, 16, 24, 32, 0, 0, 0.75, 1);
    // The top-left pixel samples s = 1/64, t = 1 - 1/32: u - 1/2 = -0.375, v - 1/2 = 3.375.
    // 8-bit weights: columns 7 (padding, 0) and 0 at 96 and 160, rows 3 (the image's top row,
    // white) and 0 (wrapped: its bottom row) at 160 and 96. R = (255 * 160 * 160 + 128 * 160 *
    // 96 + 32768) >> 16 = 130, G = A = (255 * 25600 + 255 * 15360 + 32768) >> 16 = 159,
    // B = (255 * 25600 + 64 * 15360 + 32768) >> 16 = 115. Unblended: those texels.
    CHECK(px(e.target(), 0, 0) == golden::Rgba{130, 159, 115, 159});
    // One pixel in from the left: u - 1/2 = -0.125, weights 32 (padding) and 224.
    // R = (255 * 224 * 160 + 128 * 224 * 96 + 32768) >> 16 = 181, G = A = 223, B = 160.
    CHECK(px(e.target(), 1, 0) == golden::Rgba{181, 223, 160, 223});
    // Inside the image and away from the top: white, exactly.
    CHECK(px(e.target(), 10, 6) == golden::Rgba{255, 255, 255, 255});
    CHECK(mt_gles_get_stats().misuses == 0);
}

TEST_CASE("gles texture: premultiplied texels under the game's blend, edge pixels by hand "
          "(gles_premultiplied_edges.png)") {
    // The device's pipeline, stage by stage (ADR 0041): a straight-alpha PNG with a soft edge
    // -> CoreGraphics' premultiplied context (round(c * a / 255)) -> glTexImage2D as is ->
    // GL_LINEAR at texel centres (1:1) -> modulated by the colour (the game's 0xFF00 alpha is
    // 254) -> GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA. Premultiplied colour under a straight-alpha
    // blend counts alpha twice: semi-transparent edges come out darker than a straight blend
    // would make them, over black and over white. That is what the device shows; nothing here
    // un-premultiplies.
    Gl e(true, 100, 44);
    const std::uint8_t alphas[6] = {255, 204, 153, 102, 51, 0};
    const golden::Rgba rows[4] = {
        {255, 255, 255, 0}, {255, 0, 0, 0}, {200, 150, 100, 0}, {128, 255, 64, 0}};
    std::vector<std::uint8_t> img(6 * 4 * 4);
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 6; x++) {
            std::uint8_t *q = &img[(y * 6 + x) * 4];
            q[0] = rows[y].r;
            q[1] = rows[y].g;
            q[2] = rows[y].b;
            q[3] = alphas[x];
        }
    }
    const Loaded t = load_texture(e, png_of(img, 6, 4));
    // The text texture of FUN_00007ad8: CoreGraphics' white text, premultiplied (a, a, a, a),
    // here an alpha ramp, uploaded and then given GL_LINEAR.
    std::vector<std::uint8_t> text(8 * 4 * 4, 0);
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 6; x++) {
            std::fill_n(&text[(y * 8 + x) * 4], 4, alphas[x]);
        }
    }
    const u32 text_pixels = e.bytes(text.data(), static_cast<u32>(text.size()));
    const u32 names = e.alloc(4);
    e.call(mt_hle_glGenTextures, {1, names});
    Loaded tt;
    tt.name = e.get32(names);
    tt.pot_w = 8;
    tt.pot_h = 4;
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, tt.name});
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGBA, 8, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, text_pixels});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR});
    e.call(mt_hle_glTexParameteri, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR});

    // The background: black on the left half, white on the right, opaque.
    e.call(mt_hle_glBlendFunc, {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    e.color(0, 0, 0, 1);
    e.rect(0, 0, 50, 44);
    e.color(1, 1, 1, 1);
    e.rect(50, 0, 100, 44);
    // 1:1 sprites as FUN_00007ccc draws them (colour (1, 1, 1, 0xFF00)), over black and white
    game_sprite(e, t, 4, 2, 0, 0, 6, 4);
    game_sprite(e, t, 54, 2, 0, 0, 6, 4);
    // the text as FUN_00007558 draws it in white: glColor4x(255 << 8, ...) = 254 everywhere
    const u32 text_white[4] = {0xFF00u, 0xFF00u, 0xFF00u, 0xFF00u};
    game_sprite(e, tt, 4, 8, 0, 0, 6, 4, 255, text_white);
    game_sprite(e, tt, 54, 8, 0, 0, 6, 4, 255, text_white);
    // magnified 4x with GL_LINEAR (GL's rule; the game does not stretch), over black and white
    e.call(mt_hle_glEnable, {GL_BLEND});
    e.call(mt_hle_glEnable, {GL_TEXTURE_2D});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, t.name});
    e.call(mt_hle_glColor4x, {X(1), X(1), X(1), 0xFF00u});
    e.rect(4, 44 - 16 - 24, 28, 44 - 16, 0, 0, 0.75, 1);
    e.rect(54, 44 - 16 - 24, 78, 44 - 16, 0, 0, 0.75, 1);
    e.call(mt_hle_glDisable, {GL_BLEND});
    e.call(mt_hle_glDisable, {GL_TEXTURE_2D});
    CHECK(mt_gles_get_stats().misuses == 0);

    // The 1:1 sprites against the model: texel p = premul(c, a); modulated, colour stays p and
    // alpha becomes sa = (a * 254 + 127) / 255; blended, (p * sa + d * (255 - sa) + 127) / 255.
    auto model = [](std::uint8_t p, std::uint8_t sa, std::uint8_t d) {
        return static_cast<std::uint8_t>((p * sa + d * (255 - sa) + 127) / 255);
    };
    u32 wrong = 0;
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 6; x++) {
            const std::uint8_t a = alphas[x];
            const auto sa = static_cast<std::uint8_t>((a * 254 + 127) / 255);
            const std::uint8_t c[3] = {rows[y].r, rows[y].g, rows[y].b};
            for (const std::uint8_t d : {std::uint8_t{0}, std::uint8_t{255}}) {
                const golden::Rgba got = px(e.target(), (d == 0 ? 4 : 54) + x, 2 + y);
                const golden::Rgba want = {model(premul(c[0], a), sa, d),
                                           model(premul(c[1], a), sa, d),
                                           model(premul(c[2], a), sa, d), model(sa, sa, 255)};
                if (a == 0) {
                    wrong += got == golden::Rgba{d, d, d, 255} ? 0u : 1u; // alpha 0: unchanged
                } else {
                    wrong += got == want ? 0u : 1u;
                }
            }
        }
    }
    CHECK(wrong == 0);
    // Hand-computed edge pixels of the white row:
    //   alpha 51: p = 51, sa = (51 * 254 + 127) / 255 = 51. Over black (51 * 51 + 127) / 255 =
    //   10 (a straight blend would give 51); over white (51 * 51 + 255 * 204 + 127) / 255 = 214
    //   (straight: 255). The target's alpha: (51 * 51 + 255 * 204 + 127) / 255 = 214.
    CHECK(px(e.target(), 4 + 4, 2) == golden::Rgba{10, 10, 10, 214});
    CHECK(px(e.target(), 54 + 4, 2) == golden::Rgba{214, 214, 214, 214});
    //   alpha 153: p = 153, sa = 152. Black: (153 * 152 + 127) / 255 = 91 (straight 153);
    //   white: (153 * 152 + 255 * 103 + 127) / 255 = 194 (straight 255).
    CHECK(px(e.target(), 4 + 2, 2) == golden::Rgba{91, 91, 91, 194});
    CHECK(px(e.target(), 54 + 2, 2) == golden::Rgba{194, 194, 194, 194});
    //   alpha 255: p = 255, sa = 254: black 254, white 255; the alpha (254 * 254 + 255 + 127)
    //   / 255 = 254.
    CHECK(px(e.target(), 4, 2) == golden::Rgba{254, 254, 254, 254});
    CHECK(px(e.target(), 54, 2) == golden::Rgba{255, 255, 255, 254});
    //   the (128, 255, 64) row at alpha 51: p = (26, 51, 13) (25.6, 51, 12.8), sa 51. Black:
    //   ((26 * 51 + 127) / 255, ...) = (5, 10, 3).
    CHECK(px(e.target(), 4 + 4, 5) == golden::Rgba{5, 10, 3, 214});
    // The text, (a, a, a, a) modulated by 254: colour and alpha (a * 254 + 127) / 255, then
    // blended. Alpha 102: 102 -> 102; over black (102 * 102 + 127) / 255 = 41, over white
    // (102 * 102 + 255 * 153 + 127) / 255 = 194.
    CHECK(px(e.target(), 4 + 3, 8) == golden::Rgba{41, 41, 41, 194});
    CHECK(px(e.target(), 54 + 3, 8) == golden::Rgba{194, 194, 194, 194});
    check_golden(e.target(), "gles_premultiplied_edges");
}

} // namespace
