// mt_gfx's GPU backend (#120, ADR 0060, maplethief/gfx_gpu.h) against the software rasterizer,
// the reference. The synthetic scenes of gfx_test.cpp's golden images (the prototype scene,
// every blend factor pair, the perspective floor, the lines) are rendered on the GPU and
// compared with the committed goldens, within ADR 0060's tolerance. The test also renders a
// scene at internal scale 4 on both backends, checks the sync points (CPU writes and reads
// between draws, a texture re-uploaded or destroyed while draws that use it are queued, a
// target destroyed with draws queued, switching backends halfway through a frame), the
// replacement hook's images, batching, and the GPU time of a busy frame at scale 4 (under
// 2 ms on the Mac). Further down: #88's present filters, and GPU presentation (#130, ADR 0068:
// the GL frame copied on the GPU and composed with an overlay, without a read-back).
//
// Local only. CI has no GPU: every case passes as "skipped" (the "gfx_gpu: skipped" line,
// which CTest's SKIP_REGULAR_EXPRESSION turns into a skip) when there is no GPU device, and on
// GitHub Actions. MAPLETHIEF_GPU_TESTS=1 makes a missing device a failure, =0 skips.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx_backend.h"
#include "gfx_prototype.h"
#include "golden.h"
#include "gpu_screen.h"
#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"
#include "maplethief/recomp/calls.h"
#include "remap.h"

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
namespace golden = maplethief::golden;
namespace proto = maplethief::gfx_prototype;

// ADR 0060's tolerance between the backends: a pixel differs when a channel is off by more
// than 2 (the GPU's own rounding of the modulation, the 8-bit bilinear weights and the blend),
// and at most 0.5% of the pixels may differ (edges: the GPU snaps vertices to its own
// sub-pixel grid and picks the filter per pixel).
constexpr golden::Tolerance k_tolerance{2, 0.005};

const char *env(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr ? v : "";
}

// The device, made once; nullptr: skip (see the file comment).
SDL_GPUDevice *device() {
    static SDL_GPUDevice *dev = [] {
        const std::string want = env("MAPLETHIEF_GPU_TESTS");
        if (want == "0" || (want != "1" && std::string(env("GITHUB_ACTIONS")) == "true")) {
            return static_cast<SDL_GPUDevice *>(nullptr);
        }
        SDL_GPUDevice *d = mt_gfx_gpu_create_device();
        if (d == nullptr && want == "1") {
            FAIL("MAPLETHIEF_GPU_TESTS=1 but there is no GPU device: " << SDL_GetError());
        }
        if (d != nullptr) {
            std::printf("gfx_gpu: device %s\n", SDL_GetGPUDeviceDriver(d));
        }
        return d;
    }();
    return dev;
}

#define REQUIRE_GPU()                                                                              \
    do {                                                                                           \
        if (device() == nullptr) {                                                                 \
            std::printf("gfx_gpu: skipped: no GPU device (CI, or MAPLETHIEF_GPU_TESTS=0)\n");      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

// Selects the backend for one rendering, from a clean mt_gfx.
void use(bool gpu) {
    mt_gfx_gpu_uninstall();
    mt_gfx_reset();
    if (gpu) {
        REQUIRE(mt_gfx_gpu_install(device()) == 0);
        REQUIRE(mt_gfx_gpu_installed() == 1);
    }
}

struct Fresh {
    ~Fresh() {
        mt_gfx_gpu_uninstall();
        mt_gfx_reset();
    }
};

struct TargetDeleter {
    void operator()(mt_gfx_target *t) const { mt_gfx_target_destroy(t); }
};
using Target = std::unique_ptr<mt_gfx_target, TargetDeleter>;

Target make_target(u32 w, u32 h, golden::Rgba fill = {0, 0, 0, 0}, u32 scale = 1) {
    Target t(mt_gfx_target_create(w, h, scale));
    REQUIRE(t != nullptr);
    const u8 rgba[4] = {fill.r, fill.g, fill.b, fill.a};
    mt_gfx_target_fill(t.get(), rgba);
    return t;
}

golden::Image to_image(const mt_gfx_target *t) {
    golden::Image img(static_cast<int>(mt_gfx_target_pixel_width(t)),
                      static_cast<int>(mt_gfx_target_pixel_height(t)));
    std::memcpy(img.bytes().data(), mt_gfx_target_cpixels(t), img.bytes().size());
    return img;
}

mt_gfx_vertex win(float x, float y, u32 w, u32 h, float s = 0, float t = 0) {
    return {x / static_cast<float>(w) * 2.0f - 1.0f,
            y / static_cast<float>(h) * 2.0f - 1.0f,
            0.0f,
            1.0f,
            s,
            t};
}

mt_gfx_draw draw_for(const mt_gfx_target *t) {
    mt_gfx_draw d = mt_gfx_draw_default();
    d.viewport[2] = static_cast<int32_t>(mt_gfx_target_width(t));
    d.viewport[3] = static_cast<int32_t>(mt_gfx_target_height(t));
    return d;
}

std::array<mt_gfx_vertex, 6> rect(float x0, float y0, float x1, float y1, u32 w, u32 h,
                                  float s0 = 0, float t0 = 0, float s1 = 1, float t1 = 1) {
    return {win(x0, y0, w, h, s0, t0), win(x1, y0, w, h, s1, t0), win(x1, y1, w, h, s1, t1),
            win(x0, y0, w, h, s0, t0), win(x1, y1, w, h, s1, t1), win(x0, y1, w, h, s0, t1)};
}

u32 make_texture(u32 w, u32 h, const std::vector<u8> &texels, u32 filter = MT_GFX_NEAREST,
                 u32 wrap = MT_GFX_REPEAT) {
    const u32 tex = mt_gfx_texture_create();
    REQUIRE(tex != 0);
    REQUIRE(mt_gfx_texture_image(tex, w, h, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER, filter) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MAG_FILTER, filter) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_S, wrap) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_T, wrap) == 0);
    return tex;
}

struct Lcg {
    u32 state = 12345;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    float unit() { return static_cast<float>(next() & 0xFFFF) / 65536.0f; }
};

const u32 k_factors[] = {MT_GFX_ZERO,
                         MT_GFX_ONE,
                         MT_GFX_SRC_COLOR,
                         MT_GFX_ONE_MINUS_SRC_COLOR,
                         MT_GFX_SRC_ALPHA,
                         MT_GFX_ONE_MINUS_SRC_ALPHA,
                         MT_GFX_DST_ALPHA,
                         MT_GFX_ONE_MINUS_DST_ALPHA,
                         MT_GFX_DST_COLOR,
                         MT_GFX_ONE_MINUS_DST_COLOR,
                         MT_GFX_SRC_ALPHA_SATURATE};

// --- the golden scenes of gfx_test.cpp, the same draws ------------------------------------------

using Scene = std::function<void(mt_gfx_target *)>;

struct SceneSpec {
    const char *golden; // tests/golden/data/<golden>.png, the software rasterizer's
    u32 w, h;
    golden::Rgba fill;
    Scene draw;
};

void blend_factors_scene(mt_gfx_target *t) {
    constexpr u32 tile = 16, n = 11, size = tile * n;
    const u32 dst_tex =
        make_texture(2, 2, {255, 0, 0, 255, 0, 255, 0, 64, 0, 0, 255, 192, 255, 255, 255, 0},
                     MT_GFX_LINEAR, MT_GFX_CLAMP_TO_EDGE);
    const u32 src_tex =
        make_texture(2, 2, {255, 255, 0, 0, 0, 255, 255, 255, 255, 0, 255, 128, 32, 32, 32, 224},
                     MT_GFX_LINEAR, MT_GFX_CLAMP_TO_EDGE);
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            const float x0 = static_cast<float>(col * tile), x1 = x0 + tile;
            const float y1 = static_cast<float>(size - row * tile), y0 = y1 - tile;
            mt_gfx_draw d = draw_for(t);
            d.texture = dst_tex;
            auto v = rect(x0, y0, x1, y1, size, size);
            d.vertices = v.data();
            d.count = 6;
            REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
            d.texture = src_tex;
            d.blend = 1;
            d.src_factor = k_factors[row];
            d.dst_factor = k_factors[col];
            v = rect(x0, y0, x1, y1, size, size, 1, 0, 0, 1);
            d.vertices = v.data();
            REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
        }
    }
}

void perspective_scene(mt_gfx_target *t) {
    std::vector<u8> checker;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool light = ((x ^ y) & 1u) == 0;
            checker.insert(checker.end(),
                           {static_cast<u8>(light ? 230 : 30), static_cast<u8>(light ? 200 : 60),
                            static_cast<u8>(light ? 90 : 160),
                            static_cast<u8>(x == 0 || y == 0 ? 128 : 255)});
        }
    }
    const float n = 0.5f, f = 50.0f, fy = 1.0f, fx = 0.75f;
    auto project = [&](float x, float y, float z, float s, float tc) {
        mt_gfx_vertex v;
        v.x = fx * x;
        v.y = fy * y;
        v.z = (f + n) / (n - f) * z + 2.0f * f * n / (n - f);
        v.w = -z;
        v.s = s;
        v.t = tc;
        return v;
    };
    for (int half = 0; half < 2; ++half) {
        const u32 tex =
            make_texture(8, 8, checker, half == 0 ? MT_GFX_NEAREST : MT_GFX_LINEAR, MT_GFX_REPEAT);
        const float xa = half == 0 ? -6.0f : 0.0f, xb = half == 0 ? 0.0f : 6.0f;
        const float sa = half == 0 ? 0.0f : 3.0f, sb = half == 0 ? 3.0f : 6.0f;
        const mt_gfx_vertex v[4] = {project(xa, -1, 2, sa, 0), project(xb, -1, 2, sb, 0),
                                    project(xa, -1, -20, sa, 11), project(xb, -1, -20, sb, 11)};
        mt_gfx_draw d = draw_for(t);
        d.primitive = MT_GFX_TRIANGLE_STRIP;
        d.vertices = v;
        d.count = 4;
        d.texture = tex;
        d.blend = 1;
        d.src_factor = MT_GFX_SRC_ALPHA;
        d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
    }
}

void lines_scene(mt_gfx_target *t) {
    constexpr u32 W = 96, H = 64;
    {
        u8 *p = mt_gfx_target_pixels(t);
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const u8 v = ((x / 8 + y / 8) & 1u) != 0 ? 40 : 24;
                u8 *q = p + (static_cast<std::size_t>(y) * W + x) * 4;
                q[0] = v;
                q[1] = v;
                q[2] = static_cast<u8>(v + 8);
                q[3] = 255;
            }
        }
    }
    auto submit = [&](u32 prim, const std::vector<mt_gfx_vertex> &v, mt_gfx_draw d) {
        d.primitive = prim;
        d.vertices = v.data();
        d.count = static_cast<u32>(v.size());
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
    };
    auto plain = [&](u8 r, u8 g, u8 b, u8 a = 255) {
        mt_gfx_draw d = draw_for(t);
        d.color[0] = r;
        d.color[1] = g;
        d.color[2] = b;
        d.color[3] = a;
        if (a != 255) {
            d.blend = 1;
            d.src_factor = MT_GFX_SRC_ALPHA;
            d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        }
        return d;
    };
    for (int k = 0; k < 32; ++k) {
        const double angle = k * 3.14159265358979323846 / 16.0;
        const float c = k % 2 == 0 ? 24.5f : 24.0f;
        const float ex = std::round(static_cast<float>(c + 21.0 * std::cos(angle)) * 16.0f) / 16.0f;
        const float ey = std::round(static_cast<float>(c + 21.0 * std::sin(angle)) * 16.0f) / 16.0f;
        submit(MT_GFX_LINES, {win(c, c + 7.5f, W, H), win(ex, ey + 7.5f, W, H)},
               plain(static_cast<u8>(128 + k * 4), static_cast<u8>(255 - k * 6),
                     static_cast<u8>(64 + k * 5)));
    }
    submit(MT_GFX_LINE_LOOP,
           {win(52, 4, W, H), win(92, 4, W, H), win(92, 28, W, H), win(52, 28, W, H)},
           plain(217, 145, 85, 254));
    submit(MT_GFX_LINE_STRIP,
           {win(54.5f, 8.5f, W, H), win(60.5f, 24.5f, W, H), win(66.25f, 8.75f, W, H),
            win(72.0f, 24.0f, W, H), win(78.5f, 8.5f, W, H), win(90.0f, 20.0625f, W, H)},
           plain(90, 200, 255, 128));
    {
        mt_gfx_draw d = plain(120, 40, 40);
        d.blend = 1;
        d.src_factor = MT_GFX_ONE;
        d.dst_factor = MT_GFX_ONE;
        submit(MT_GFX_LINES,
               {win(52, 34, W, H), win(92, 60, W, H), win(52, 60, W, H), win(92, 34, W, H),
                win(72.5f, 32.5f, W, H), win(72.5f, 61.5f, W, H)},
               d);
    }
    {
        std::vector<u8> ramp;
        for (int i = 0; i < 8; ++i) {
            ramp.insert(ramp.end(),
                        {static_cast<u8>(i * 36), 255, static_cast<u8>(255 - i * 36), 255});
        }
        mt_gfx_draw d = plain(255, 255, 255);
        d.texture = make_texture(8, 1, ramp, MT_GFX_LINEAR);
        submit(MT_GFX_LINES, {win(4.5f, 62.5f, W, H, 0, 0.5f), win(91.5f, 62.5f, W, H, 2, 0.5f)},
               d);
    }
    submit(MT_GFX_LINES,
           {win(-10, 2, W, H), win(40, 1.5f, W, H), win(95.5f, -6, W, H), win(95.5f, 70, W, H),
            win(30, 70, W, H), win(46, -4, W, H)},
           plain(255, 255, 0));
}

