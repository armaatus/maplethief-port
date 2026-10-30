// mt_gfx's GPU backend (#120, ADR 0060, maplethief/gfx_gpu.h) against the software rasterizer,
// the reference. The synthetic scenes of gfx_test.cpp's golden images (the prototype scene,
// every blend factor pair, the perspective floor, the lines) are rendered on the GPU and
// compared with the committed goldens, within ADR 0060's tolerance. The test also renders a
// scene at internal scale 4 on both backends, checks the sync points (CPU writes and reads
// between draws, a texture re-uploaded or destroyed while draws that use it are queued, a
// target destroyed with draws queued, switching backends halfway through a frame), the
// replacement hook's images, batching, and the GPU time of a busy frame at scale 4 (under
// 2 ms on the Mac).
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
#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"
#include "maplethief/recomp/calls.h"

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
    const double median_ms = ms[frames / 2], worst_ms = ms.back();
    const mt_gfx_gpu_stats st = mt_gfx_gpu_get_stats();
    CHECK(st.batches == warm.batches + 2u * static_cast<unsigned>(frames));
    CHECK(st.texture_uploads == 1); // the texture stays on the GPU
    CHECK(st.target_uploads == 1);
    CHECK(st.readbacks == 0); // nothing read the target
    // The backend's own median (what the frontend logs) is over the same 61 timed frames.
    CHECK(st.timed_frames == static_cast<unsigned>(frames) + 1u);
    CHECK(st.median_frame_gpu_ns > 0);
    std::printf("gfx_gpu: busy frame at scale 4 (1920x1280, 301 draws, 2 batches): GPU time "
                "%.3f ms a frame (median of %d), %.3f ms at most (fence-timed, an upper "
                "bound)\n",
                median_ms, frames, worst_ms);
    // #120's acceptance criterion: under 2 ms of GPU time per frame on the Mac at N = 4. The
    // median, because a fence wait now and then also waits for the OS to schedule the thread.
    CHECK(median_ms < 2.0);
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
