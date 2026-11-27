// The overlays' drawing surface and its 5x7 font (#70, ADR 0054); see canvas.h.
#include "canvas.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "maplethief/cg.h"

namespace maplethief::frontend {

namespace {

using i64 = std::int64_t;

// The port's own 5x7 pixel font for printable ASCII (0x20-0x7E), drawn for this project:
// rows of five pixels per character, '#' set; capitals and digits are seven rows high, and g j
// p q y reach into an eighth. A character cell is 6 x 9 points with the gaps.
constexpr const char *kFont[95][8] = {
    {".....", ".....", ".....", ".....", ".....", ".....", ".....", "....."}, // ' '
    {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#..", "....."}, // !
    {".#.#.", ".#.#.", ".....", ".....", ".....", ".....", ".....", "....."}, // "
    {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#.", "....."}, // #
    {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..", "....."}, // $
    {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##", "....."}, // %
    {".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#", "....."}, // &
    {"..#..", "..#..", ".....", ".....", ".....", ".....", ".....", "....."}, // '
    {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#.", "....."}, // (
    {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#...", "....."}, // )
    {".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", ".....", "....."}, // *
    {".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....", "....."}, // +
    {".....", ".....", ".....", ".....", ".##..", "..#..", ".#...", "....."}, // ,
    {".....", ".....", ".....", "#####", ".....", ".....", ".....", "....."}, // -
    {".....", ".....", ".....", ".....", ".....", ".##..", ".##..", "....."}, // .
    {".....", "....#", "...#.", "..#..", ".#...", "#....", ".....", "....."}, // /
    {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.", "....."}, // 0
    {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."}, // 1
    {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####", "....."}, // 2
    {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###.", "....."}, // 3
    {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.", "....."}, // 4
    {"#####", "#....", "####.", "....#", "....#", "#...#", ".###.", "....."}, // 5
    {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.", "....."}, // 6
    {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...", "....."}, // 7
    {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.", "....."}, // 8
    {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..", "....."}, // 9
    {".....", ".##..", ".##..", ".....", ".##..", ".##..", ".....", "....."}, // :
    {".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#...", "....."}, // ;
    {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#.", "....."}, // <
    {".....", ".....", "#####", ".....", "#####", ".....", ".....", "....."}, // =
    {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#...", "....."}, // >
    {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..", "....."}, // ?
    {".###.", "#...#", "....#", ".##.#", "#.#.#", "#.#.#", ".###.", "....."}, // @
    {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "....."}, // A
    {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.", "....."}, // B
    {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.", "....."}, // C
    {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###..", "....."}, // D
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####", "....."}, // E
    {"#####", "#....", "#....", "####.", "#....", "#....", "#....", "....."}, // F
    {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####", "....."}, // G
    {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "....."}, // H
    {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."}, // I
    {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..", "....."}, // J
    {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", "....."}, // K
    {"#....", "#....", "#....", "#....", "#....", "#....", "#####", "....."}, // L
    {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#", "....."}, // M
    {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "....."}, // N
    {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "....."}, // O
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#....", "....."}, // P
    {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", "....."}, // Q
    {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", "....."}, // R
    {".####", "#....", "#....", ".###.", "....#", "....#", "####.", "....."}, // S
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "....."}, // T
    {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "....."}, // U
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", "....."}, // V
    {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#.", "....."}, // W
    {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#", "....."}, // X
    {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", "....."}, // Y
    {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####", "....."}, // Z
    {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###.", "....."}, // [
    {".....", "#....", ".#...", "..#..", "...#.", "....#", ".....", "....."}, // backslash
    {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###.", "....."}, // ]
    {"..#..", ".#.#.", "#...#", ".....", ".....", ".....", ".....", "....."}, // ^
    {".....", ".....", ".....", ".....", ".....", ".....", "#####", "....."}, // _
    {".#...", "..#..", ".....", ".....", ".....", ".....", ".....", "....."}, // `
    {".....", ".....", ".###.", "....#", ".####", "#...#", ".####", "....."}, // a
    {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "####.", "....."}, // b
    {".....", ".....", ".###.", "#....", "#....", "#...#", ".###.", "....."}, // c
    {"....#", "....#", ".##.#", "#..##", "#...#", "#...#", ".####", "....."}, // d
    {".....", ".....", ".###.", "#...#", "#####", "#....", ".###.", "....."}, // e
    {"..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#...", "....."}, // f
    {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", ".###."}, // g
    {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#", "....."}, // h
    {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###.", "....."}, // i
    {"...#.", ".....", "..##.", "...#.", "...#.", "...#.", "#..#.", ".##.."}, // j
    {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "....."}, // k
    {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."}, // l
    {".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#", "....."}, // m
    {".....", ".....", "#.##.", "##..#", "#...#", "#...#", "#...#", "....."}, // n
    {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###.", "....."}, // o
    {".....", ".....", "####.", "#...#", "#...#", "####.", "#....", "#...."}, // p
    {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#"}, // q
    {".....", ".....", "#.##.", "##..#", "#....", "#....", "#....", "....."}, // r
    {".....", ".....", ".###.", "#....", ".###.", "....#", "####.", "....."}, // s
    {".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##.", "....."}, // t
    {".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#", "....."}, // u
    {".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#..", "....."}, // v
    {".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", "....."}, // w
    {".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "....."}, // x
    {".....", ".....", "#...#", "#...#", "#...#", ".####", "....#", ".###."}, // y
    {".....", ".....", "#####", "...#.", "..#..", ".#...", "#####", "....."}, // z
    {"...#.", "..#..", "..#..", ".#...", "..#..", "..#..", "...#.", "....."}, // {
    {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "....."}, // |
    {".#...", "..#..", "..#..", "...#.", "..#..", "..#..", ".#...", "....."}, // }
    {".....", ".....", ".#...", "#.#.#", "...#.", ".....", ".....", "....."}, // ~
};

} // namespace

Canvas::Canvas(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale)
    : px_(rgba), width_(static_cast<int>(width)), height_(static_cast<int>(height)),
      scale_(std::max(1, scale)) {}

void Canvas::blend(int px, int py, Rgba c) {
    if (px < 0 || py < 0 || px >= width_ || py >= height_ || c.a == 0) {
        return;
    }
    std::uint8_t *p = px_ + (static_cast<std::size_t>(py) * static_cast<std::size_t>(width_) +
                             static_cast<std::size_t>(px)) *
                                4;
    const unsigned a = c.a;
    const auto mix = [a](unsigned src, unsigned dst) {
        return static_cast<std::uint8_t>((src * a + dst * (255u - a) + 127u) / 255u);
    };
    p[0] = mix(c.r, p[0]);
    p[1] = mix(c.g, p[1]);
    p[2] = mix(c.b, p[2]);
    p[3] = mix(255u, p[3]); // "over": an opaque image stays opaque (255)
}

void Canvas::fill(int x, int y, int w, int h, Rgba c) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const int x0 = std::max(0, x * scale_), y0 = std::max(0, y * scale_);
    const int x1 = std::min(width_, (x + w) * scale_), y1 = std::min(height_, (y + h) * scale_);
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            blend(px, py, c);
        }
    }
}

void Canvas::frame(int x, int y, int w, int h, int t, Rgba c) {
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y + t, t, h - 2 * t, c);
    fill(x + w - t, y + t, t, h - 2 * t, c);
}

// The shapes test each pixel's centre in doubled pixel units: pixel (px, py) is at
// (2 px + 1, 2 py + 1), and a point coordinate v is at 2 * scale * v.

void Canvas::disc(int x, int y, int d, Rgba c) { ring(x, y, d, d, c); }

void Canvas::ring(int x, int y, int d, int t, Rgba c) {
    const i64 s = scale_;
    const i64 cx = s * (2 * x + d), cy = s * (2 * y + d);
    const i64 r = s * d, rin = s * std::max(0, d - 2 * t);
    for (int py = y * scale_; py < (y + d) * scale_; py++) {
        for (int px = x * scale_; px < (x + d) * scale_; px++) {
            const i64 dx = 2 * px + 1 - cx, dy = 2 * py + 1 - cy;
            const i64 q = dx * dx + dy * dy;
            if (q <= r * r && (t >= d || q > rin * rin)) {
                blend(px, py, c);
            }
        }
    }
}

void Canvas::pill(int x, int y, int w, int h, Rgba c) {
    const i64 s = scale_;
    const i64 r = s * h; // the radius h / 2 in doubled units
    const i64 left = 2 * s * x + r, right = 2 * s * (x + w) - r, cy = s * (2 * y + h);
    for (int py = y * scale_; py < (y + h) * scale_; py++) {
        for (int px = x * scale_; px < (x + w) * scale_; px++) {
            const i64 X = 2 * px + 1;
            const i64 nx = X < left ? left : (X > right ? right : X);
            const i64 dx = X - nx, dy = 2 * py + 1 - cy;
            if (dx * dx + dy * dy <= r * r) {
                blend(px, py, c);
            }
        }
    }
}

void Canvas::cross(int x, int y, int d, int t, Rgba c) {
    const i64 s = scale_;
    const i64 cx = s * (2 * x + d), cy = s * (2 * y + d);
    // Perpendicular distance from a diagonal within t / 2: (u - v)^2 / 2 <= (s t)^2.
    const i64 limit = 2 * s * s * t * t;
    for (int py = y * scale_; py < (y + d) * scale_; py++) {
        for (int px = x * scale_; px < (x + d) * scale_; px++) {
            const i64 u = 2 * px + 1 - cx, v = 2 * py + 1 - cy;
            if ((u - v) * (u - v) <= limit || (u + v) * (u + v) <= limit) {
                blend(px, py, c);
            }
        }
    }
}

namespace {

// The triangle (apex top centre) of the box in doubled units, its edges oriented so that the
// inside is positive: e(P) = (B - A) x (P - A).
struct Tri {
    i64 x[3], y[3];
};
Tri tri_of(int scale, int x, int y, int w, int h) {
    const i64 s = scale;
    return Tri{{s * (2 * x + w), 2 * s * x, 2 * s * (x + w)},
               {2 * s * y, 2 * s * (y + h), 2 * s * (y + h)}};
}
i64 edge(const Tri &t, int i, i64 px, i64 py) {
    const int j = (i + 1) % 3;
    // Apex -> bottom left -> bottom right runs counter-clockwise on screen (y down): flip it.
    return -((t.x[j] - t.x[i]) * (py - t.y[i]) - (t.y[j] - t.y[i]) * (px - t.x[i]));
}
i64 edge_length_sq(const Tri &t, int i) {
    const int j = (i + 1) % 3;
    const i64 dx = t.x[j] - t.x[i], dy = t.y[j] - t.y[i];
    return dx * dx + dy * dy;
}

} // namespace

void Canvas::triangle(int x, int y, int w, int h, Rgba c) { triangle_outline(x, y, w, h, -1, c); }

void Canvas::triangle_outline(int x, int y, int w, int h, int t, Rgba c) {
    const Tri tr = tri_of(scale_, x, y, w, h);
    const i64 thick = 2 * static_cast<i64>(scale_) * t; // t points, doubled
    for (int py = y * scale_; py < (y + h) * scale_; py++) {
        for (int px = x * scale_; px < (x + w) * scale_; px++) {
            const i64 X = 2 * px + 1, Y = 2 * py + 1;
            bool inside = true, deep = true;
            for (int i = 0; i < 3; i++) {
                const i64 e = edge(tr, i, X, Y);
                inside = inside && e >= 0;
                // Distance to the edge e / |edge| at least `thick`: e^2 >= thick^2 |edge|^2.
                deep = deep && e > 0 && e * e >= thick * thick * edge_length_sq(tr, i);
            }
            if (inside && (t < 0 || !deep)) {
                blend(px, py, c);
            }
        }
    }
}

namespace {

// A bundled font's glyph at one size (mt_cg_bundled_glyph), kept: the overlays draw the same
// few characters frame after frame.
struct BundledGlyph {
    bool ok = false;
    mt_cg_glyph g{};
    std::vector<std::uint8_t> coverage;
};

const BundledGlyph &bundled_glyph(char32_t cp, int em, int max_width) {
    static std::mutex lock;
    static std::map<std::tuple<char32_t, int, int>, BundledGlyph> cache;
    const std::lock_guard<std::mutex> hold(lock);
    if (cache.size() >= 4096) {
        cache.clear();
    }
    const auto key = std::make_tuple(cp, em, max_width);
    auto it = cache.find(key);
    if (it == cache.end()) {
        BundledGlyph b;
        b.ok = mt_cg_bundled_glyph(cp, em, max_width, &b.g, nullptr, 0) != 0;
        b.coverage.resize(static_cast<std::size_t>(b.g.width) *
                          static_cast<std::size_t>(b.g.height));
        if (b.ok && !b.coverage.empty()) {
            mt_cg_bundled_glyph(cp, em, max_width, &b.g, b.coverage.data(), b.coverage.size());
        }
        it = cache.emplace(key, std::move(b)).first;
    }
    return it->second; // std::map's nodes stay put until the cache is cleared, under the lock
}

} // namespace

namespace {
TextObserver g_text_observer = nullptr;
void *g_text_observer_user = nullptr;
} // namespace

void set_text_observer(TextObserver fn, void *user) {
    g_text_observer = fn;
    g_text_observer_user = user;
}

std::int32_t next_code_point(std::string_view s, std::size_t *at) {
    const std::size_t i = *at;
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        *at = i + 1;
        return c;
    }
    std::size_t len = 0;
    std::uint32_t cp = 0, min = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2, cp = c & 0x1Fu, min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3, cp = c & 0x0Fu, min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4, cp = c & 0x07u, min = 0x10000;
    }
    bool ok = len != 0 && i + len <= s.size();
    for (std::size_t k = 1; ok && k < len; k++) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        ok = (b & 0xC0u) == 0x80u;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    ok = ok && cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp < 0xE000);
    *at = i + (ok ? len : 1);
    return ok ? static_cast<std::int32_t>(cp) : -1;
}

int Canvas::char_columns(char32_t cp) {
    const auto in = [cp](char32_t lo, char32_t hi) { return cp >= lo && cp <= hi; };
    const bool wide = in(0x1100, 0x115F) || in(0x2E80, 0x303E) || in(0x3041, 0x33FF) ||
                      in(0x3400, 0x4DBF) || in(0x4E00, 0x9FFF) || in(0xA960, 0xA97F) ||
                      in(0xAC00, 0xD7A3) || in(0xF900, 0xFAFF) || in(0xFE30, 0xFE4F) ||
                      in(0xFF00, 0xFF60) || in(0xFFE0, 0xFFE6) || in(0x20000, 0x3FFFD);
    return wide ? 2 : 1;
}

int Canvas::columns(std::string_view s) {
    int cols = 0;
    for (std::size_t at = 0; at < s.size();) {
        const std::int32_t cp = next_code_point(s, &at);
        cols += cp < 0 ? 1 : char_columns(static_cast<char32_t>(cp));
    }
    return cols;
}

std::size_t Canvas::fit_columns(std::string_view s, int cols) {
    int used = 0;
    std::size_t at = 0;
    while (at < s.size()) {
        std::size_t next = at;
        const std::int32_t cp = next_code_point(s, &next);
        used += cp < 0 ? 1 : char_columns(static_cast<char32_t>(cp));
        if (used > cols) {
            break;
        }
        at = next;
    }
    return at;
}

std::string Canvas::fit_width(std::string_view s, int max_w, int size) {
    if (text_width(s, size) <= max_w) {
        return std::string(s);
    }
    const int cols = (max_w + size) / (6 * size) - 3; // "..." takes three cells
    if (cols < 0) {
        return {};
    }
    return std::string(s.substr(0, fit_columns(s, cols))) + "...";
}

int Canvas::text(int x, int y, std::string_view s, Rgba c, int size) {
    int pen = x;
    int missing = 0;
    for (std::size_t at = 0; at < s.size();) {
        const std::int32_t cp = next_code_point(s, &at);
        const int cells = cp < 0 ? 1 : char_columns(static_cast<char32_t>(cp));
        unsigned index = '?' - 0x20;
        if (cp >= 0x20 && cp <= 0x7E) {
            index = static_cast<unsigned>(cp) - 0x20;
        } else if (cp >= 0xA0) {
            // The bundled fonts, at the image's pixels: centred in the cells' span (their last
            // gap left out), on the 5x7 font's baseline.
            const int span = (cells * 6 - 1) * size * scale_;
            const BundledGlyph &b =
                bundled_glyph(static_cast<char32_t>(cp), kFontEm * size * scale_, span);
            if (b.ok) {
                const int left = pen * scale_ + (span - b.g.width) / 2;
                const int top = (y + 7 * size) * scale_ + b.g.top;
                for (int gy = 0; gy < b.g.height; gy++) {
                    for (int gx = 0; gx < b.g.width; gx++) {
                        const unsigned cov = b.coverage[static_cast<std::size_t>(gy) *
                                                            static_cast<std::size_t>(b.g.width) +
                                                        static_cast<std::size_t>(gx)];
                        if (cov != 0) {
                            blend(left + gx, top + gy,
                                  {c.r, c.g, c.b, static_cast<std::uint8_t>(c.a * cov / 255)});
                        }
                    }
                }
                pen += 6 * size * cells;
                continue;
            }
        }
        if (cp != '?' && index == '?' - 0x20) {
            missing++;
        }
        for (int gy = 0; gy < kGlyphHeight; gy++) {
            for (int gx = 0; gx < 5; gx++) {
                if (kFont[index][gy][gx] == '#') {
                    fill(pen + gx * size, y + gy * size, size, size, c);
                }
            }
        }
        pen += 6 * size * cells;
    }
    const int w = text_width(s, size);
    if (g_text_observer != nullptr) {
        g_text_observer(g_text_observer_user, {x, y, w, kGlyphHeight * size, s, missing});
    }
    return w;
}

int Canvas::text_width(std::string_view s, int size) {
    return s.empty() ? 0 : columns(s) * 6 * size - size;
}

} // namespace maplethief::frontend