void prototype_scene(mt_gfx_target *t) { REQUIRE(proto::render(t)); }

// A busy map frame: a full-screen opaque background and 300 alpha-blended 32 x 32 sprites
// (gfx_test.cpp's "timing" case), with the logical size 480 x 320.
struct BusyFrame {
    u32 tex = 0;
    std::vector<std::array<mt_gfx_vertex, 6>> quads;
    std::array<mt_gfx_vertex, 6> bg = rect(0, 0, 480, 320, 480, 320);

    BusyFrame() {
        std::vector<u8> sprite(32 * 32 * 4);
        for (std::size_t i = 0; i < sprite.size(); ++i) {
            sprite[i] = static_cast<u8>(i * 7);
        }
        tex = make_texture(32, 32, sprite, MT_GFX_NEAREST, MT_GFX_CLAMP_TO_EDGE);
        Lcg rng;
        for (int i = 0; i < 300; ++i) {
            const float x = rng.unit() * 448.0f, y = rng.unit() * 288.0f;
            quads.push_back(rect(x, y, x + 32, y + 32, 480, 320));
        }
    }
    void draw(mt_gfx_target *t) const {
        mt_gfx_draw d = draw_for(t);
        d.texture = tex;
        d.vertices = bg.data();
        d.count = 6;
        mt_gfx_submit(t, &d);
        d.blend = 1;
        d.src_factor = MT_GFX_SRC_ALPHA;
        d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        for (const auto &q : quads) {
            d.vertices = q.data();
            mt_gfx_submit(t, &d);
        }
    }
};

// Renders a scene on one backend into a new w x h target at `scale`.
golden::Image render(bool gpu, u32 w, u32 h, golden::Rgba fill, u32 scale, const Scene &scene) {
    use(gpu);
    golden::Image img;
    {
        Target t = make_target(w, h, fill, scale);
        scene(t.get());
        mt_gfx_end_frame();
        img = to_image(t.get());
    }
    return img;
}

std::string describe(const golden::Comparison &c) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%zu of %zu pixels beyond +-%d (%.3f%%), max channel delta %d",
                  c.differing_pixels, c.total_pixels, k_tolerance.channel,
                  c.total_pixels != 0 ? 100.0 * static_cast<double>(c.differing_pixels) /
                                            static_cast<double>(c.total_pixels)
                                      : 0.0,
                  c.max_channel_delta);
    return buf;
}

void check_scene(const SceneSpec &s) {
    const std::filesystem::path path =
        std::filesystem::path(MT_GOLDEN_DATA_DIR) / (std::string(s.golden) + ".png");
    // The scene is the golden's: the software rasterizer draws it exactly.
    const golden::Image sw = render(false, s.w, s.h, s.fill, 1, s.draw);
    const auto exact = golden::check_golden(
        sw, path, std::string("gfx_gpu/") + s.golden + ".software", {}, golden::Update::Never);
    CHECK_MESSAGE(exact.passed, exact.message);
    // The GPU draws it within the tolerance.
    const golden::Image gpu = render(true, s.w, s.h, s.fill, 1, s.draw);
    const auto r = golden::check_golden(gpu, path, std::string("gfx_gpu/") + s.golden, k_tolerance,
                                        golden::Update::Never);
    std::printf("gfx_gpu: %s: %s\n", s.golden, describe(r.comparison).c_str());
    CHECK_MESSAGE(r.passed, r.message);
}

} // namespace

TEST_CASE("the GPU backend installs, names itself and uninstalls") {
    REQUIRE_GPU();
    Fresh fresh;
    CHECK(std::string(mt_gfx_backend_name()) == "software");
    use(true);
    const std::string name = mt_gfx_backend_name();
    CHECK(name.rfind("gpu (", 0) == 0);
    CHECK(mt_gfx_gpu_get_stats().pipelines >= 2); // made up front
    mt_gfx_gpu_uninstall();
    CHECK(mt_gfx_gpu_installed() == 0);
    CHECK(std::string(mt_gfx_backend_name()) == "software");
    // Every shader of the library loads on the device.
    for (const char *s : {"gfx.vert", "gfx.frag", "present_copy.frag"}) {
        SDL_GPUShader *shader = mt_gfx_gpu_create_shader(device(), s);
        CHECK_MESSAGE(shader != nullptr, s);
        if (shader != nullptr) {
            SDL_ReleaseGPUShader(device(), shader);
        }
    }
    CHECK(mt_gfx_gpu_create_shader(device(), "no.such") == nullptr);
}

TEST_CASE("golden scenes on the GPU: the prototype scene (gfx_quad_blend)") {
    REQUIRE_GPU();
    Fresh fresh;
    check_scene({"gfx_quad_blend", proto::kWidth, proto::kHeight, {}, prototype_scene});
}

TEST_CASE("golden scenes on the GPU: every blend factor pair (gfx_blend_factors)") {
    REQUIRE_GPU();
    Fresh fresh;
    check_scene({"gfx_blend_factors", 176, 176, {}, blend_factors_scene});
}

TEST_CASE("golden scenes on the GPU: the perspective floor (gfx_perspective)") {
    REQUIRE_GPU();
    Fresh fresh;
    check_scene({"gfx_perspective", 160, 120, {64, 64, 64, 255}, perspective_scene});
}

TEST_CASE("golden scenes on the GPU: lines (gfx_lines)") {
    REQUIRE_GPU();
    Fresh fresh;
    check_scene({"gfx_lines", 96, 64, {}, lines_scene});
}

TEST_CASE("internal scale 4: the prototype scene and a busy frame on both backends") {
    REQUIRE_GPU();
    Fresh fresh;
    const golden::Image sw = render(false, 480, 320, {}, 4, prototype_scene);
    const golden::Image gpu = render(true, 480, 320, {}, 4, prototype_scene);
    REQUIRE(sw.width() == 1920);
    const auto c = golden::compare(gpu, sw, k_tolerance);
    std::printf("gfx_gpu: prototype scene at scale 4: %s\n", describe(c).c_str());
    CHECK_MESSAGE(c.matches, describe(c));

    auto busy = [](mt_gfx_target *t) { BusyFrame().draw(t); };
    const golden::Image bsw = render(false, 480, 320, {0, 0, 0, 255}, 4, busy);
    const golden::Image bgpu = render(true, 480, 320, {0, 0, 0, 255}, 4, busy);
    const auto bc = golden::compare(bgpu, bsw, k_tolerance);
    std::printf("gfx_gpu: busy frame at scale 4: %s\n", describe(bc).c_str());
    CHECK_MESSAGE(bc.matches, describe(bc));
}

TEST_CASE("batching: a busy frame is two batches, and its GPU time at scale 4") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    mt_gfx_gpu_set_timing(1);
    BusyFrame frame;
    Target t = make_target(480, 320, {0, 0, 0, 255}, 4);
    // The first frame uploads the target and the texture and warms the pipelines up; the
    // others are what the game's frames cost.
    frame.draw(t.get());
    mt_gfx_end_frame();
    const mt_gfx_gpu_stats warm = mt_gfx_gpu_get_stats();
    CHECK(warm.batches == 2); // the opaque background, then 300 sprites with one blend state
    CHECK(warm.texture_uploads == 1);
    CHECK(warm.target_uploads == 1);
    constexpr int frames = 60;
    std::vector<double> ms;
    for (int i = 0; i < frames; ++i) {
        frame.draw(t.get());
        mt_gfx_end_frame();
        ms.push_back(static_cast<double>(mt_gfx_gpu_get_stats().last_frame_gpu_ns) / 1e6);
    }
    std::sort(ms.begin(), ms.end());
    const double best_ms = ms.front(), median_ms = ms[frames / 2], worst_ms = ms.back();
    const mt_gfx_gpu_stats st = mt_gfx_gpu_get_stats();
    CHECK(st.batches == warm.batches + 2u * static_cast<unsigned>(frames));
    CHECK(st.texture_uploads == 1); // the texture stays on the GPU
    CHECK(st.target_uploads == 1);
    CHECK(st.readbacks == 0); // nothing read the target
    // The backend's own median (what the frontend logs) is over the same 61 timed frames.
    CHECK(st.timed_frames == static_cast<unsigned>(frames) + 1u);
    CHECK(st.median_frame_gpu_ns > 0);
    std::printf("gfx_gpu: busy frame at scale 4 (1920x1280, 301 draws, 2 batches): GPU time "
                "%.3f ms a frame at best of %d, %.3f ms median, %.3f ms at most (fence-timed, "
                "upper bounds)\n",
                best_ms, frames, median_ms, worst_ms);
    // #120's acceptance criterion: under 2 ms of GPU time per frame on the Mac at N = 4.
    // SDL's GPU API has no timestamp queries, so each frame's time is the wall time from its
    // submit to its fence's wait returning: the GPU's work plus the time the GPU and the OS
    // took to get to it and to wake this thread. That extra time is never negative and grows
    // with the machine's load (median 3.23 ms at load average 17 against 0.57 ms alone), so
    // the check uses the best of the frames: every frame is the same work, and each reading is
    // an upper bound on it, so the smallest is the tightest. A real regression (more than
    // 2 ms of GPU work) still fails, because then every frame reads over 2 ms.
    CHECK(best_ms < 2.0);
}

