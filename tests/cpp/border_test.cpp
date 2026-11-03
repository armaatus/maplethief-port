// The border (#157, ADR 0085) with synthetic data only: the cover rectangles, the blur's levels,
// the border art's decoder against good and hostile PNGs, the settings rows, and headless
// read-backs (SDL's dummy video driver, the software renderer) at 16:9 and 21:9 in every scale
// mode: border = color with black byte-identical to the Presenter's drawing before #157, the
// blur's bars a smooth, dimmed continuation of the frame's edges, the art covering the bars,
// a file that cannot be read keeping the border colour, a new render scale keeping the art, and
// widescreen's wings (#86) over the border, beside the picture.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "border.h"
#include "present.h"
#include "scaling.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::BorderMode;
using fe::ScaleMode;

constexpr ScaleMode kModes[] = {ScaleMode::kInteger, ScaleMode::kFit, ScaleMode::kSmooth,
                                ScaleMode::kSharp};

struct Size {
    int w, h;
};
// 16:9 (1080p, 1440p), 21:9 (2560x1080) and 16:10 (1440x900).
constexpr Size kWide[] = {{1920, 1080}, {2560, 1080}, {2560, 1440}, {1440, 900}};

std::string output_dir() {
    const char *env = std::getenv("MAPLETHIEF_TEST_OUTPUT_DIR");
    return env != nullptr && *env != '\0' ? env : MT_TEST_OUTPUT_DIR;
}

// A headless window (SDL's dummy video driver: the software renderer) of the given size.
struct Window {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    Window(int w, int h) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("border_test", w, h, 0, &window, &renderer),
                        SDL_GetError());
    }
    ~Window() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
};

// The drawn output, R, G, B, A bytes, rows of w * 4.
struct Readback {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    explicit Readback(SDL_Renderer *renderer) {
        SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
        REQUIRE_MESSAGE(read != nullptr, SDL_GetError());
        SDL_Surface *s = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(read);
        REQUIRE_MESSAGE(s != nullptr, SDL_GetError());
        w = s->w;
        h = s->h;
        px.resize(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; y++) {
            std::memcpy(px.data() + static_cast<std::size_t>(y) * w * 4,
                        static_cast<const std::uint8_t *>(s->pixels) +
                            static_cast<std::size_t>(y) * s->pitch,
                        static_cast<std::size_t>(w) * 4);
        }
        SDL_DestroySurface(s);
    }
    const std::uint8_t *at(int x, int y) const {
        return px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
    }
};

// Every texel its own colour (as scaling_test's), never black.
std::vector<std::uint8_t> unique_image(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            std::uint8_t *p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(x & 0xFF);
            p[1] = static_cast<std::uint8_t>(y & 0xFF);
            p[2] = static_cast<std::uint8_t>(0x80 | ((x >> 8) << 2) | (y >> 8));
            p[3] = 255;
        }
    }
    return px;
}

// Left half red, right half blue: the blur's bars show each side's colour, dimmed.
std::vector<std::uint8_t> halves_image(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            std::uint8_t *p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            const bool left = x < w / 2;
            p[0] = left ? 255 : 0;
            p[1] = 0;
            p[2] = left ? 0 : 255;
            p[3] = 255;
        }
    }
    return px;
}

// A PNG of w x h RGBA pixels, encoded by SDL.
std::vector<std::uint8_t> encode_png(int w, int h, const std::vector<std::uint8_t> &rgba) {
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888,
                                           const_cast<std::uint8_t *>(rgba.data()), w * 4);
    REQUIRE_MESSAGE(s != nullptr, SDL_GetError());
    SDL_IOStream *io = SDL_IOFromDynamicMem();
    REQUIRE(io != nullptr);
    REQUIRE_MESSAGE(SDL_SavePNG_IO(s, io, false), SDL_GetError());
    SDL_DestroySurface(s);
    const Sint64 n = SDL_TellIO(io);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
    REQUIRE(SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) == 0);
    REQUIRE(SDL_ReadIO(io, bytes.data(), bytes.size()) == bytes.size());
    SDL_CloseIO(io);
    return bytes;
}

void write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    REQUIRE(f.good());
}

// A PNG whose header says w x h, with no image data (CRCs zero: stb_image does not check them).
std::vector<std::uint8_t> header_only_png(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto be32 = [&b](std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) {
            b.push_back(static_cast<std::uint8_t>(v >> s));
        }
    };
    be32(13);
    for (const char c : {'I', 'H', 'D', 'R'}) {
        b.push_back(static_cast<std::uint8_t>(c));
    }
    be32(w);
    be32(h);
    for (const std::uint8_t v : std::initializer_list<std::uint8_t>{
             8, 6, 0, 0, 0}) { // 8-bit RGBA, deflate, no filter, no interlace
        b.push_back(v);
    }
    be32(0); // CRC
    be32(0);
    for (const char c : {'I', 'E', 'N', 'D'}) {
        b.push_back(static_cast<std::uint8_t>(c));
    }
    be32(0);
    return b;
}

bool near(const std::uint8_t *p, int r, int g, int b, int tolerance) {
    return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance &&
           std::abs(p[2] - b) <= tolerance;
}

} // namespace