TEST_CASE("sync points: CPU writes and reads between draws") {
    REQUIRE_GPU();
    Fresh fresh;
    auto scene = [](mt_gfx_target *t) {
        mt_gfx_draw d = draw_for(t);
        d.color[0] = 200;
        d.color[1] = 0;
        d.color[2] = 0;
        auto v = rect(0, 0, 32, 32, 64, 64);
        d.vertices = v.data();
        d.count = 6;
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
        // The CPU reads the draw's pixel back, then writes one of its own...
        const u8 *p = mt_gfx_target_cpixels(t);
        const std::size_t bottom_left = (static_cast<std::size_t>(63) * 64 + 0) * 4;
        CHECK(p[bottom_left] == 200);
        u8 *w = mt_gfx_target_pixels(t);
        w[0] = 1;
        w[1] = 2;
        w[2] = 3;
        w[3] = 4;
        // ...which the next draw keeps (it does not cover the top-left pixel).
        d.color[0] = 0;
        d.color[1] = 200;
        v = rect(32, 32, 64, 64, 64, 64);
        d.vertices = v.data();
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
        // A fill overwrites queued draws' results too; a draw after it lands on the fill.
        auto filled = make_target(8, 8, {9, 9, 9, 9});
        mt_gfx_draw e = draw_for(filled.get());
        auto ev = rect(0, 0, 4, 4, 8, 8);
        e.vertices = ev.data();
        e.count = 6;
        REQUIRE(mt_gfx_submit(filled.get(), &e) == MT_GFX_DRAWN);
        const u8 grey[4] = {7, 7, 7, 7};
        mt_gfx_target_fill(filled.get(), grey);
        REQUIRE(mt_gfx_submit(filled.get(), &e) == MT_GFX_DRAWN);
        const u8 *f = mt_gfx_target_cpixels(filled.get());
        CHECK(f[0] == 7);                                       // top left: the fill
        CHECK(f[(static_cast<std::size_t>(7) * 8) * 4] == 255); // bottom left: the draw
    };
    const golden::Image sw = render(false, 64, 64, {}, 1, scene);
    const golden::Image gpu = render(true, 64, 64, {}, 1, scene);
    CHECK(gpu == sw); // axis-aligned, untextured, unblended: exact
    CHECK(gpu.at(0, 0) == golden::Rgba{1, 2, 3, 4});
    CHECK(gpu.at(63, 0) == golden::Rgba{0, 200, 0, 255});
    CHECK(gpu.at(0, 63) == golden::Rgba{200, 0, 0, 255});
}

TEST_CASE("sync points: a texture re-uploaded or destroyed while draws that use it are queued") {
    REQUIRE_GPU();
    Fresh fresh;
    auto scene = [](mt_gfx_target *t) {
        const u32 tex = make_texture(1, 1, {255, 0, 0, 255});
        mt_gfx_draw d = draw_for(t);
        d.texture = tex;
        auto left = rect(0, 0, 16, 16, 32, 16);
        d.vertices = left.data();
        d.count = 6;
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
        // The same texture gets a new image: the queued draw still shows the old one.
        const u8 blue[4] = {0, 0, 255, 255};
        REQUIRE(mt_gfx_texture_image(tex, 1, 1, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, blue) == 0);
        auto right = rect(16, 0, 32, 16, 32, 16);
        d.vertices = right.data();
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
        // Destroyed with a draw queued: the draw happened before.
        mt_gfx_texture_destroy(tex);
    };
    const golden::Image sw = render(false, 32, 16, {}, 1, scene);
    const golden::Image gpu = render(true, 32, 16, {}, 1, scene);
    CHECK(gpu == sw);
    CHECK(gpu.at(0, 0) == golden::Rgba{255, 0, 0, 255});
    CHECK(gpu.at(31, 0) == golden::Rgba{0, 0, 255, 255});
    CHECK(mt_gfx_get_stats().textures == 0);
}

TEST_CASE("sync points: a target destroyed with draws queued, and switching backends mid-frame") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    {
        auto doomed = make_target(16, 16);
        auto kept = make_target(16, 16);
        mt_gfx_draw d = draw_for(doomed.get());
        auto v = rect(0, 0, 16, 16, 16, 16);
        d.vertices = v.data();
        d.count = 6;
        REQUIRE(mt_gfx_submit(doomed.get(), &d) == MT_GFX_DRAWN);
        d.color[1] = 0;
        REQUIRE(mt_gfx_submit(kept.get(), &d) == MT_GFX_DRAWN);
        doomed.reset(); // queued draws for it go, the other target's stay
        CHECK(to_image(kept.get()).at(5, 5) == golden::Rgba{255, 0, 255, 255});
    }
    // Half a frame on each backend: the result is the software rasterizer's (exact here).
    auto half = [](mt_gfx_target *t, int part) {
        mt_gfx_draw d = draw_for(t);
        d.color[0] = static_cast<u8>(part == 0 ? 255 : 0);
        d.color[3] = 128;
        d.blend = 1;
        d.src_factor = MT_GFX_ONE;
        d.dst_factor = MT_GFX_ONE;
        auto v =
            rect(static_cast<float>(part * 8), 0, static_cast<float>(part * 8 + 16), 16, 24, 16);
        d.vertices = v.data();
        d.count = 6;
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
    };
    use(false);
    auto sw = make_target(24, 16);
    half(sw.get(), 0);
    half(sw.get(), 1);
    use(true);
    auto mixed = make_target(24, 16);
    half(mixed.get(), 0);
    mt_gfx_gpu_uninstall(); // reads the GPU's half back
    CHECK(std::string(mt_gfx_backend_name()) == "software");
    half(mixed.get(), 1);
    REQUIRE(mt_gfx_gpu_install(device()) == 0); // and on to the GPU again
    CHECK(to_image(mixed.get()) == to_image(sw.get()));
}

namespace {

// A 2x replacement of every upload: each texel becomes 2 x 2.
int double_hook(void *, const mt_gfx_upload *up, mt_gfx_replacement *out) {
    static std::vector<u8> buf;
    if (up->format != MT_GFX_RGBA || up->type != MT_GFX_UNSIGNED_BYTE) {
        return 0;
    }
    const auto *src = static_cast<const u8 *>(up->pixels);
    const u32 w = up->width * 2, h = up->height * 2;
    buf.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u8 *s = src + (static_cast<std::size_t>(y / 2) * up->width + x / 2) * 4;
            std::memcpy(&buf[(static_cast<std::size_t>(y) * w + x) * 4], s, 4);
        }
    }
    out->pixels = buf.data();
    out->width = w;
    out->height = h;
    return 1;
}

} // namespace

TEST_CASE("texture replacement: the hook's images are what the GPU samples") {
    REQUIRE_GPU();
    Fresh fresh;
    mt_gfx_set_replace_hook(double_hook, nullptr);
    const golden::Image sw = render(false, proto::kWidth, proto::kHeight, {}, 2, prototype_scene);
    const golden::Image gpu = render(true, proto::kWidth, proto::kHeight, {}, 2, prototype_scene);
    mt_gfx_set_replace_hook(nullptr, nullptr);
    const auto c = golden::compare(gpu, sw, k_tolerance);
    std::printf("gfx_gpu: prototype scene, 2x replacements, scale 2: %s\n", describe(c).c_str());
    CHECK_MESSAGE(c.matches, describe(c));
    CHECK(mt_gfx_get_stats().replacements > 0); // the GPU run's uploads were replaced
}

TEST_CASE("texture_filter nearest: the GPU's samplers are nearest as the rasterizer's (#153)") {
    REQUIRE_GPU();
    Fresh fresh;
    REQUIRE(mt_gfx_set_texture_filter(MT_GFX_NEAREST) == 0);
    const golden::Image sw = render(false, proto::kWidth, proto::kHeight, {}, 2, prototype_scene);
    const golden::Image gpu = render(true, proto::kWidth, proto::kHeight, {}, 2, prototype_scene);
    REQUIRE(mt_gfx_set_texture_filter(0) == 0);
    const golden::Image linear =
        render(true, proto::kWidth, proto::kHeight, {}, 2, prototype_scene);
    const auto c = golden::compare(gpu, sw, k_tolerance);
    std::printf("gfx_gpu: prototype scene, texture_filter nearest, scale 2: %s\n",
                describe(c).c_str());
    CHECK_MESSAGE(c.matches, describe(c));
    CHECK_FALSE(gpu == linear); // the scene's bilinear sprite is nearest now
}

TEST_CASE("the core still counts and rejects on the GPU backend") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    auto t = make_target(8, 8);
    mt_gfx_draw d = draw_for(t.get());
    d.primitive = MT_GFX_POINTS;
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_UNSUPPORTED);
    d.primitive = MT_GFX_TRIANGLES;
    d.texture = 999;
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_INVALID);
    d.texture = 0;
    const mt_gfx_vertex v[4] = {win(0, 0, 8, 8), win(8, 0, 8, 8), win(0, 8, 8, 8), win(8, 8, 8, 8)};
    d.primitive = MT_GFX_TRIANGLE_STRIP;
    d.vertices = v;
    d.count = 4;
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
    const mt_gfx_stats st = mt_gfx_get_stats();
    CHECK(st.total.draws == 1);
    CHECK(st.total.triangles == 2);
    CHECK(st.total.unsupported == 1);
    CHECK(st.total.invalid == 1);
    CHECK(st.total.pixels == 0); // the GPU does not count them
    CHECK(to_image(t.get()).at(3, 3) == golden::Rgba{255, 255, 255, 255});
}

// The frontend library (the Presenter, below) links the boot, which calls the generated
// dispatcher; nothing here runs guest code.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t target, std::uint32_t) noexcept {
    std::fprintf(stderr, "gfx_gpu_test: mt_dispatch(0x%X) called\n", static_cast<unsigned>(target));
    std::abort();
}

// --- the present stage's shader filters (#88, ADR 0066) -------------------------------------
//
// Each filter is drawn the way the game draws it: the frontend's Presenter with the filter's
// PresentPass on SDL's GPU renderer. The renderer is offscreen (no window), on this test's
// device, drawing into a render-target texture of a window's drawable size.

namespace {

namespace fe = maplethief::frontend;
using fe::Rect;
using fe::ScaleMode;

constexpr fe::Color k_bars{0x10, 0x20, 0x30};

struct OffscreenPresent {
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *target = nullptr;
    fe::Presenter presenter;
    fe::PresentPass pass;
    int width, height;

    OffscreenPresent(int w, int h, fe::ShaderFilter filter, u32 image_w = 480, u32 image_h = 320,
                     ScaleMode mode = ScaleMode::kInteger)
        : width(w), height(h) {
        renderer = SDL_CreateGPURenderer(device(), nullptr);
        REQUIRE_MESSAGE(renderer != nullptr, SDL_GetError());
        target =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
        REQUIRE_MESSAGE(target != nullptr, SDL_GetError());
        REQUIRE(SDL_SetRenderTarget(renderer, target));
        REQUIRE(presenter.start(renderer, image_w, image_h));
        presenter.set_scaling(mode, k_bars);
        if (filter == fe::ShaderFilter::kNone) {
            REQUIRE(pass.open(renderer, "present_copy.frag"));
            presenter.set_render_state(pass.state());
        } else {
            REQUIRE(pass.open_filter(renderer, filter));
            presenter.set_stage(pass.stage());
        }
    }
    ~OffscreenPresent() {
        presenter.stop();
        pass.close();
        SDL_DestroyTexture(target);
        SDL_DestroyRenderer(renderer);
    }
    OffscreenPresent(const OffscreenPresent &) = delete;
    OffscreenPresent &operator=(const OffscreenPresent &) = delete;

    // Uploads and draws `img`; returns the drawn output (RGBA8, top row first).
    golden::Image draw(const golden::Image &img) {
        REQUIRE(presenter.upload(img.bytes().data(), static_cast<u32>(img.width()) * 4));
        REQUIRE(presenter.draw());
        SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
        REQUIRE_MESSAGE(read != nullptr, SDL_GetError());
        SDL_Surface *s = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(read);
        REQUIRE(s != nullptr);
        golden::Image out(s->w, s->h);
        for (int y = 0; y < s->h; y++) {
            std::memcpy(out.bytes().data() + static_cast<std::size_t>(y) * s->w * 4,
                        static_cast<const u8 *>(s->pixels) + static_cast<std::size_t>(y) * s->pitch,
                        static_cast<std::size_t>(s->w) * 4);
        }
        SDL_DestroySurface(s);
        return out;
    }
    long check(const golden::Image &img, std::string *report) {
        return presenter.check_readback(img.bytes().data(), static_cast<u32>(img.width()) * 4,
                                        report);
    }
    // The GPU time of drawing the uploaded image once: from the renderer's submit
    // (SDL_RenderPresent of an offscreen renderer) to a fence submitted after it on the same
    // device, which signals once the renderer's command buffer has run. Like ADR 0060's timing,
    // an upper bound (the submit's own recording and the wake-up are in it).
    // `draws` draws the image that many times (each a clear and the pass over the whole
    // output), to tell the pass's own time from the frame's fixed cost.
    double frame_ms(int draws = 1) {
        REQUIRE(SDL_SetRenderTarget(renderer, target));
        for (int i = 0; i < draws; i++) {
            REQUIRE(presenter.draw());
        }
        const Uint64 t0 = SDL_GetTicksNS();
        REQUIRE(SDL_RenderPresent(renderer));
        SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(device());
        REQUIRE(cb != nullptr);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        REQUIRE(fence != nullptr);
        REQUIRE(SDL_WaitForGPUFences(device(), true, &fence, 1));
        const Uint64 t1 = SDL_GetTicksNS();
        SDL_ReleaseGPUFence(device(), fence);
        return static_cast<double>(t1 - t0) / 1e6;
    }
};

golden::Image flat(int w, int h, golden::Rgba c) {
    golden::Image img(w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            img.set(x, y, c);
        }
    }
    return img;
}