TEST_CASE("border: cover_rect and cover_about cover the output, centred on the image") {
    // The art: a 16:9 image over a 21:9 output fills the width and is cropped top and bottom.
    fe::Rect r = fe::cover_rect(2560, 1080, 1920, 1080);
    CHECK(r.x == 0);
    CHECK(r.w == 2560);
    CHECK(r.h == 1440);
    CHECK(r.y == -180);
    // A 4:3 image over 16:9 also fills the width.
    r = fe::cover_rect(1920, 1080, 800, 600);
    CHECK(r.w == 1920);
    CHECK(r.h == 1440);
    CHECK(r.y == -180);
    // A wide image over a tall output fills the height.
    r = fe::cover_rect(1000, 1000, 300, 100);
    CHECK(r.h == 1000);
    CHECK(r.w == 3000);
    CHECK(r.x == -1000);
    // Exactly the output's ratio: the output itself.
    r = fe::cover_rect(1920, 1080, 16, 9);
    CHECK((r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080));
    CHECK(fe::cover_rect(0, 1080, 16, 9).w == 0);
    CHECK(fe::cover_rect(1920, 1080, 0, 9).w == 0);
    // The blur: integer 3x at 1080p (1440x960 at (240, 60)) scaled by 4/3 about (960, 540).
    r = fe::cover_about(fe::Rect{240, 60, 1440, 960}, 1920, 1080);
    CHECK(r.x == 0);
    CHECK(r.w == 1920);
    CHECK(r.y == -100);
    CHECK(r.h == 1280);
    // The fit at 21:9: 1620x1080 at (470, 0), scaled by 2560/1620.
    r = fe::cover_about(fe::Rect{470, 0, 1620, 1080}, 2560, 1080);
    CHECK(r.x <= 0);
    CHECK(r.x + r.w >= 2560);
    CHECK(r.y <= 0);
    CHECK(r.y + r.h >= 1080);
    CHECK(r.w <= 2562);
    // Off centre (a safe area moved it): still covers every edge.
    r = fe::cover_about(fe::Rect{100, 50, 480, 320}, 1000, 400);
    CHECK(r.x <= 0);
    CHECK(r.y <= 0);
    CHECK(r.x + r.w >= 1000);
    CHECK(r.y + r.h >= 400);
    // Already covering: unchanged (never scaled down).
    r = fe::cover_about(fe::Rect{0, 0, 960, 640}, 960, 640);
    CHECK((r.x == 0 && r.y == 0 && r.w == 960 && r.h == 640));
    CHECK(fe::cover_about(fe::Rect{}, 960, 640).w == 0);
}

TEST_CASE("border: the blur's levels halve to 32 texels wide and come back up to 128") {
    fe::BlurChain c = fe::blur_chain(480, 320);
    REQUIRE(c.levels.size() == 4);
    CHECK(c.levels[0] == fe::BlurLevel{240, 160});
    CHECK(c.levels[1] == fe::BlurLevel{120, 80});
    CHECK(c.levels[3] == fe::BlurLevel{30, 20});
    CHECK(c.top == 1);
    // Render scale 3: 1440x960 to 23x15, drawn from 90x60.
    c = fe::blur_chain(1440, 960);
    REQUIRE(c.levels.size() == 6);
    CHECK(c.levels.back() == fe::BlurLevel{23, 15});
    CHECK(c.levels[c.top] == fe::BlurLevel{90, 60});
    // Render scale 8: every level at most half the last, the last at most 32 wide.
    c = fe::blur_chain(3840, 2560);
    CHECK(c.levels.front() == fe::BlurLevel{1920, 1280});
    CHECK(c.levels.back().w <= fe::kBlurBaseWidth);
    CHECK(c.levels[c.top].w <= fe::kBlurTopWidth);
    CHECK(c.levels[c.top - 1].w > fe::kBlurTopWidth);
    // Tiny and empty sources.
    c = fe::blur_chain(1, 1);
    REQUIRE(c.levels.size() == 1);
    CHECK(c.levels[0] == fe::BlurLevel{1, 1});
    CHECK(c.top == 0);
    CHECK(fe::blur_chain(0, 320).levels.empty());
}