bool same_rgb(golden::Rgba a, golden::Rgba b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

int luma(golden::Rgba c) { return (c.r * 299 + c.g * 587 + c.b * 114) / 1000; }

} // namespace

TEST_CASE("present filters: every filter's pass opens on SDL's GPU renderer, with its sizes") {
    REQUIRE_GPU();
    for (const fe::ShaderFilter f :
         {fe::ShaderFilter::kXbr, fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
        INFO(fe::shader_filter_name(f));
        OffscreenPresent o(1500, 1000, f);
        const fe::PresentStage st = o.pass.stage();
        CHECK(st.state != nullptr);
        CHECK(st.sizes);
        CHECK(st.check ==
              (f == fe::ShaderFilter::kXbr ? fe::PassCheck::kBlend : fe::PassCheck::kBarsOnly));
        // Bars in the border colour and the image inside, whatever the filter does to it.
        const golden::Image img = flat(480, 320, {200, 100, 50, 255});
        const golden::Image out = o.draw(img);
        std::string report;
        CHECK_MESSAGE(o.check(img, &report) == 0, report);
        CHECK(same_rgb(out.at(0, 0), golden::Rgba{0x10, 0x20, 0x30, 255}));
        CHECK(same_rgb(out.at(1499, 999), golden::Rgba{0x10, 0x20, 0x30, 255}));
        CHECK_FALSE(same_rgb(out.at(750, 500), golden::Rgba{0x10, 0x20, 0x30, 255}));
    }
}

// The safe area (#73, ADR 0055) on SDL's GPU renderer: forced insets place the image inside
// them through the identity pass and every filter's pass, as on the software renderer
// (safe_area_test.cpp), and the bars cover the strips.
TEST_CASE("present filters: forced safe-area insets on SDL's GPU renderer, every pass") {
    REQUIRE_GPU();
    golden::Image img(480, 320);
    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 480; x++) {
            img.set(x, y, {static_cast<u8>(x * 7 + 1), static_cast<u8>(y * 5 + 3), 120, 255});
        }
    }
    const fe::Insets in{40, 59, 21, 59};
    for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kXbr,
                                     fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
        for (const ScaleMode mode : {ScaleMode::kInteger, ScaleMode::kFit}) {
            INFO(fe::shader_filter_name(f) << ", " << fe::scale_mode_name(mode));
            OffscreenPresent o(1000, 700, f, 480, 320, mode);
            o.presenter.set_forced_insets(in);
            // The identity pass is checked pixel for pixel; a filter's image is flat.
            const golden::Image src =
                f == fe::ShaderFilter::kNone ? img : flat(480, 320, {200, 100, 50, 255});
            const golden::Image out = o.draw(src);
            CHECK(o.presenter.insets() == in);
            const fe::Rect &r = o.presenter.placement().rect;
            CHECK(r.x >= in.left);
            CHECK(r.y >= in.top);
            CHECK(r.x + r.w <= 1000 - in.right);
            CHECK(r.y + r.h <= 700 - in.bottom);
            std::string report;
            CHECK_MESSAGE(o.check(src, &report) == 0, report);
            CHECK(same_rgb(out.at(20, 350), golden::Rgba{0x10, 0x20, 0x30, 255})); // the strip
        }
    }
}

TEST_CASE("present filters: xbr smooths diagonal edges and leaves everything else exact") {
    REQUIRE_GPU();
    // A 480x320 image: black, with a white staircase (texel (x, y) white when x < y) in the
    // block [0, 64) x [0, 64), an isolated white texel at (200, 100), a checkerboard in
    // [300, 340) x [200, 240), and a flat grey area elsewhere at the right.
    golden::Image img = flat(480, 320, {0, 0, 0, 255});
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            img.set(x, y, x < y ? golden::Rgba{255, 255, 255, 255} : golden::Rgba{0, 0, 0, 255});
        }
    }
    img.set(200, 100, {255, 255, 255, 255});
    for (int y = 200; y < 240; y++) {
        for (int x = 300; x < 340; x++) {
            img.set(x, y,
                    (x + y) % 2 != 0 ? golden::Rgba{255, 0, 0, 255} : golden::Rgba{0, 0, 255, 255});
        }
    }
    for (int y = 0; y < 320; y++) {
        for (int x = 400; x < 480; x++) {
            img.set(x, y, {128, 128, 128, 255});
        }
    }
    OffscreenPresent o(1440, 960, fe::ShaderFilter::kXbr); // integer 3x, no bars
    const golden::Image out = o.draw(img);
    std::string report;
    CHECK_MESSAGE(o.check(img, &report) == 0, report); // every pixel a blend of its neighbours
    long smoothed = 0, off_diagonal = 0, corners = 0, elsewhere = 0;
    for (int oy = 0; oy < 960; oy++) {
        for (int ox = 0; ox < 1440; ox++) {
            const int tx = ox / 3, ty = oy / 3;
            if (same_rgb(out.at(ox, oy), img.at(tx, ty))) {
                continue;
            }
            if (tx < 64 && ty < 64) {
                smoothed++;
                off_diagonal += std::abs(tx - ty) > 1 ? 1 : 0;
            } else if ((tx == 300 || tx == 339) && (ty == 200 || ty == 239)) {
                corners++; // the checkerboard block's outer corners are rounded, as xBR does
            } else {
                elsewhere++;
            }
        }
    }
    std::printf("gfx_gpu: xbr at 3x: %ld pixels of the staircase smoothed, %ld away from its "
                "diagonal, %ld at the block's corners, %ld changed elsewhere\n",
                smoothed, off_diagonal, corners, elsewhere);
    CHECK(smoothed > 63 * 2); // at least a corner pixel on each side of every step
    CHECK(off_diagonal == 0);
    CHECK(corners <= 4 * 4);
    CHECK(elsewhere == 0); // the isolated texel, the checkerboard and the flat areas are exact
    // Texel centres are always the texel.
    for (int ty = 0; ty < 64; ty++) {
        for (int tx = 0; tx < 64; tx++) {
            CHECK(same_rgb(out.at(tx * 3 + 1, ty * 3 + 1), img.at(tx, ty)));
        }
    }
    // A fractional fit: still every pixel a blend of its neighbours, flat areas exact.
    OffscreenPresent fit(1001, 667, fe::ShaderFilter::kXbr, 480, 320, ScaleMode::kFit);
    const golden::Image fout = fit.draw(img);
    CHECK_MESSAGE(fit.check(img, &report) == 0, report);
    CHECK(same_rgb(fout.at(950, 300), golden::Rgba{128, 128, 128, 255}));
}

TEST_CASE("present filters: crt draws scanlines per source row and a 3-pixel aperture mask") {
    REQUIRE_GPU();
    const golden::Image img = flat(480, 320, {160, 160, 160, 255});
    OffscreenPresent o(2400, 1600, fe::ShaderFilter::kCrt); // integer 5x, no bars
    const golden::Image out = o.draw(img);
    std::string report;
    CHECK_MESSAGE(o.check(img, &report) == 0, report);
    // Down a column: brightest at each row's centre (output row 5 r + 2), darkest between rows.
    for (int r = 10; r < 20; r++) {
        INFO(r);
        const int centre = luma(out.at(600, 5 * r + 2));
        const int edge = luma(out.at(600, 5 * r));
        CHECK(centre > edge + 20);
        CHECK(luma(out.at(600, 5 * (r + 1) + 2)) == centre); // the same every row
    }
    // Along a row centre: red, green, blue columns in turn.
    for (int x = 300; x < 330; x += 3) {
        const golden::Rgba r = out.at(x, 52), g = out.at(x + 1, 52), b = out.at(x + 2, 52);
        CHECK(r.r > r.g);
        CHECK(g.g > g.r);
        CHECK(b.b > b.g);
        CHECK(same_rgb(out.at(x + 3, 52), r));
    }
    // About as bright as the image overall (the gain gives back the gaps and the mask).
    long sum = 0;
    for (int y = 500; y < 600; y++) {
        for (int x = 500; x < 600; x++) {
            sum += luma(out.at(x, y));
        }
    }
    const double mean = static_cast<double>(sum) / 10000.0;
    std::printf("gfx_gpu: crt at 5x on grey 160: mean output %.1f\n", mean);
    CHECK(mean > 110);
    CHECK(mean < 200);
}

TEST_CASE("present filters: lcd darkens the cell edges on texel boundaries") {
    REQUIRE_GPU();
    const golden::Image img = flat(480, 320, {200, 100, 50, 255});
    OffscreenPresent o(1440, 960, fe::ShaderFilter::kLcd); // integer 3x
    const golden::Image out = o.draw(img);
    std::string report;
    CHECK_MESSAGE(o.check(img, &report) == 0, report);
    for (int t = 50; t < 60; t++) {
        // Each 3x3 cell: its centre is the texel, its edge pixels darker, its corners darkest.
        const golden::Rgba centre = out.at(3 * t + 1, 3 * t + 1);
        const golden::Rgba side = out.at(3 * t, 3 * t + 1);
        const golden::Rgba corner = out.at(3 * t, 3 * t);
        CHECK(std::abs(centre.r - 200) <= 1);
        CHECK(std::abs(centre.g - 100) <= 1);
        CHECK(side.r < centre.r - 20);
        CHECK(corner.r <= side.r);
        CHECK(same_rgb(out.at(3 * t + 2, 3 * t + 1), side)); // symmetric
    }
}

TEST_CASE("present filters: GPU time per frame at 1080p, 1800p and 4K outputs (under 1 ms)") {
    REQUIRE_GPU();
    // A busy image (every texel its own colour), so no filter takes an early way out.
    golden::Image img(480, 320);
    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 480; x++) {
            img.set(x, y,
                    {static_cast<u8>(x * 7), static_cast<u8>(y * 13), static_cast<u8>((x ^ y) * 5),
                     255});
        }
    }
    struct Size {
        int w, h;
    };
    constexpr int frames = 60;
    constexpr int extra = 4;
    for (const Size size : {Size{1920, 1080}, Size{2880, 1800}, Size{3840, 2160}}) {
        for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kXbr,
                                         fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
            OffscreenPresent o(size.w, size.h, f, 480, 320, ScaleMode::kFit);
            o.draw(img); // uploads; warms the pipeline up
            o.frame_ms(1 + extra);
            std::vector<double> one, many;
            for (int i = 0; i < frames; i++) {
                one.push_back(o.frame_ms(1));
                many.push_back(o.frame_ms(1 + extra));
            }
            std::sort(one.begin(), one.end());
            std::sort(many.begin(), many.end());
            const double frame = one[frames / 2];
            // The pass's own time: what each further draw adds (a clear and the pass).
            const double pass = std::max(0.0, (many[frames / 2] - frame) / extra);
            const Rect &r = o.presenter.placement().rect;
            const char *name = f == fe::ShaderFilter::kNone ? "copy" : fe::shader_filter_name(f);
            std::printf("gfx_gpu: present %s at %dx%d (the image %gx%g): the frame %.3f ms "
                        "(median of %d, %.3f ms at most), the pass alone %.3f ms (fence-timed, "
                        "upper bounds)\n",
                        name, size.w, size.h, static_cast<double>(r.w), static_cast<double>(r.h),
                        frame, frames, one.back(), pass);
            // #88's acceptance criterion, a filter under 1 ms a frame on the target hardware (the
            // median, as ADR 0060's bound), on the pass's own time at every size. The frame's
            // fixed part (the submit, the fence's wake-up: the copy pass's frame minus its pass)
            // is the same without a filter and follows the machine's load, not the filter.
            CHECK(pass < 1.0);
        }
    }
}

// The border (#157, ADR 0085) on SDL's GPU renderer: the blur's chain of render targets under
// every filter's pass, at 16:9 and 21:9, and what it adds to a frame's GPU time at 1080p.
TEST_CASE("present border: blur on SDL's GPU renderer under every filter, 16:9 and 21:9") {
    REQUIRE_GPU();
    // Left half red, right half blue.
    golden::Image img(480, 320);
    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 480; x++) {
            img.set(x, y, x < 240 ? golden::Rgba{255, 0, 0, 255} : golden::Rgba{0, 0, 255, 255});
        }
    }
    const int dim = fe::kBlurDim;
    struct Size {
        int w, h;
    };
    for (const Size size : {Size{1920, 1080}, Size{2560, 1080}}) {
        for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kXbr,
                                         fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
            for (const ScaleMode mode :
                 {ScaleMode::kInteger, ScaleMode::kFit, ScaleMode::kSmooth, ScaleMode::kSharp}) {
                INFO(fe::shader_filter_name(f)
                     << ", " << fe::scale_mode_name(mode) << " at " << size.w << "x" << size.h);
                OffscreenPresent o(size.w, size.h, f, 480, 320, mode);
                o.presenter.set_border(fe::BorderMode::kBlur);
                const golden::Image out = o.draw(img);
                CHECK(o.presenter.border().drawn() == fe::BorderMode::kBlur);
                CHECK(SDL_GetRenderTarget(o.renderer) == o.target);
                std::string report;
                CHECK_MESSAGE(o.check(img, &report) == 0, report);
                const Rect &r = o.presenter.placement().rect;
                const int lx = static_cast<int>(r.x) / 2;
                const int rx =
                    static_cast<int>(r.x + r.w) + (size.w - static_cast<int>(r.x + r.w)) / 2;
                const golden::Rgba left = out.at(lx, size.h / 2);
                const golden::Rgba right = out.at(rx, size.h / 2);
                CHECK(std::abs(left.r - dim) <= 3);
                CHECK(left.b <= 3);
                CHECK(std::abs(right.b - dim) <= 3);
                CHECK(right.r <= 3);
                // The image's middle row is the identity's (or the filter's), not the blur.
                if (f == fe::ShaderFilter::kNone) {
                    CHECK(same_rgb(out.at(static_cast<int>(r.x) + 2, size.h / 2),
                                   golden::Rgba{255, 0, 0, 255}));
                }
            }
        }
    }
}

TEST_CASE("present border: the blur's GPU time at 1080p and 21:9 (under 1 ms a frame)") {
    REQUIRE_GPU();
    golden::Image img(480, 320);
    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 480; x++) {
            img.set(x, y,
                    {static_cast<u8>(x * 7), static_cast<u8>(y * 13), static_cast<u8>((x ^ y) * 5),
                     255});
        }
    }
    struct Size {
        int w, h;
    };
    constexpr int frames = 60;
    constexpr int extra = 4;
    for (const Size size : {Size{1920, 1080}, Size{2560, 1080}, Size{3840, 2160}}) {
        for (const int scale : {1, 3}) {
            golden::Image src(480 * scale, 320 * scale);
            for (int y = 0; y < src.height(); y++) {
                for (int x = 0; x < src.width(); x++) {
                    src.set(x, y, img.at(x / scale, y / scale));
                }
            }
            double pass[2] = {};
            double frame[2] = {};
            for (const fe::BorderMode b : {fe::BorderMode::kColor, fe::BorderMode::kBlur}) {
                OffscreenPresent o(size.w, size.h, fe::ShaderFilter::kNone,
                                   static_cast<u32>(src.width()), static_cast<u32>(src.height()),
                                   ScaleMode::kFit);
                o.presenter.set_border(b);
                o.draw(src); // uploads; warms the pipelines up
                o.frame_ms(1 + extra);
                std::vector<double> one, many;
                for (int i = 0; i < frames; i++) {
                    one.push_back(o.frame_ms(1));
                    many.push_back(o.frame_ms(1 + extra));
                }
                std::sort(one.begin(), one.end());
                std::sort(many.begin(), many.end());
                const int k = b == fe::BorderMode::kBlur ? 1 : 0;
                frame[k] = one[frames / 2];
                pass[k] = std::max(0.0, (many[frames / 2] - frame[k]) / extra);
            }
            // What the blur adds to a draw of the image (its chain and the stretch): the draw
            // with it less the draw without (each a clear and the copy pass over the output).
            const double blur = std::max(0.0, pass[1] - pass[0]);
            std::printf("gfx_gpu: border blur at %dx%d, render scale %d: a draw %.3f ms with it, "
                        "%.3f ms without, the blur alone %.3f ms; the frame %.3f / %.3f ms "
                        "(median of %d, fence-timed, upper bounds)\n",
                        size.w, size.h, scale, pass[1], pass[0], blur, frame[1], frame[0], frames);
            // #157's criterion: the blur under 1 ms a frame on the Mac at 1080p (and here at
            // 21:9 and 4K, at render scales 1 and 3).
            CHECK(blur < 1.0);
        }
    }
}

// The colour-blind filter (#158, ADR 0093) on SDL's GPU renderer: present_colorblind.frag into
// an image-sized target, the first step of the list, under every shader filter's pass and the
// border's blur; what it adds to a frame's GPU time.
namespace {

constexpr fe::ColorblindFilter k_colorblind[] = {fe::ColorblindFilter::kProtanopia,
                                                 fe::ColorblindFilter::kDeuteranopia,
                                                 fe::ColorblindFilter::kTritanopia};

golden::Image busy_image(int w, int h, int scale = 1) {
    golden::Image img(w * scale, h * scale);
    for (int y = 0; y < h * scale; y++) {
        for (int x = 0; x < w * scale; x++) {
            const int tx = x / scale;
            const int ty = y / scale;
            img.set(x, y,
                    {static_cast<u8>(tx * 7), static_cast<u8>(ty * 13),
                     static_cast<u8>((tx ^ ty) * 5), 255});
        }
    }
    return img;
}

} // namespace

TEST_CASE("present colorblind: the GPU way under every filter's pass and the blur, every mode") {
    REQUIRE_GPU();
    const golden::Image img = busy_image(480, 320);
    struct Size {
        int w, h;
    };
    for (const fe::ColorblindFilter cb : k_colorblind) {
        fe::ColorTransform cpu;
        cpu.set(cb);
        golden::Image ref(480, 320);
        cpu.apply(img.bytes().data(), 480 * 4, ref.bytes().data(), 480 * 4, 480, 320);
        for (const Size size : {Size{1920, 1080}, Size{2560, 1080}}) {
            for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kXbr,
                                             fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
                for (const ScaleMode mode : {ScaleMode::kInteger, ScaleMode::kFit,
                                             ScaleMode::kSmooth, ScaleMode::kSharp}) {
                    INFO(fe::colorblind_filter_name(cb)
                         << " under " << fe::shader_filter_name(f) << ", "
                         << fe::scale_mode_name(mode) << " at " << size.w << "x" << size.h);
                    OffscreenPresent o(size.w, size.h, f, 480, 320, mode);
                    o.presenter.set_colorblind(cb);
                    const golden::Image out = o.draw(img);
                    CHECK(o.presenter.color_path() == fe::Presenter::ColorPath::kGpu);
                    CHECK(o.presenter.color_drawn() == cb);
                    CHECK(SDL_GetRenderTarget(o.renderer) == o.target);
                    std::string report;
                    CHECK_MESSAGE(o.check(img, &report) == 0, report);
                    CHECK(report.find("on the GPU") != std::string::npos);
                    const Rect &r = o.presenter.placement().rect;
                    if (f == fe::ShaderFilter::kNone && mode == ScaleMode::kInteger) {
                        // Each texel's centre is the CPU way's colour within 2.
                        const int k = o.presenter.placement().integer_scale;
                        int worst = 0;
                        for (int ty = 0; ty < 320; ty += 3) {
                            for (int tx = 0; tx < 480; tx += 3) {
                                const golden::Rgba g =
                                    out.at(static_cast<int>(r.x) + tx * k + k / 2,
                                           static_cast<int>(r.y) + ty * k + k / 2);
                                const golden::Rgba e = ref.at(tx, ty);
                                worst = std::max({worst, std::abs(g.r - e.r), std::abs(g.g - e.g),
                                                  std::abs(g.b - e.b)});
                            }
                        }
                        CHECK(worst <= 2);
                    }
                    // The bars keep the border colour (the filter is the picture's only).
                    CHECK(same_rgb(out.at(0, 0), golden::Rgba{0x10, 0x20, 0x30, 255}));
                    // Under the blur: the bars read the filtered image, and the picture passes.
                    o.presenter.set_border(fe::BorderMode::kBlur);
                    o.draw(img);
                    CHECK(o.presenter.border().drawn() == fe::BorderMode::kBlur);
                    CHECK_MESSAGE(o.check(img, &report) == 0, report);
                    // Off again: the identity's image, the target freed by nothing (kept).
                    o.presenter.set_border(fe::BorderMode::kColor);
                    o.presenter.set_colorblind(fe::ColorblindFilter::kOff);
                    o.draw(img);
                    CHECK(o.presenter.color_path() == fe::Presenter::ColorPath::kNone);
                    CHECK_MESSAGE(o.check(img, &report) == 0, report);
                }
            }
        }
    }
}

// The picture adjustments (#187, ADR 0093's addendum) on SDL's GPU renderer: the same pass with
// the adjustments folded into its matrix, offset and gamma, alone and with the colour-blind
// filter in one pass; check_readback (+-2 against the CPU way) under a shader filter's pass.
TEST_CASE("present grade: the picture adjustments on the GPU way, with and without the filter") {
    REQUIRE_GPU();
    const golden::Image img = busy_image(480, 320);
    const fe::ColorGrade grades[] = {
        {fe::ColorblindFilter::kOff, 150, 100, 100, 100},
        {fe::ColorblindFilter::kOff, 100, 50, 150, 100},
        {fe::ColorblindFilter::kOff, 100, 100, 100, 140},
        {fe::ColorblindFilter::kOff, 50, 150, 50, 80},
        {fe::ColorblindFilter::kDeuteranopia, 120, 130, 70, 115},
        {fe::ColorblindFilter::kProtanopia, 150, 150, 150, 140},
    };
    for (const fe::ColorGrade &g : grades) {
        fe::ColorTransform cpu;
        cpu.set(g);
        golden::Image ref(480, 320);
        cpu.apply(img.bytes().data(), 480 * 4, ref.bytes().data(), 480 * 4, 480, 320);
        for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kCrt}) {
            for (const ScaleMode mode : {ScaleMode::kInteger, ScaleMode::kSmooth}) {
                INFO(fe::color_grade_text(g) << " under " << fe::shader_filter_name(f) << ", "
                                             << fe::scale_mode_name(mode));
                OffscreenPresent o(1920, 1080, f, 480, 320, mode);
                o.presenter.set_grade(g);
                const golden::Image out = o.draw(img);
                CHECK(o.presenter.color_path() == fe::Presenter::ColorPath::kGpu);
                CHECK(o.presenter.grade_drawn() == g);
                std::string report;
                CHECK_MESSAGE(o.check(img, &report) == 0, report);
                CHECK(report.find("on the GPU, +-2") != std::string::npos);
                if (f == fe::ShaderFilter::kNone && mode == ScaleMode::kInteger) {
                    const Rect &r = o.presenter.placement().rect;
                    const int k = o.presenter.placement().integer_scale;
                    int worst = 0;
                    for (int ty = 0; ty < 320; ty += 3) {
                        for (int tx = 0; tx < 480; tx += 3) {
                            const golden::Rgba a = out.at(static_cast<int>(r.x) + tx * k + k / 2,
                                                          static_cast<int>(r.y) + ty * k + k / 2);
                            const golden::Rgba e = ref.at(tx, ty);
                            worst = std::max({worst, std::abs(a.r - e.r), std::abs(a.g - e.g),
                                              std::abs(a.b - e.b)});
                        }
                    }
                    CHECK(worst <= 2);
                }
                // Back to the defaults: no step.
                o.presenter.set_grade(fe::ColorGrade{});
                o.draw(img);
                CHECK(o.presenter.color_path() == fe::Presenter::ColorPath::kNone);
                CHECK_MESSAGE(o.check(img, &report) == 0, report);
            }
        }
    }
}