TEST_CASE("border: a PNG within the limits decodes; hostile and oversized ones are refused") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE(SDL_Init(0));
    std::vector<std::uint8_t> rgba(64 * 36 * 4);
    for (std::size_t i = 0; i < rgba.size(); i++) {
        rgba[i] = static_cast<std::uint8_t>(i * 7);
    }
    const std::vector<std::uint8_t> png = encode_png(64, 36, rgba);
    fe::BorderImage img;
    std::string why;
    REQUIRE_MESSAGE(fe::decode_border_image(png.data(), png.size(), &img, &why), why);
    CHECK(img.width == 64);
    CHECK(img.height == 36);
    CHECK(img.rgba == rgba);

    auto refused = [](const std::vector<std::uint8_t> &bytes, const char *reason) {
        fe::BorderImage out;
        std::string w;
        CHECK_FALSE(fe::decode_border_image(bytes.data(), bytes.size(), &out, &w));
        CHECK(out.rgba.empty());
        CHECK_MESSAGE(w.find(reason) != std::string::npos, w);
    };
    refused({}, "empty");
    refused({0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I', 'F'}, "not a PNG"); // a JPEG
    refused(std::vector<std::uint8_t>(png.begin(), png.begin() + 8), "not a PNG stb_image");
    refused(std::vector<std::uint8_t>(png.begin(), png.begin() + png.size() / 2), "broken");
    // Garbage after the signature, and the image data's bytes scrambled.
    std::vector<std::uint8_t> garbage(png.begin(), png.begin() + 8);
    for (int i = 0; i < 4000; i++) {
        garbage.push_back(static_cast<std::uint8_t>((i * 2654435761u) >> 13));
    }
    refused(garbage, "not a PNG");
    std::vector<std::uint8_t> scrambled = png;
    for (std::size_t i = 60; i < scrambled.size() - 12; i++) {
        scrambled[i] = static_cast<std::uint8_t>(scrambled[i] * 31 + 7);
    }
    refused(scrambled, "broken");
    // Sizes: refused from the header, before anything is decoded.
    refused(header_only_png(8193, 10), "not a PNG stb_image"); // stb_image's own dimension cap
    refused(header_only_png(8192, 8192), "is larger than");    // over 8K UHD in all
    refused(header_only_png(0, 10), "not a PNG stb_image");
    // A header within the limits but no data: broken.
    refused(header_only_png(7680, 4320), "broken");
    // Over 32 MiB.
    std::vector<std::uint8_t> huge(fe::kMaxBorderImageBytes + 1, 0);
    std::memcpy(huge.data(), png.data(), 8);
    refused(huge, "MiB");

    // From a file: the same, and a missing file.
    const fs::path dir = fs::path(output_dir()) / "border_test";
    fs::create_directories(dir);
    write_file(dir / "art.png", png);
    fe::BorderImage from_file;
    REQUIRE_MESSAGE(fe::load_border_image((dir / "art.png").string(), &from_file, &why), why);
    CHECK(from_file.rgba == rgba);
    CHECK_FALSE(fe::load_border_image((dir / "missing.png").string(), &from_file, &why));
    CHECK(why.find("cannot open") != std::string::npos);
    CHECK_FALSE(fe::load_border_image("", &from_file, &why));
    SDL_Quit();
}

TEST_CASE("settings: border is color (the default), blur or image; border_image a file (#157)") {
    fe::Settings s;
    CHECK(s.border == BorderMode::kColor);
    CHECK(s.border_image.empty());
    CHECK(std::string(fe::border_mode_name(BorderMode::kBlur)) == "blur");
    std::vector<std::string> problems =
        fe::parse_settings("border = BLUR\nborder_image = /home/me/art work.png\n", &s);
    CHECK(problems.empty());
    CHECK(s.border == BorderMode::kBlur);
    CHECK(s.border_image == "/home/me/art work.png");
    problems = fe::parse_settings("border = image\nborder_image = C:\\Users\\Me\\art.png\n", &s);
    CHECK(problems.empty());
    CHECK(s.border == BorderMode::kImage);
    CHECK(s.border_image == "C:\\Users\\Me\\art.png");
    // Bad values keep the old one.
    problems = fe::parse_settings("border = image\nborder = fuzzy\nborder_image = a\tb\n", &s);
    CHECK(problems.size() == 2);
    CHECK(s.border == BorderMode::kImage);
    CHECK(s.border_image == "C:\\Users\\Me\\art.png");
    problems = fe::parse_settings("border_image = " + std::string(5000, 'x') + "\n", &s);
    CHECK(problems.size() == 1);
    // Empty is none.
    problems = fe::parse_settings("border_image =\n", &s);
    CHECK(problems.empty());
    CHECK(s.border_image.empty());
    // Written and read back.
    fe::Settings w;
    w.border = BorderMode::kImage;
    w.border_image = "art.png";
    const std::string text = fe::serialise_settings(w);
    CHECK(text.find("border = image\n") != std::string::npos);
    CHECK(text.find("border_image = art.png\n") != std::string::npos);
    fe::Settings r;
    CHECK(fe::parse_settings(text, &r).empty());
    CHECK(r == w);
    // The flags.
    CHECK(fe::find_setting_flag("--border") == fe::find_setting("border"));
    CHECK(fe::find_setting_flag("--border-image") == fe::find_setting("border_image"));
    CHECK(fe::setting_choices(*fe::find_setting("border")) ==
          std::vector<std::string>{"color", "blur", "image"});
}

TEST_CASE("border: color with black is byte-identical to the Presenter before #157, every mode") {
    // border = color draws what the Presenter drew before #157: the clear and the image, nothing
    // else. A Presenter that never heard of the border, one set to color (black) explicitly and
    // one set to color after a frame of blur read back the same bytes; integer mode's equal a
    // reference made from the image (black bars, each texel k x k pixels), and the Presenter's
    // own check passes with every bar checked.
    for (const Size size : kWide) {
        Window w(size.w, size.h);
        const std::vector<std::uint8_t> image = unique_image(480, 320);
        for (const ScaleMode mode : kModes) {
            INFO(fe::scale_mode_name(mode) << " at " << size.w << "x" << size.h);
            fe::Presenter before;
            REQUIRE(before.start(w.renderer, 480, 320));
            before.set_scaling(mode, fe::Color{});
            REQUIRE(before.upload(image.data(), 480 * 4));
            REQUIRE(before.draw());
            CHECK(before.border().drawn() == BorderMode::kColor);
            const Readback a(w.renderer);
            std::string report;
            CHECK_MESSAGE(before.check_readback(image.data(), 480 * 4, &report) == 0, report);
            CHECK(report.find("not checked") == std::string::npos);
            before.stop();

            fe::Presenter now;
            REQUIRE(now.start(w.renderer, 480, 320));
            now.set_scaling(mode, fe::Color{});
            now.set_border(BorderMode::kColor);
            REQUIRE(now.upload(image.data(), 480 * 4));
            REQUIRE(now.draw());
            const Readback b(w.renderer);
            CHECK(a.px == b.px);
            now.set_border(BorderMode::kBlur);
            REQUIRE(now.draw());
            CHECK(now.border().drawn() == BorderMode::kBlur);
            now.set_border(BorderMode::kColor);
            REQUIRE(now.draw());
            const Readback c(w.renderer);
            CHECK(a.px == c.px);
            CHECK(now.border().levels() == 0); // color frees the blur's targets
            CHECK_MESSAGE(now.check_readback(image.data(), 480 * 4, &report) == 0, report);
            if (mode == ScaleMode::kInteger) {
                const fe::Rect &r = now.placement().rect;
                const int k = now.placement().integer_scale;
                REQUIRE(k >= 2);
                std::vector<std::uint8_t> ref(a.px.size(), 0);
                for (int y = 0; y < size.h; y++) {
                    for (int x = 0; x < size.w; x++) {
                        std::uint8_t *p = &ref[(static_cast<std::size_t>(y) * size.w + x) * 4];
                        const int tx = (x - static_cast<int>(r.x));
                        const int ty = (y - static_cast<int>(r.y));
                        if (tx >= 0 && ty >= 0 && tx < 480 * k && ty < 320 * k) {
                            std::memcpy(
                                p, &image[(static_cast<std::size_t>(ty / k) * 480 + tx / k) * 4],
                                4);
                        }
                        p[3] = 255;
                    }
                }
                std::size_t differ = 0;
                for (std::size_t i = 0; i < ref.size(); i += 4) {
                    differ += std::memcmp(&ref[i], &a.px[i], 3) != 0 ? 1 : 0;
                }
                CHECK(differ == 0);
            }
            now.stop();
        }
    }
}

TEST_CASE("border: blur fills the bars with the frame's edges, smooth and dimmed, 16:9 and 21:9") {
    const int dim = fe::kBlurDim;
    for (const Size size : kWide) {
        Window w(size.w, size.h);
        for (const int scale : {1, 2}) {
            const int iw = 480 * scale, ih = 320 * scale;
            const std::vector<std::uint8_t> halves = halves_image(iw, ih);
            for (const ScaleMode mode : kModes) {
                INFO(fe::scale_mode_name(mode)
                     << " at " << size.w << "x" << size.h << ", render scale " << scale);
                fe::Presenter p;
                REQUIRE(p.start(w.renderer, static_cast<std::uint32_t>(iw),
                                static_cast<std::uint32_t>(ih)));
                p.set_scaling(mode, fe::Color{0x20, 0x40, 0x10});
                p.set_border(BorderMode::kBlur);
                REQUIRE(p.upload(halves.data(), static_cast<std::uint32_t>(iw) * 4));
                REQUIRE(p.draw());
                CHECK(p.border().drawn() == BorderMode::kBlur);
                CHECK(p.border().levels() == fe::blur_chain(iw, ih).levels.size());
                // The image is the Presenter's as before; the bars are not checked.
                std::string report;
                CHECK_MESSAGE(p.check_readback(halves.data(), static_cast<std::uint32_t>(iw) * 4,
                                               &report) == 0,
                              report);
                CHECK(report.find("under the border blur (not checked)") != std::string::npos);
                // The screen texture's scale mode is the placement's again.
                SDL_ScaleMode sm = SDL_SCALEMODE_LINEAR;
                REQUIRE(SDL_GetTextureScaleMode(p.texture(), &sm));
                CHECK(sm == (mode == ScaleMode::kSmooth  ? SDL_SCALEMODE_LINEAR
                             : mode == ScaleMode::kSharp ? SDL_SCALEMODE_PIXELART
                                                         : SDL_SCALEMODE_NEAREST));
                const Readback rb(w.renderer);
                const fe::Rect &r = p.placement().rect;
                const int y = size.h / 2;
                // The left bar: the frame's red side, dimmed; the right bar its blue side.
                const int lx = static_cast<int>(r.x) / 2;
                const int rx =
                    static_cast<int>(r.x + r.w) + (size.w - static_cast<int>(r.x + r.w)) / 2;
                CHECK(near(rb.at(lx, y), dim, 0, 0, 4));
                CHECK(near(rb.at(rx, y), 0, 0, dim, 4));
                // Nowhere brighter than the dimmed frame; the bars never the border colour.
                long bright = 0;
                long colour = 0;
                for (int oy = 0; oy < size.h; oy += 7) {
                    for (int ox = 0; ox < size.w; ox += 5) {
                        const bool in = ox >= r.x && ox < r.x + r.w && oy >= r.y && oy < r.y + r.h;
                        if (in) {
                            continue;
                        }
                        const std::uint8_t *q = rb.at(ox, oy);
                        bright += q[0] > dim + 2 || q[2] > dim + 2 ? 1 : 0;
                        colour += near(q, 0x20, 0x40, 0x10, 0) ? 1 : 0;
                    }
                }
                CHECK(bright == 0);
                CHECK(colour == 0);
                // Where the bars run above and below the image (integer mode), the frame's
                // middle shows as a smooth blend from red to blue: monotone along the row.
                if (r.y >= 8) {
                    const int by = static_cast<int>(r.y) / 2;
                    int last_r = 255;
                    int last_b = 0;
                    long steps_back = 0;
                    for (int ox = 0; ox < size.w; ox++) {
                        const std::uint8_t *q = rb.at(ox, by);
                        steps_back += q[0] > last_r + 1 || q[2] + 1 < last_b ? 1 : 0;
                        last_r = q[0];
                        last_b = q[2];
                    }
                    CHECK(steps_back == 0);
                    // The change from red to blue is spread over many pixels (blurred).
                    int from = -1;
                    int to = -1;
                    for (int ox = 0; ox < size.w; ox++) {
                        const std::uint8_t *q = rb.at(ox, by);
                        if (from < 0 && q[2] > 10) {
                            from = ox;
                        }
                        if (to < 0 && q[0] < 10) {
                            to = ox;
                        }
                    }
                    CHECK(to - from > size.w / 32);
                }
                p.stop();
            }
        }
    }
}

TEST_CASE("border: the blur keeps a render target the caller set, and follows a new size") {
    Window w(1920, 1080);
    SDL_Texture *target = SDL_CreateTexture(w.renderer, SDL_PIXELFORMAT_ABGR8888,
                                            SDL_TEXTUREACCESS_TARGET, 1920, 1080);
    REQUIRE(target != nullptr);
    REQUIRE(SDL_SetRenderTarget(w.renderer, target));
    const std::vector<std::uint8_t> one = halves_image(480, 320);
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    p.set_border(BorderMode::kBlur);
    REQUIRE(p.upload(one.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(SDL_GetRenderTarget(w.renderer) == target);
    CHECK(p.border().levels() == 4);
    {
        const Readback rb(w.renderer); // the target's pixels
        CHECK(near(rb.at(100, 540), fe::kBlurDim, 0, 0, 4));
    }
    // render_scale = auto's resize: the levels follow the new texture.
    const std::vector<std::uint8_t> two = halves_image(1440, 960);
    REQUIRE(p.start(w.renderer, 1440, 960));
    REQUIRE(p.upload(two.data(), 1440 * 4));
    REQUIRE(p.draw());
    CHECK(p.border().levels() == 6);
    CHECK(p.border().drawn() == BorderMode::kBlur);
    p.stop();
    SDL_SetRenderTarget(w.renderer, nullptr);
    SDL_DestroyTexture(target);
}

TEST_CASE("border: image covers the bars with the art over the border colour; bad files keep it") {
    const fs::path dir = fs::path(output_dir()) / "border_test";
    fs::create_directories(dir);
    // A 160x90 (16:9) picture: green on the left, magenta on the right, a transparent band in
    // the middle third of its rows (the border colour shows there).
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE(SDL_Init(0));
    std::vector<std::uint8_t> art(160 * 90 * 4);
    for (int y = 0; y < 90; y++) {
        for (int x = 0; x < 160; x++) {
            std::uint8_t *q = &art[(static_cast<std::size_t>(y) * 160 + x) * 4];
            const bool left = x < 80;
            q[0] = left ? 0 : 255;
            q[1] = left ? 200 : 0;
            q[2] = left ? 0 : 255;
            q[3] = y >= 30 && y < 60 ? 0 : 255;
        }
    }
    const fs::path png = dir / "border_art.png";
    write_file(png, encode_png(160, 90, art));
    write_file(dir / "not_a_png.png", std::vector<std::uint8_t>(100, 'x'));
    SDL_Quit();
    const fe::Color colour{0x20, 0x40, 0x10};
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    for (const Size size : kWide) {
        Window w(size.w, size.h);
        for (const ScaleMode mode : kModes) {
            INFO(fe::scale_mode_name(mode) << " at " << size.w << "x" << size.h);
            fe::Presenter p;
            REQUIRE(p.start(w.renderer, 480, 320));
            p.set_scaling(mode, colour);
            p.set_border(BorderMode::kImage, png.string());
            REQUIRE(p.upload(image.data(), 480 * 4));
            REQUIRE(p.draw());
            CHECK(p.border().drawn() == BorderMode::kImage);
            std::string report;
            CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
            CHECK(report.find("under the border image (not checked)") != std::string::npos);
            const fe::Rect &r = p.placement().rect;
            const fe::Rect c = fe::cover_rect(size.w, size.h, 160, 90);
            const Readback rb(w.renderer);
            // The bars: green left, magenta right, outside the transparent band; the border
            // colour inside it. Sampled away from the art's edges (linear filtering).
            auto art_row = [&](int oy) { return (oy - c.y) * 90 / c.h; };
            long wrong = 0;
            long checked = 0;
            for (int oy = 0; oy < size.h; oy += 9) {
                const double ay = art_row(oy);
                const bool band = ay > 31 && ay < 59;
                const bool opaque = ay < 29 || ay > 61;
                for (int ox = 0; ox < size.w; ox += 11) {
                    const bool in = ox >= r.x && ox < r.x + r.w && oy >= r.y && oy < r.y + r.h;
                    const double ax = (ox - c.x) * 160 / c.w;
                    if (in || (ax > 78 && ax < 82)) {
                        continue;
                    }
                    const std::uint8_t *q = rb.at(ox, oy);
                    const bool left = ax < 80;
                    if (band) {
                        wrong += near(q, colour.r, colour.g, colour.b, 0) ? 0 : 1;
                    } else if (opaque) {
                        wrong += near(q, left ? 0 : 255, left ? 200 : 0, left ? 0 : 255, 1) ? 0 : 1;
                    } else {
                        continue;
                    }
                    checked++;
                }
            }
            CHECK(checked > 100);
            CHECK(wrong == 0);
            // A missing file, then one that is not a PNG: the border colour, every bar checked.
            for (const fs::path &bad : {dir / "missing.png", dir / "not_a_png.png"}) {
                p.set_border(BorderMode::kImage, bad.string());
                REQUIRE(p.draw());
                CHECK(p.border().drawn() == BorderMode::kColor);
                CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
                CHECK(report.find("not checked") == std::string::npos);
            }
            // image with no file: the border colour.
            p.set_border(BorderMode::kImage, "");
            REQUIRE(p.draw());
            CHECK(p.border().drawn() == BorderMode::kColor);
            p.stop();
        }
    }
}

TEST_CASE("border: a new render scale keeps the art (not read again), a new renderer reads it") {
    const fs::path dir = fs::path(output_dir()) / "border_test";
    fs::create_directories(dir);
    const fs::path png = dir / "border_art_kept.png";
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE(SDL_Init(0));
        std::vector<std::uint8_t> art(32 * 18 * 4, 0);
        for (std::size_t i = 0; i < art.size(); i += 4) {
            art[i + 0] = 250;
            art[i + 3] = 255;
        }
        write_file(png, encode_png(32, 18, art));
        SDL_Quit();
    }
    Window w(1920, 1080);
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    p.set_border(BorderMode::kImage, png.string());
    const std::vector<std::uint8_t> one = unique_image(480, 320);
    REQUIRE(p.upload(one.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(p.border().drawn() == BorderMode::kImage);
    fs::remove(png);
    const std::vector<std::uint8_t> two = unique_image(960, 640);
    REQUIRE(p.start(w.renderer, 960, 640));
    REQUIRE(p.upload(two.data(), 960 * 4));
    REQUIRE(p.draw());
    CHECK(p.border().drawn() == BorderMode::kImage); // the texture was kept
    {
        const Readback rb(w.renderer);
        CHECK(near(rb.at(50, 540), 250, 0, 0, 1));
    }
    // Stopped (the renderer goes): the art is read again, and now it is gone.
    p.stop();
    REQUIRE(p.start(w.renderer, 480, 320));
    REQUIRE(p.upload(one.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(p.border().drawn() == BorderMode::kColor);
    p.stop();
}

// Widescreen (#86, ADR 0090) in #157's step list: the clear, the border, then the picture, of
// which the wings (the GL frame beside the screen image, in the 568-point wide picture) are the
// first part. The border (colour or blur) fills around the wide picture, the wings go over it,
// and the read-back never counts the wings as a bar.
TEST_CASE("border: widescreen's wings go over the border, beside the picture, never a bar") {
    Window w(1920, 1080);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    std::vector<std::uint8_t> gl(568 * 320 * 4);
    for (std::size_t i = 0; i < gl.size(); i += 4) {
        gl[i] = 0;
        gl[i + 1] = 200;
        gl[i + 2] = 200;
        gl[i + 3] = 255;
    }
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    p.set_scaling(ScaleMode::kInteger, fe::Color{});
    // Off: the wide placement is the placement.
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(p.wide_placement().rect.x == p.placement().rect.x);
    CHECK(p.wide_placement().rect.w == p.placement().rect.w);
    // On, integer at 3x: the wide picture 1704 x 960 at (108, 60), the image 1440 wide at 240.
    p.set_wide(568);
    for (const BorderMode mode : {BorderMode::kColor, BorderMode::kBlur}) {
        CAPTURE(static_cast<int>(mode));
        p.set_border(mode);
        REQUIRE(p.upload_wings(gl.data(), 568, 320, 568));
        REQUIRE(p.upload(image.data(), 480 * 4));
        REQUIRE(p.draw());
        CHECK(p.border().drawn() == mode);
        CHECK(p.wide_rect().x == 108);
        CHECK(p.wide_rect().w == 1704);
        CHECK(p.placement().rect.x == 240);
        CHECK(p.placement().rect.w == 1440);
        CHECK(p.wide_placement().rect.x == 108);
        std::string report;
        CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
        const Readback rb(w.renderer);
        for (const int x : {108, 200, 239, 1680, 1811}) {
            CAPTURE(x);
            const std::uint8_t *q = rb.at(x, 500);
            CHECK((q[0] == 0 && q[1] == 200 && q[2] == 200)); // the wings, over the border
        }
        const std::uint8_t *bar = rb.at(50, 500);
        if (mode == BorderMode::kColor) {
            CHECK((bar[0] == 0 && bar[1] == 0 && bar[2] == 0));
            const std::uint8_t *above = rb.at(150, 30);
            CHECK((above[0] == 0 && above[1] == 0 && above[2] == 0));
        } else {
            CHECK(bar[0] + bar[1] + bar[2] > 24); // the blur, around the wide picture
        }
    }
    // A narrow view (a menu, a narrow map): no wings, the wide picture's sides are bars again.
    p.set_border(BorderMode::kColor);
    REQUIRE(p.upload_wings(gl.data(), 568, 320, 480));
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    std::string report;
    CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
    const Readback rb(w.renderer);
    const std::uint8_t *side = rb.at(150, 500);
    CHECK((side[0] == 0 && side[1] == 0 && side[2] == 0));
    CHECK_FALSE(p.picture_drawn()); // no wings: nothing composed, the steps as before
    p.stop();
}

// Widescreen's wide picture (#170, ADR 0090): with the wings drawn, the Presenter composes the
// GL frame's columns, the screen image over their middle and the key pad's strips into one
// picture, so the scale mode's filter and the colour-blind filter see one image.
namespace {

// A continuous frame 568 points wide at render scale k, and the screen image cut from its middle
// 480 points: drawn as wings + image, it must look like the frame drawn as one image.
struct WideFrame {
    int k = 1;
    std::vector<std::uint8_t> whole, screen;
};
WideFrame wide_frame(int k) {
    WideFrame f;
    f.k = k;
    f.whole = unique_image(568 * k, 320 * k);
    f.screen.resize(static_cast<std::size_t>(480 * k) * 320 * k * 4);
    for (int y = 0; y < 320 * k; y++) {
        std::memcpy(&f.screen[static_cast<std::size_t>(y) * 480 * k * 4],
                    &f.whole[(static_cast<std::size_t>(y) * 568 * k + 44 * k) * 4],
                    static_cast<std::size_t>(480 * k) * 4);
    }
    return f;
}

SDL_ScaleMode sdl_scale(fe::Filter f) {
    return f == fe::Filter::kLinear     ? SDL_SCALEMODE_LINEAR
           : f == fe::Filter::kPixelArt ? SDL_SCALEMODE_PIXELART
                                        : SDL_SCALEMODE_NEAREST;
}

// `img` (w x h) drawn once into `r` with the filter, over black: what the window shows when the
// picture is one image.
Readback one_image(SDL_Renderer *renderer, const std::vector<std::uint8_t> &img, int w, int h,
                   const fe::Rect &r, fe::Filter filter) {
    SDL_Texture *t =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    REQUIRE_MESSAGE(t != nullptr, SDL_GetError());
    REQUIRE(SDL_UpdateTexture(t, nullptr, img.data(), w * 4));
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(t, sdl_scale(filter));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    const SDL_FRect dst{r.x, r.y, r.w, r.h};
    REQUIRE(SDL_RenderTexture(renderer, t, nullptr, &dst));
    Readback rb(renderer);
    SDL_DestroyTexture(t);
    return rb;
}

} // namespace

TEST_CASE("border: widescreen's wide picture is one image, in every scale mode and graded (#170)") {
    for (const Size size : {Size{1920, 1080}, Size{2560, 1080}, Size{1440, 900}}) {
        Window w(size.w, size.h);
        for (const int k : {1, 2}) {
            const WideFrame f = wide_frame(k);
            const auto uk = static_cast<std::uint32_t>(k);
            for (const ScaleMode mode : kModes) {
                for (const fe::ColorblindFilter cb :
                     {fe::ColorblindFilter::kOff, fe::ColorblindFilter::kDeuteranopia}) {
                    CAPTURE(size.w);
                    CAPTURE(k);
                    CAPTURE(fe::scale_mode_name(mode));
                    CAPTURE(fe::colorblind_filter_name(cb));
                    fe::Presenter p;
                    REQUIRE(p.start(w.renderer, 480 * uk, 320 * uk));
                    p.set_scaling(mode, fe::Color{});
                    p.set_colorblind(cb);
                    p.set_wide(568);
                    REQUIRE(p.upload_wings(f.whole.data(), 568 * uk, 320 * uk, 568));
                    REQUIRE(p.upload(f.screen.data(), 480 * uk * 4));
                    REQUIRE(p.draw());
                    REQUIRE(p.picture_drawn());
                    CHECK(p.picture_width() == 568 * uk);
                    CHECK(p.picture_rect().x == p.wide_rect().x);
                    CHECK(p.picture_rect().w == p.wide_rect().w);
                    std::string report;
                    CHECK_MESSAGE(p.check_readback(f.screen.data(), 480 * uk * 4, &report) == 0,
                                  report);
                    CHECK(report.find("wide picture") != std::string::npos);
                    const Readback got(w.renderer);
                    // The same frame, graded on the CPU as a whole, drawn as one image.
                    std::vector<std::uint8_t> ref = f.whole;
                    if (cb != fe::ColorblindFilter::kOff) {
                        CHECK(p.color_path() == fe::Presenter::ColorPath::kCpu);
                        fe::ColorTransform t;
                        t.set(cb);
                        t.apply(ref.data(), 568 * uk * 4, ref.data(), 568 * uk * 4, 568 * uk,
                                320 * uk);
                    }
                    const Readback one = one_image(w.renderer, ref, 568 * k, 320 * k,
                                                   p.picture_rect(), p.placement().filter);
                    CHECK(got.px == one.px); // no seam: byte for byte the one image
                    p.stop();
                }
            }
        }
    }
}

TEST_CASE("border: the wide picture holds the key pad's strips, graded, and check_readback sees "
          "the wings (#170)") {
    Window w(1920, 1080);
    // The engine drew 520 of 568 points: the wings are 20 points a side, and the wide picture's
    // outer 24 points a side are bars.
    std::vector<std::uint8_t> gl = unique_image(568, 320);
    const std::vector<std::uint8_t> screen = unique_image(480, 320);
    // The strips (44 points a side): keys in the 20 points next to the screen image, half
    // transparent at their edges, nothing beyond (as the pad's anchored move leaves them).
    std::vector<std::uint8_t> strips(88 * 320 * 4, 0);
    for (int y = 100; y < 200; y++) {
        for (int x = 26; x < 62; x++) {
            const bool edge = x == 26 || x == 61 || y == 100 || y == 199;
            const unsigned a = edge ? 128 : 255;
            std::uint8_t *q = &strips[(static_cast<std::size_t>(y) * 88 + x) * 4];
            q[0] = static_cast<std::uint8_t>(a * 220 / 255);
            q[1] = static_cast<std::uint8_t>(a * 40 / 255);
            q[2] = static_cast<std::uint8_t>(a * 30 / 255);
            q[3] = static_cast<std::uint8_t>(a);
        }
    }
    for (const fe::ColorblindFilter cb :
         {fe::ColorblindFilter::kOff, fe::ColorblindFilter::kProtanopia}) {
        CAPTURE(fe::colorblind_filter_name(cb));
        fe::Presenter p;
        REQUIRE(p.start(w.renderer, 480, 320));
        p.set_scaling(ScaleMode::kInteger, fe::Color{});
        p.set_colorblind(cb);
        p.set_wide(568);
        REQUIRE(p.upload_wings(gl.data(), 568, 320, 520));
        REQUIRE(p.upload_pad_wings(strips.data(), 88, 320));
        REQUIRE(p.upload(screen.data(), 480 * 4));
        REQUIRE(p.draw());
        REQUIRE(p.picture_drawn());
        // Integer 3x: the wide picture at (108, 60) (1704 wide), the 520-point picture at x 180.
        CHECK(p.picture_width() == 520);
        CHECK(p.picture_rect().x == 180);
        CHECK(p.picture_rect().w == 1560);
        std::string report;
        CHECK_MESSAGE(p.check_readback(screen.data(), 480 * 4, &report) == 0, report);
        CHECK(report.find("wings checked against the GL frame") != std::string::npos);
        const Readback rb(w.renderer);
        fe::ColorTransform t;
        t.set(cb);
        // A key's opaque middle: the strip's colour, graded as the picture is.
        const std::uint8_t red[4] = {220, 40, 30, 255};
        std::uint8_t want[4];
        t.apply(red, 4, want, 4, 1, 1);
        for (const int point : {30, 40, 527, 535}) {
            CAPTURE(point);
            const std::uint8_t *q = rb.at(108 + point * 3 + 1, 60 + 150 * 3 + 1);
            CHECK((q[0] == want[0] && q[1] == want[1] && q[2] == want[2]));
        }
        // A wing beside the key (no strip there): the GL frame's texel, graded.
        const std::uint8_t *g = &gl[(static_cast<std::size_t>(50) * 568 + 2) * 4];
        std::uint8_t wing[4];
        t.apply(g, 4, wing, 4, 1, 1);
        const std::uint8_t *q = rb.at(180 + 2 * 3 + 1, 60 + 50 * 3 + 1);
        CHECK((q[0] == wing[0] && q[1] == wing[1] && q[2] == wing[2]));
        // The wide picture's points beyond the view: bars.
        const std::uint8_t *bar = rb.at(150, 500);
        CHECK((bar[0] == 0 && bar[1] == 0 && bar[2] == 0));
        // check_readback compares the wings with the uploaded frame: a changed wing texel and a
        // changed screen texel are each caught.
        gl[(static_cast<std::size_t>(50) * 568 + 2) * 4] ^= 0x40;
        CHECK(p.check_readback(screen.data(), 480 * 4, &report) > 0);
        CHECK(report.find("wings (texel)") != std::string::npos);
        gl[(static_cast<std::size_t>(50) * 568 + 2) * 4] ^= 0x40;
        std::vector<std::uint8_t> other = screen;
        other[(static_cast<std::size_t>(10) * 480 + 10) * 4 + 1] ^= 0x40;
        CHECK(p.check_readback(other.data(), 480 * 4, &report) > 0);
        p.stop();
    }
}