TEST_CASE(
    "present colorblind: GPU time at 1080p, 21:9 and 4K, render scales 1 and 3 (under 1 ms)") {
    REQUIRE_GPU();
    struct Size {
        int w, h;
    };
    constexpr int frames = 60;
    constexpr int extra = 4;
    for (const Size size : {Size{1920, 1080}, Size{2560, 1080}, Size{3840, 2160}}) {
        for (const int scale : {1, 3}) {
            const golden::Image src = busy_image(480, 320, scale);
            for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kCrt}) {
                double pass[2] = {};
                double frame[2] = {};
                for (const bool on : {false, true}) {
                    OffscreenPresent o(size.w, size.h, f, static_cast<u32>(src.width()),
                                       static_cast<u32>(src.height()), ScaleMode::kFit);
                    o.presenter.set_colorblind(on ? fe::ColorblindFilter::kDeuteranopia
                                                  : fe::ColorblindFilter::kOff);
                    o.draw(src); // uploads; warms the pipelines up
                    o.frame_ms(1 + extra);
                    std::vector<double> one, many;
                    for (int i = 0; i < frames; i++) {
                        one.push_back(o.frame_ms(1));
                        many.push_back(o.frame_ms(1 + extra));
                    }
                    std::sort(one.begin(), one.end());
                    std::sort(many.begin(), many.end());
                    const int k = on ? 1 : 0;
                    frame[k] = one[frames / 2];
                    pass[k] = std::max(0.0, (many[frames / 2] - frame[k]) / extra);
                }
                // What the filter adds to a draw of the image: its pass into the image-sized
                // target (and the target switch), the draw with it less the draw without.
                const double added = std::max(0.0, pass[1] - pass[0]);
                std::printf("gfx_gpu: colour-blind filter at %dx%d, render scale %d, under %s: a "
                            "draw %.3f ms with it, %.3f ms without, the filter alone %.3f ms; the "
                            "frame %.3f / %.3f ms (median of %d, fence-timed, upper bounds)\n",
                            size.w, size.h, scale, f == fe::ShaderFilter::kNone ? "copy" : "crt",
                            pass[1], pass[0], added, frame[1], frame[0], frames);
                // #158's criterion: under 1 ms a frame on the Mac at 1080p (and here at 21:9
                // and 4K, render scales 1 and 3, with and without a shader filter).
                CHECK(added < 1.0);
            }
        }
    }
}

// Widescreen's wide picture (#170, ADR 0090) on SDL's GPU renderer: the wings, the screen image
// and the key pad's strips composed into one picture, which the colour-blind grade and every
// filter's pass then draw as one image (no seam at the wings' edges); what it costs a frame.
namespace {

constexpr fe::ShaderFilter k_filters[] = {fe::ShaderFilter::kNone, fe::ShaderFilter::kXbr,
                                          fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd};
constexpr ScaleMode k_modes[] = {ScaleMode::kInteger, ScaleMode::kFit, ScaleMode::kSmooth,
                                 ScaleMode::kSharp};

// A continuous 568-point frame at render scale k (the GL frame) and the screen image cut from
// its middle 480 points.
struct WideImages {
    golden::Image whole, screen;
};
WideImages wide_images(int k) {
    WideImages w{busy_image(568, 320, k), golden::Image(480 * k, 320 * k)};
    for (int y = 0; y < 320 * k; y++) {
        std::memcpy(w.screen.bytes().data() + static_cast<std::size_t>(y) * 480 * k * 4,
                    w.whole.bytes().data() + (static_cast<std::size_t>(y) * 568 * k + 44 * k) * 4,
                    static_cast<std::size_t>(480 * k) * 4);
    }
    return w;
}

// `img` drawn once into `r` over the bars, through the Presenter's pass (its sizes for `img`) or
// SDL's shader, with the placement's filter: the pass over one continuous picture. With `cb`,
// `img` is first graded through present_colorblind.frag into a target of its size, as the
// Presenter grades its picture.
golden::Image one_image(OffscreenPresent &o, const golden::Image &img, const Rect &r,
                        fe::ColorblindFilter cb = fe::ColorblindFilter::kOff) {
    SDL_Texture *t = SDL_CreateTexture(o.renderer, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STATIC, img.width(), img.height());
    REQUIRE_MESSAGE(t != nullptr, SDL_GetError());
    REQUIRE(SDL_UpdateTexture(t, nullptr, img.bytes().data(), img.width() * 4));
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    SDL_Texture *graded = nullptr;
    fe::ColorblindPass pass;
    if (cb != fe::ColorblindFilter::kOff) {
        graded = SDL_CreateTexture(o.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET,
                                   img.width(), img.height());
        REQUIRE(graded != nullptr);
        SDL_SetTextureBlendMode(graded, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
        REQUIRE(pass.open(o.renderer));
        REQUIRE(
            pass.set_uniforms(cb, static_cast<u32>(img.width()), static_cast<u32>(img.height())));
        REQUIRE(SDL_SetRenderTarget(o.renderer, graded));
        REQUIRE(SDL_SetGPURenderState(o.renderer, pass.state()));
        REQUIRE(SDL_RenderTexture(o.renderer, t, nullptr, nullptr));
        SDL_SetGPURenderState(o.renderer, nullptr);
        SDL_DestroyTexture(t);
        t = graded;
    }
    const fe::Filter f = o.presenter.placement().filter;
    SDL_SetTextureScaleMode(t, f == fe::Filter::kLinear     ? SDL_SCALEMODE_LINEAR
                               : f == fe::Filter::kPixelArt ? SDL_SCALEMODE_PIXELART
                                                            : SDL_SCALEMODE_NEAREST);
    REQUIRE(SDL_SetRenderTarget(o.renderer, o.target));
    SDL_SetRenderDrawColor(o.renderer, k_bars.r, k_bars.g, k_bars.b, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(o.renderer);
    const fe::PresentStage st = o.presenter.stage();
    const SDL_FRect dst{r.x, r.y, r.w, r.h};
    if (st.state != nullptr && st.sizes) {
        const float w = static_cast<float>(img.width());
        const float h = static_cast<float>(img.height());
        const fe::PresentSizes sizes{{w, h, 1.0f / w, 1.0f / h},
                                     {dst.w, dst.h, 1.0f / dst.w, 1.0f / dst.h}};
        REQUIRE(SDL_SetGPURenderStateFragmentUniforms(st.state, 0, &sizes, sizeof sizes));
    }
    if (st.state != nullptr) {
        REQUIRE(SDL_SetGPURenderState(o.renderer, st.state));
    }
    REQUIRE(SDL_RenderTexture(o.renderer, t, nullptr, &dst));
    SDL_SetGPURenderState(o.renderer, nullptr);
    SDL_Surface *read = SDL_RenderReadPixels(o.renderer, nullptr);
    REQUIRE_MESSAGE(read != nullptr, SDL_GetError());
    SDL_Surface *s = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(read);
    REQUIRE(s != nullptr);
    golden::Image out(s->w, s->h);
    for (int y = 0; y < s->h; y++) {
        std::memcpy(out.bytes().data() + static_cast<std::size_t>(y) * s->w * 4,
                    static_cast<const u8 *>(s->pixels) + static_cast<std::size_t>(y) * s->pitch,
                    static_cast<std::size_t>(s->w) * 4);
    }
    SDL_DestroySurface(s);
    SDL_DestroyTexture(t);
    pass.close();
    return out;
}

int max_delta(const golden::Image &a, const golden::Image &b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.bytes().size(); i++) {
        if (i % 4 != 3) {
            worst = std::max(worst, std::abs(a.bytes()[i] - b.bytes()[i]));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("present widescreen: the wide picture through every pass and the grade is one image") {
    REQUIRE_GPU();
    struct Size {
        int w, h;
    };
    int compared = 0;
    for (const int k : {1, 3}) {
        const WideImages wi = wide_images(k);
        const auto uk = static_cast<u32>(k);
        for (const Size size : {Size{1920, 1080}, Size{2560, 1080}}) {
            for (const fe::ShaderFilter f : k_filters) {
                for (const ScaleMode mode : k_modes) {
                    for (const fe::ColorblindFilter cb :
                         {fe::ColorblindFilter::kOff, fe::ColorblindFilter::kDeuteranopia}) {
                        INFO(std::string(fe::shader_filter_name(f))
                             << ", " << std::string(fe::scale_mode_name(mode)) << ", "
                             << std::string(fe::colorblind_filter_name(cb)) << " at " << size.w
                             << "x" << size.h << ", render scale " << k);
                        OffscreenPresent o(size.w, size.h, f, 480 * uk, 320 * uk, mode);
                        o.presenter.set_colorblind(cb);
                        o.presenter.set_wide(568);
                        REQUIRE(o.presenter.upload_wings(wi.whole.bytes().data(), 568 * uk,
                                                         320 * uk, 568));
                        const golden::Image out = o.draw(wi.screen);
                        REQUIRE(o.presenter.picture_drawn());
                        std::string report;
                        CHECK_MESSAGE(o.check(wi.screen, &report) == 0, report);
                        // The same frame as one picture (graded on the CPU as a whole) through
                        // the same pass into the same rectangle.
                        if (cb != fe::ColorblindFilter::kOff) {
                            CHECK(o.presenter.color_path() == fe::Presenter::ColorPath::kGpu);
                        }
                        const golden::Image one =
                            one_image(o, wi.whole, o.presenter.picture_rect(), cb);
                        // Byte for byte: one image, no seam.
                        CHECK(max_delta(out, one) == 0);
                        compared++;
                    }
                }
            }
        }
    }
    std::printf(
        "gfx_gpu: widescreen's wide picture: %d draws (every pass, scale mode, colour-blind "
        "off and on, render scales 1 and 3), each byte for byte the one-picture draw\n",
        compared);
}

TEST_CASE("present widescreen: the key pad's strips and a narrow view in the wide picture") {
    REQUIRE_GPU();
    // 520 of 568 points drawn; keys in the 20 points next to the screen image.
    const golden::Image gl = busy_image(568, 320);
    const golden::Image screen = busy_image(480, 320);
    std::vector<u8> strips(88 * 320 * 4, 0);
    for (int y = 100; y < 200; y++) {
        for (int x = 26; x < 62; x++) {
            const unsigned a = x == 26 || x == 61 ? 128 : 255;
            u8 *q = &strips[(static_cast<std::size_t>(y) * 88 + x) * 4];
            q[0] = static_cast<u8>(a * 220 / 255);
            q[1] = static_cast<u8>(a * 40 / 255);
            q[2] = static_cast<u8>(a * 30 / 255);
            q[3] = static_cast<u8>(a);
        }
    }
    for (const fe::ShaderFilter f : k_filters) {
        for (const fe::ColorblindFilter cb :
             {fe::ColorblindFilter::kOff, fe::ColorblindFilter::kTritanopia}) {
            INFO(std::string(fe::shader_filter_name(f))
                 << ", " << std::string(fe::colorblind_filter_name(cb)));
            OffscreenPresent o(1920, 1080, f);
            o.presenter.set_colorblind(cb);
            o.presenter.set_wide(568);
            REQUIRE(o.presenter.upload_wings(gl.bytes().data(), 568, 320, 520));
            REQUIRE(o.presenter.upload_pad_wings(strips.data(), 88, 320));
            o.draw(screen);
            REQUIRE(o.presenter.picture_drawn());
            CHECK(o.presenter.picture_width() == 520);
            std::string report;
            CHECK_MESSAGE(o.check(screen, &report) == 0, report);
            CHECK(report.find("wings checked against the GL frame") != std::string::npos);
        }
    }
}

TEST_CASE("present widescreen: the wide picture's GPU time at 1080p and 1800p (under 1 ms)") {
    REQUIRE_GPU();
    struct Size {
        int w, h;
    };
    constexpr int frames = 60;
    constexpr int extra = 4;
    for (const Size size : {Size{1920, 1080}, Size{2880, 1800}}) {
        for (const int k : {1, 3}) {
            const WideImages wi = wide_images(k);
            const auto uk = static_cast<u32>(k);
            for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kCrt}) {
                for (const fe::ColorblindFilter cb :
                     {fe::ColorblindFilter::kOff, fe::ColorblindFilter::kDeuteranopia}) {
                    double pass[2] = {};
                    double frame[2] = {};
                    for (const bool wings : {false, true}) {
                        OffscreenPresent o(size.w, size.h, f, 480 * uk, 320 * uk, ScaleMode::kFit);
                        o.presenter.set_colorblind(cb);
                        o.presenter.set_wide(568);
                        // Without wings: #86's wide placement with the bars beside the image.
                        REQUIRE(o.presenter.upload_wings(wi.whole.bytes().data(), 568 * uk,
                                                         320 * uk, wings ? 568 : 480));
                        o.draw(wi.screen); // uploads; warms the pipelines up
                        CHECK(o.presenter.picture_drawn() == wings);
                        o.frame_ms(1 + extra);
                        std::vector<double> one, many;
                        for (int i = 0; i < frames; i++) {
                            one.push_back(o.frame_ms(1));
                            many.push_back(o.frame_ms(1 + extra));
                        }
                        std::sort(one.begin(), one.end());
                        std::sort(many.begin(), many.end());
                        const int j = wings ? 1 : 0;
                        frame[j] = one[frames / 2];
                        pass[j] = std::max(0.0, (many[frames / 2] - frame[j]) / extra);
                    }
                    // What composing the wide picture adds to a draw: the composition (the GL
                    // frame's columns and the screen image into the picture), the grade and the
                    // pass over 568 points instead of 480.
                    const double added = std::max(0.0, pass[1] - pass[0]);
                    std::printf("gfx_gpu: widescreen's wide picture at %dx%d, render scale %d, "
                                "%s, colour-blind %s: a draw %.3f ms with it, %.3f ms without "
                                "wings, the picture alone %.3f ms; the frame %.3f / %.3f ms "
                                "(median of %d, fence-timed, upper bounds)\n",
                                size.w, size.h, k, f == fe::ShaderFilter::kNone ? "copy" : "crt",
                                fe::colorblind_filter_name(cb), pass[1], pass[0], added, frame[1],
                                frame[0], frames);
                    CHECK(added < 1.0);
                }
            }
        }
    }
}

// --- GPU presentation (#130, ADR 0068) ---------------------------------------------------------
//
// The GL frame stays on the GPU: GpuScreen copies the target's GPU texture at the present and
// composes it with an overlay (UIKit's views, transparent in the GL slot) into the Presenter's
// screen texture, on an offscreen SDL GPU renderer on the test's device. Nothing of mt_gfx is
// read back: the stats' readbacks stay 0 until the test itself asks for the CPU's pixels.

namespace {

// The CPU's reference: the FULL composite of the same layers. Opaque black; the GL frame's
// colour in its rectangle (whole pixels here); the premultiplied overlay over it, rounded as the
// compositor rounds: s + (d * (255 - a) + 127) / 255.
golden::Image reference(const golden::Image &gl, const golden::Image &overlay, int rx, int ry) {
    golden::Image out(overlay.width(), overlay.height());
    for (int y = 0; y < out.height(); y++) {
        for (int x = 0; x < out.width(); x++) {
            golden::Rgba under{0, 0, 0, 255};
            if (x >= rx && y >= ry && x < rx + gl.width() && y < ry + gl.height()) {
                under = gl.at(x - rx, y - ry);
            }
            const golden::Rgba o = overlay.at(x, y);
            const auto over = [&](u8 s, u8 d) {
                return static_cast<u8>(std::min(255, s + (d * (255 - o.a) + 127) / 255));
            };
            out.set(x, y, {over(o.r, under.r), over(o.g, under.g), over(o.b, under.b), 255});
        }
    }
    return out;
}

golden::Image from_bytes(const std::vector<u8> &rgba, int w, int h) {
    golden::Image img(w, h);
    REQUIRE(rgba.size() == static_cast<std::size_t>(w) * h * 4);
    std::memcpy(img.bytes().data(), rgba.data(), rgba.size());
    return img;
}

// A busy GL frame: a textured, blended quad over stripes of untextured quads.
void gl_scene(mt_gfx_target *t, u32 w, u32 h) {
    for (u32 i = 0; i < 8; i++) {
        mt_gfx_draw d = draw_for(t);
        d.color[0] = static_cast<u8>(30 * i);
        d.color[1] = static_cast<u8>(255 - 30 * i);
        d.color[2] = 90;
        auto v = rect(static_cast<float>(i * w / 8), 0, static_cast<float>((i + 1) * w / 8),
                      static_cast<float>(h), w, h);
        d.vertices = v.data();
        d.count = 6;
        REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
    }
    std::vector<u8> texels(16 * 16 * 4);
    for (std::size_t i = 0; i < 16 * 16; i++) {
        texels[i * 4 + 0] = static_cast<u8>(i * 7);
        texels[i * 4 + 1] = static_cast<u8>(i * 3);
        texels[i * 4 + 2] = static_cast<u8>(255 - i);
        texels[i * 4 + 3] = static_cast<u8>(i % 3 == 0 ? 255 : 128);
    }
    mt_gfx_draw d = draw_for(t);
    d.texture = make_texture(16, 16, texels);
    d.blend = 1;
    d.src_factor = MT_GFX_SRC_ALPHA;
    d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
    auto v = rect(static_cast<float>(w) / 4, static_cast<float>(h) / 4,
                  static_cast<float>(w) * 3 / 4, static_cast<float>(h) * 3 / 4, w, h);
    d.vertices = v.data();
    d.count = 6;
    REQUIRE(mt_gfx_submit(t, &d) == MT_GFX_DRAWN);
}

// An overlay as the compositor's overlay mode makes it: opaque UI outside the GL slot, the slot
// transparent, a translucent panel, solid glyph-like pixels and a faint button over it
// (premultiplied).
golden::Image overlay_image(int w, int h, int rx, int ry, int rw, int rh) {
    golden::Image img(w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const bool slot = x >= rx && y >= ry && x < rx + rw && y < ry + rh;
            golden::Rgba c{static_cast<u8>(x), static_cast<u8>(y), 60, 255}; // the shell
            if (slot) {
                const bool panel = y > h / 2 && y < h / 2 + h / 8;
                c = {0, 0, 0, 0};
                if (panel) { // a dialogue panel: (20, 40, 200) at alpha 160
                    c = {static_cast<u8>(20 * 160 / 255), static_cast<u8>(40 * 160 / 255),
                         static_cast<u8>(200 * 160 / 255), 160};
                }
                if (panel && (x / 3 + y / 5) % 7 == 0) {
                    c = {255, 255, 255, 255}; // text
                }
                if (x > rx + 10 && x < rx + 30 && y > ry + 10 && y < ry + 30) {
                    c = {64, 32, 16, 64}; // a faint key pad button
                }
            }
            img.set(x, y, c);
        }
    }
    return img;
}

struct GpuPresent {
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *window = nullptr; // the window's stand-in
    fe::Presenter presenter;
    fe::GpuScreen screen;
    fe::PresentPass pass;
    u32 width, height;

    GpuPresent(u32 w, u32 h, int out_w, int out_h) : width(w), height(h) {
        renderer = SDL_CreateGPURenderer(device(), nullptr);
        REQUIRE_MESSAGE(renderer != nullptr, SDL_GetError());
        window = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET,
                                   out_w, out_h);
        REQUIRE_MESSAGE(window != nullptr, SDL_GetError());
        REQUIRE(SDL_SetRenderTarget(renderer, window));
        REQUIRE(presenter.start(renderer, w, h, true));
        presenter.set_scaling(ScaleMode::kInteger, k_bars);
        REQUIRE(screen.start(renderer, presenter.texture(), w, h));
    }
    ~GpuPresent() {
        screen.stop();
        pass.close();
        presenter.stop();
        SDL_DestroyTexture(window);
        SDL_DestroyRenderer(renderer);
    }
    GpuPresent(const GpuPresent &) = delete;
    GpuPresent &operator=(const GpuPresent &) = delete;

    void take(mt_gfx_target *t, u32 index) {
        mt_gles_frame f{};
        f.index = index;
        f.target = t;
        REQUIRE(screen.take_gl_frame(f));
    }
    golden::Image compose(const golden::Image &overlay, const fe::Rect &r) {
        REQUIRE(screen.compose(overlay.bytes().data(), width * 4, &r));
        std::vector<u8> px;
        REQUIRE(screen.read_screen(&px));
        return from_bytes(px, static_cast<int>(width), static_cast<int>(height));
    }
};

} // namespace

TEST_CASE("GPU presentation: the GL frame under an overlay, composed on the GPU, no read-back") {
    REQUIRE_GPU();
    for (const u32 scale : {1u, 4u}) {
        INFO("scale " << scale);
        Fresh fresh;
        use(true);
        const u32 w = 480 * scale, h = 320 * scale;
        // The GL view inside the screen (the game's fills it; a smaller one shows the edges).
        const int rx = static_cast<int>(40 * scale), ry = static_cast<int>(16 * scale);
        const u32 gw = 400 * scale, gh = 288 * scale;
        Target t(mt_gfx_target_create(400, 288, scale));
        REQUIRE(t != nullptr);
        gl_scene(t.get(), 400, 288);
        mt_gfx_end_frame();
        GpuPresent p(w, h, static_cast<int>(w) + 100, static_cast<int>(h) + 60);
        p.take(t.get(), 1);
        const golden::Image overlay = overlay_image(static_cast<int>(w), static_cast<int>(h), rx,
                                                    ry, static_cast<int>(gw), static_cast<int>(gh));
        const fe::Rect r{static_cast<float>(rx), static_cast<float>(ry), static_cast<float>(gw),
                         static_cast<float>(gh)};
        const golden::Image got = p.compose(overlay, r);
        // The window: the Presenter draws the screen texture exactly as an uploaded one.
        REQUIRE(p.presenter.draw());
        std::string report;
        CHECK_MESSAGE(p.presenter.check_readback(got.bytes().data(), w * 4, &report) == 0, report);
        // Nothing of mt_gfx came back to the CPU.
        CHECK(mt_gfx_gpu_get_stats().readbacks == 0);
        CHECK(p.screen.stats().frames_taken == 1);
        // The CPU's pixels of the same frame (a read-back now) and the reference composite.
        golden::Image gl(static_cast<int>(gw), static_cast<int>(gh));
        std::memcpy(gl.bytes().data(), mt_gfx_target_cpixels(t.get()), gl.bytes().size());
        CHECK(mt_gfx_gpu_get_stats().readbacks == 1);
        const golden::Image want = reference(gl, overlay, rx, ry);
        const fe::ScreenDiff d = fe::diff_screens(got.bytes().data(), want.bytes().data(), w, h);
        std::printf("gfx_gpu: GPU presentation at %ux: %llu of %llu pixels beyond +-2, max "
                    "channel delta %d\n",
                    scale, static_cast<unsigned long long>(d.beyond),
                    static_cast<unsigned long long>(d.pixels), d.max_delta);
        CHECK(d.within());
        CHECK(d.max_delta <= 1); // one layer: the blend's rounding at most
        // Opaque: the GL frame's own alpha never shows.
        std::size_t translucent = 0;
        for (std::size_t i = 3; i < got.bytes().size(); i += 4) {
            translucent += got.bytes()[i] != 255 ? 1 : 0;
        }
        CHECK(translucent == 0);
        // read_gl hands back the frame the GPU shows, opaque, as Screen::take_gl_frame keeps it.
        std::vector<u8> shown;
        REQUIRE(p.screen.read_gl(&shown));
        golden::Image opaque = gl;
        for (std::size_t i = 3; i < opaque.bytes().size(); i += 4) {
            opaque.bytes()[i] = 255;
        }
        CHECK(from_bytes(shown, static_cast<int>(gw), static_cast<int>(gh)) == opaque);
    }
}

TEST_CASE("GPU presentation: the frame taken is shown, whatever the game draws after it") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    // A target the GPU never drew: its CPU fill goes up at the take.
    Target t = make_target(480, 320, {200, 10, 10, 255});
    GpuPresent p(480, 320, 480, 320);
    const golden::Image clear(480, 320); // all transparent
    const fe::Rect r{0, 0, 480, 320};
    p.take(t.get(), 1);
    CHECK(p.compose(clear, r).at(100, 100) == golden::Rgba{200, 10, 10, 255});
    // The game draws the next frame (queued, then flushed by end_frame) without presenting:
    // the window still shows frame 1, as the device's CAEAGLLayer would.
    mt_gfx_draw d = draw_for(t.get());
    d.color[0] = 0;
    d.color[1] = 0;
    d.color[2] = 255;
    auto v = rect(0, 0, 480, 320, 480, 320);
    d.vertices = v.data();
    d.count = 6;
    REQUIRE(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
    CHECK(p.compose(clear, r).at(100, 100) == golden::Rgba{200, 10, 10, 255});
    mt_gfx_end_frame();
    CHECK(p.compose(clear, r).at(100, 100) == golden::Rgba{200, 10, 10, 255});
    p.take(t.get(), 2);
    CHECK(p.compose(clear, r).at(100, 100) == golden::Rgba{0, 0, 255, 255});
    // A CPU write after the GPU's draws (mt_gfx_target_fill) is what the next take shows.
    const u8 green[4] = {0, 180, 0, 255};
    mt_gfx_target_fill(t.get(), green);
    p.take(t.get(), 3);
    CHECK(p.compose(clear, r).at(100, 100) == golden::Rgba{0, 180, 0, 255});
    CHECK(p.screen.gl_frame() == 3);
    CHECK(mt_gfx_gpu_get_stats().readbacks == 0);
    // Without a GL view nothing of the frame shows: the FULL composite's black.
    REQUIRE(p.screen.compose(clear.bytes().data(), 480 * 4, nullptr));
    std::vector<u8> px;
    REQUIRE(p.screen.read_screen(&px));
    CHECK(from_bytes(px, 480, 320).at(100, 100) == golden::Rgba{0, 0, 0, 255});
}

TEST_CASE("GPU presentation: an unchanged overlay is not uploaded again") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    Target t = make_target(480, 320, {1, 2, 3, 255});
    GpuPresent p(480, 320, 480, 320);
    p.take(t.get(), 1);
    golden::Image overlay = overlay_image(480, 320, 0, 0, 480, 320);
    const fe::Rect r{0, 0, 480, 320};
    const golden::Image first = p.compose(overlay, r);
    CHECK(p.compose(overlay, r) == first);
    CHECK(p.screen.stats().overlay_uploads == 1);
    CHECK(p.screen.stats().composes == 2);
    overlay.set(5, 5, {255, 0, 0, 255});
    CHECK(p.compose(overlay, r).at(5, 5) == golden::Rgba{255, 0, 0, 255});
    CHECK(p.screen.stats().overlay_uploads == 2);
}

TEST_CASE("GPU presentation: a shader filter's pass draws the composed screen") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    Target t(mt_gfx_target_create(480, 320, 1));
    REQUIRE(t != nullptr);
    gl_scene(t.get(), 480, 320);
    GpuPresent p(480, 320, 1500, 1000);
    p.take(t.get(), 1);
    const golden::Image overlay = overlay_image(480, 320, 0, 0, 480, 320);
    const golden::Image got = p.compose(overlay, fe::Rect{0, 0, 480, 320});
    for (const fe::ShaderFilter f :
         {fe::ShaderFilter::kXbr, fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
        INFO(fe::shader_filter_name(f));
        REQUIRE(p.pass.open_filter(p.renderer, f));
        p.presenter.set_stage(p.pass.stage());
        REQUIRE(p.presenter.draw());
        std::string report;
        CHECK_MESSAGE(p.presenter.check_readback(got.bytes().data(), 480 * 4, &report) == 0,
                      report);
    }
    // The colour-blind filter (#158) on the composed screen texture, under a filter and alone.
    for (const fe::ColorblindFilter cb : k_colorblind) {
        INFO(fe::colorblind_filter_name(cb));
        p.presenter.set_colorblind(cb);
        REQUIRE(p.presenter.draw());
        CHECK(p.presenter.color_path() == fe::Presenter::ColorPath::kGpu);
        std::string report;
        CHECK_MESSAGE(p.presenter.check_readback(got.bytes().data(), 480 * 4, &report) == 0,
                      report);
    }
    p.presenter.set_stage(fe::PresentStage{});
    p.pass.close();
    REQUIRE(p.presenter.draw());
    std::string report;
    CHECK_MESSAGE(p.presenter.check_readback(got.bytes().data(), 480 * 4, &report) == 0, report);
    p.presenter.set_colorblind(fe::ColorblindFilter::kOff);
    CHECK(mt_gfx_gpu_get_stats().readbacks == 0);
}

// The passes' shaders stay with the renderer (gfx_backend.h's present_shader): SDL's GPU renderer
// caches a pipeline per custom shader pointer and never forgets one, so a shader released while
// it lives could hand its address, and its cached pipeline, to the next shader made (the
// colour-blind pass after a filter switch drew black, about 1 run in 12 under load). Switching
// filters and closing a pass releases nothing; each name has one shader per renderer, and no two
// names share one.
TEST_CASE("GPU presentation: a pass's shader is the renderer's until it goes") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    GpuPresent p(480, 320, 480, 320);
    SDL_GPUShader *xbr = fe::present_shader(p.renderer, "present_xbr.frag");
    SDL_GPUShader *crt = fe::present_shader(p.renderer, "present_crt.frag");
    SDL_GPUShader *lcd = fe::present_shader(p.renderer, "present_lcd.frag");
    SDL_GPUShader *cb = fe::present_shader(p.renderer, "present_colorblind.frag");
    REQUIRE(xbr != nullptr);
    REQUIRE(crt != nullptr);
    REQUIRE(lcd != nullptr);
    REQUIRE(cb != nullptr);
    CHECK(xbr != crt);
    CHECK(crt != lcd);
    CHECK(lcd != xbr);
    CHECK(cb != xbr);
    CHECK(cb != crt);
    CHECK(cb != lcd);
    for (const fe::ShaderFilter f :
         {fe::ShaderFilter::kXbr, fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
        REQUIRE(p.pass.open_filter(p.renderer, f));
    }
    p.pass.close();
    CHECK(fe::present_shader(p.renderer, "present_xbr.frag") == xbr);
    CHECK(fe::present_shader(p.renderer, "present_crt.frag") == crt);
    CHECK(fe::present_shader(p.renderer, "present_lcd.frag") == lcd);
    CHECK(fe::present_shader(p.renderer, "present_colorblind.frag") == cb);
    CHECK(fe::present_shader(p.renderer, "no.such") == nullptr);
    CHECK(fe::present_shader(nullptr, "present_xbr.frag") == nullptr);
}

// The remap overlay with GPU presentation (#70 with #130, ADR 0068's addendum): drawn by
// compose_over over UIKit's premultiplied layer, the screen texture equals the CPU path's
// window image (the FULL composite with the overlay drawn over it, game.cpp's shown_pixels)
// within ADR 0060's tolerance; redrawn while the guest is held it needs no new compose; the
// next compose is without it (a shot after it), and nothing of mt_gfx is read back.
TEST_CASE("GPU presentation: the remap overlay over the composed screen, and the shot without it") {
    REQUIRE_GPU();
    for (const u32 scale : {1u, 2u}) {
        INFO("scale " << scale);
        Fresh fresh;
        use(true);
        const u32 w = 480 * scale, h = 320 * scale;
        const int rx = static_cast<int>(40 * scale), ry = static_cast<int>(16 * scale);
        const u32 gw = 400 * scale, gh = 288 * scale;
        Target t(mt_gfx_target_create(400, 288, scale));
        REQUIRE(t != nullptr);
        gl_scene(t.get(), 400, 288);
        mt_gfx_end_frame();
        GpuPresent p(w, h, static_cast<int>(w), static_cast<int>(h));
        p.take(t.get(), 1);
        const golden::Image overlay = overlay_image(static_cast<int>(w), static_cast<int>(h), rx,
                                                    ry, static_cast<int>(gw), static_cast<int>(gh));
        const fe::Rect r{static_cast<float>(rx), static_cast<float>(ry), static_cast<float>(gw),
                         static_cast<float>(gh)};
        const golden::Image game = p.compose(overlay, r);

        fe::Remap remap(nullptr, nullptr);
        remap.open(0);
        REQUIRE(remap.is_open());
        const auto draw = [&](u8 *rgba) { remap.draw(rgba, w, h, static_cast<int>(scale)); };
        REQUIRE(p.screen.compose_over(draw));
        std::vector<u8> px;
        REQUIRE(p.screen.read_screen(&px));
        const golden::Image shown = from_bytes(px, static_cast<int>(w), static_cast<int>(h));
        REQUIRE(p.presenter.draw());
        std::string report;
        CHECK_MESSAGE(p.presenter.check_readback(shown.bytes().data(), w * 4, &report) == 0,
                      report);
        CHECK(mt_gfx_gpu_get_stats().readbacks == 0);

        // The CPU path: the FULL composite of the same layers, the overlay drawn over it.
        golden::Image gl(static_cast<int>(gw), static_cast<int>(gh));
        std::memcpy(gl.bytes().data(), mt_gfx_target_cpixels(t.get()), gl.bytes().size());
        golden::Image want = reference(gl, overlay, rx, ry);
        remap.draw(want.bytes().data(), w, h, static_cast<int>(scale));
        const fe::ScreenDiff d = fe::diff_screens(shown.bytes().data(), want.bytes().data(), w, h);
        std::printf("gfx_gpu: the remap overlay at %ux on the GPU: %llu of %llu pixels beyond "
                    "+-2, max channel delta %d\n",
                    scale, static_cast<unsigned long long>(d.beyond),
                    static_cast<unsigned long long>(d.pixels), d.max_delta);
        CHECK(d.within());
        CHECK(d.max_delta <= 2);
        CHECK_FALSE(shown == game); // the overlay shows (its backdrop dims the whole screen)

        // A redraw while the guest is held (run_remap): the same layers, no compose between.
        REQUIRE(p.screen.compose_over(draw));
        REQUIRE(p.screen.read_screen(&px));
        CHECK(from_bytes(px, static_cast<int>(w), static_cast<int>(h)) == shown);
        // The next compose (a present, a shot) has UIKit's layer only: the game's screen again.
        CHECK(p.compose(overlay, r) == game);
        // And the overlay can be drawn over that compose again.
        REQUIRE(p.screen.compose_over(draw));
        REQUIRE(p.screen.read_screen(&px));
        CHECK(from_bytes(px, static_cast<int>(w), static_cast<int>(h)) == shown);
        CHECK(mt_gfx_gpu_get_stats().readbacks == 1); // the test's own CPU pixels only
    }
}

TEST_CASE("GPU presentation: compose_over needs a compose first") {
    REQUIRE_GPU();
    Fresh fresh;
    use(true);
    GpuPresent p(480, 320, 480, 320);
    bool called = false;
    CHECK_FALSE(p.screen.compose_over([&](u8 *) { called = true; }));
    CHECK_FALSE(called);
}
