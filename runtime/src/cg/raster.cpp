// The software rasterizer behind CoreGraphics (#51, ADR 0036): pixel formats, the graphics
// state, anti-aliased polygon coverage (exact area coverage, nonzero rule), solid fills,
// gradients, images and glyph coverage, composited with the Normal, Copy or Clear blend mode
// through the device clip mask.
//
// Coverage uses signed-area accumulation (as in font-rs and stb_truetype's v2 rasterizer):
// each edge adds its signed area to the cells it crosses; a running sum along each row gives
// the winding-weighted coverage, clamped to 1, which is the nonzero rule for the paths CG
// draws here (rectangles, rounded rectangles, strokes as consistently oriented quads).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "cg/cg_objects.h"

namespace mt_cg {

namespace {

bool g_shortcuts = true; // set_raster_shortcuts

// x * y / 255, rounded.
inline u32 mul255(u32 x, u32 y) {
    const u32 t = x * y + 128u;
    return (t + (t >> 8)) >> 8;
}

inline u8 to_byte(double v) {
    return static_cast<u8>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

Pixel scale(Pixel p, u32 c) {
    return {static_cast<u8>(mul255(p.r, c)), static_cast<u8>(mul255(p.g, c)),
            static_cast<u8>(mul255(p.b, c)), static_cast<u8>(mul255(p.a, c))};
}

Pixel blend(Pixel d, Pixel s, u32 cov, u32 mode) {
    switch (mode) {
    case k_blend_copy: {
        const Pixel a = scale(s, cov), b = scale(d, 255u - cov);
        return {static_cast<u8>(a.r + b.r), static_cast<u8>(a.g + b.g), static_cast<u8>(a.b + b.b),
                static_cast<u8>(a.a + b.a)};
    }
    case k_blend_clear:
        return scale(d, 255u - cov);
    default: {
        // Shortcuts with the same bytes: scale(x, 255) is x and scale(x, 0) is 0.
        if (g_shortcuts) {
            if (cov == 255u && s.a == 255u) {
                return s;
            }
            if ((s.r | s.g | s.b | s.a) == 0) {
                return d;
            }
        }
        const Pixel a = scale(s, cov);
        const Pixel b = scale(d, 255u - a.a);
        return {
            static_cast<u8>(std::min(255, a.r + b.r)), static_cast<u8>(std::min(255, a.g + b.g)),
            static_cast<u8>(std::min(255, a.b + b.b)), static_cast<u8>(std::min(255, a.a + b.a))};
    }
    }
}

// --- coverage ----------------------------------------------------------------------------------

// Coverage of a raster-space path over the box [x0, x1) x [y0, y1) of a w x h surface.
// `full`: every pixel of the box is covered (255), and `cov` is empty.
struct Coverage {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<u8> cov;
    bool full = false;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    u8 at(int x, int y) const {
        return full ? u8{255} : cov[static_cast<std::size_t>(y - y0) * (x1 - x0) + (x - x0)];
    }
};

class Accumulator {
  public:
    Accumulator(int w, int h) : w_(w), h_(h), a_(static_cast<std::size_t>(w) * h + 4, 0.0f) {}

    // An edge in accumulator coordinates (0 <= x <= w); y is clipped here.
    void line(Point p0, Point p1) {
        if (std::fabs(p0.y - p1.y) <= 1e-9) {
            return;
        }
        float dir = 1.0f;
        if (p0.y > p1.y) {
            std::swap(p0, p1);
            dir = -1.0f;
        }
        const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
        double x = p0.x;
        if (p0.y < 0) {
            x -= p0.y * dxdy;
        }
        const int ystart = std::max(0, static_cast<int>(std::floor(p0.y)));
        const int yend = std::min(h_, static_cast<int>(std::ceil(p1.y)));
        for (int y = ystart; y < yend; y++) {
            const std::size_t row = static_cast<std::size_t>(y) * w_;
            const double dy =
                std::min(static_cast<double>(y + 1), p1.y) - std::max(static_cast<double>(y), p0.y);
            const double xnext = x + dxdy * dy;
            const float d = static_cast<float>(dy) * dir;
            const double lo = std::min(x, xnext), hi = std::max(x, xnext);
            const double lofloor = std::floor(lo);
            const int loi = static_cast<int>(lofloor);
            const double hiceil = std::ceil(hi);
            const int hii = static_cast<int>(hiceil);
            if (hii <= loi + 1) {
                const float xmf = static_cast<float>(0.5 * (x + xnext) - lofloor);
                add(row + loi, d - d * xmf);
                add(row + loi + 1, d * xmf);
            } else {
                const double s = 1.0 / (hi - lo);
                const double x0f = lo - lofloor;
                const double a0 = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
                const double x1f = hi - hiceil + 1.0;
                const double am = 0.5 * s * x1f * x1f;
                add(row + loi, static_cast<float>(d * a0));
                if (hii == loi + 2) {
                    add(row + loi + 1, static_cast<float>(d * (1.0 - a0 - am)));
                } else {
                    const double a1 = s * (1.5 - x0f);
                    add(row + loi + 1, static_cast<float>(d * (a1 - a0)));
                    for (int xi = loi + 2; xi < hii - 1; xi++) {
                        add(row + xi, static_cast<float>(d * s));
                    }
                    const double a2 = a1 + (hii - loi - 3) * s;
                    add(row + hii - 1, static_cast<float>(d * (1.0 - a2 - am)));
                }
                add(row + hii, static_cast<float>(d * am));
            }
            x = xnext;
        }
    }

    // An edge that may leave [0, w] in x: the parts outside are moved onto x = 0 or x = w
    // (a vertical edge there has the same effect on the cells inside).
    void clipped_line(Point p, Point q) {
        // The split points in order: 0, the crossings of x = 0 and x = w inside (0, 1), 1.
        // (Two crossings are ordered by hand: GCC 15's -O3 -Warray-bounds misreads std::sort
        // over this small array.)
        double ts[4] = {0, 0, 0, 0};
        int n = 1;
        const double dx = q.x - p.x;
        if (dx != 0) {
            for (const double edge : {0.0, static_cast<double>(w_)}) {
                const double t = (edge - p.x) / dx;
                if (t > 0 && t < 1) {
                    ts[n++] = t;
                }
            }
        }
        if (n == 3 && ts[2] < ts[1]) {
            std::swap(ts[1], ts[2]);
        }
        ts[n++] = 1;
        for (int i = 0; i + 1 < n; i++) {
            Point a{p.x + dx * ts[i], p.y + (q.y - p.y) * ts[i]};
            Point b{p.x + dx * ts[i + 1], p.y + (q.y - p.y) * ts[i + 1]};
            a.x = std::clamp(a.x, 0.0, static_cast<double>(w_));
            b.x = std::clamp(b.x, 0.0, static_cast<double>(w_));
            line(a, b);
        }
    }

    void finish(std::vector<u8> &out) const {
        out.resize(static_cast<std::size_t>(w_) * h_);
        float acc = 0;
        for (std::size_t i = 0; i < out.size(); i++) {
            acc += a_[i];
            const float v = std::min(1.0f, std::fabs(acc));
            out[i] = static_cast<u8>(v * 255.0f + 0.5f);
        }
    }

  private:
    void add(std::size_t i, float v) {
        if (i < a_.size()) {
            a_[i] += v;
        }
    }
    int w_, h_;
    std::vector<float> a_;
};

// Whether a raster-space path is one axis-aligned rectangle with whole-pixel corners: four
// points, edges alternately horizontal and vertical.
bool whole_pixel_rect(const Path &path) {
    if (path.subpaths.size() != 1 || path.subpaths[0].points.size() != 4) {
        return false;
    }
    const std::vector<Point> &p = path.subpaths[0].points;
    for (const Point &q : p) {
        if (q.x != std::floor(q.x) || q.y != std::floor(q.y)) {
            return false;
        }
    }
    const bool h0 = p[0].y == p[1].y && p[1].x == p[2].x && p[2].y == p[3].y && p[3].x == p[0].x;
    const bool v0 = p[0].x == p[1].x && p[1].y == p[2].y && p[2].x == p[3].x && p[3].y == p[0].y;
    return h0 || v0;
}

Coverage rasterize(const Path &raster_path, u32 width, u32 height) {
    Coverage c;
    double minx = std::numeric_limits<double>::infinity(), miny = minx;
    double maxx = -minx, maxy = -minx;
    for (const Subpath &sp : raster_path.subpaths) {
        for (const Point &p : sp.points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                return c; // a degenerate transform: nothing is drawn
            }
            minx = std::min(minx, p.x);
            maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y);
            maxy = std::max(maxy, p.y);
        }
    }
    if (!(minx <= maxx)) {
        return c;
    }
    c.x0 = static_cast<int>(std::clamp(std::floor(minx), 0.0, static_cast<double>(width)));
    c.x1 = static_cast<int>(std::clamp(std::ceil(maxx), 0.0, static_cast<double>(width)));
    c.y0 = static_cast<int>(std::clamp(std::floor(miny), 0.0, static_cast<double>(height)));
    c.y1 = static_cast<int>(std::clamp(std::ceil(maxy), 0.0, static_cast<double>(height)));
    if (c.empty()) {
        return c;
    }
    if (g_shortcuts && whole_pixel_rect(raster_path)) {
        // What the accumulator gives such a rectangle: its vertical edges add exactly +-1 per
        // row at whole cells (the right one at the next row's first cell, where the box ends),
        // so the running sum is exactly 1 across the box: 255 everywhere.
        c.full = true;
        return c;
    }
    Accumulator acc(c.x1 - c.x0, c.y1 - c.y0);
    for (const Subpath &sp : raster_path.subpaths) {
        const std::size_t n = sp.points.size();
        for (std::size_t i = 0; i < n && n >= 2; i++) {
            const Point &a = sp.points[i], &b = sp.points[(i + 1) % n];
            acc.clipped_line({a.x - c.x0, a.y - c.y0}, {b.x - c.x0, b.y - c.y0});
        }
    }
    acc.finish(c.cov);
    return c;
}

Path transformed(const Path &p, const Affine &m) {
    Path out;
    out.subpaths.reserve(p.subpaths.size());
    for (const Subpath &sp : p.subpaths) {
        Subpath t;
        t.closed = sp.closed;
        t.points.reserve(sp.points.size());
        for (const Point &pt : sp.points) {
            t.points.push_back(m.apply(pt));
        }
        out.subpaths.push_back(std::move(t));
    }
    return out;
}

Path rect_path(const Rect &r) {
    Path p;
    p.move_to({r.x, r.y});
    p.line_to({r.x + r.w, r.y});
    p.line_to({r.x + r.w, r.y + r.h});
    p.line_to({r.x, r.y + r.h});
    p.close();
    return p;
}

// Composites shade(x, y) (a premultiplied colour) over the pixels of box [x0, x1) x [y0, y1)
// with coverage cov(x, y), through the clip.
template <typename Cov, typename Shade>
void composite(Surface &s, const GState &gs, u32 blend_mode, int x0, int y0, int x1, int y1,
               Cov cov, Shade shade) {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, static_cast<int>(s.width));
    y1 = std::min(y1, static_cast<int>(s.height));
    const std::vector<u8> *clip = gs.clip.get();
    const u32 alpha = gs.alpha >= 1 ? 255u : static_cast<u32>(to_byte(gs.alpha));
    if (alpha == 0) {
        return;
    }
    for (int y = y0; y < y1; y++) {
        u8 *row = s.pixels + static_cast<std::size_t>(y) * s.bytes_per_row;
        for (int x = x0; x < x1; x++) {
            u32 c = cov(x, y);
            if (clip != nullptr && c != 0) {
                c = mul255(c, (*clip)[static_cast<std::size_t>(y) * s.width + x]);
            }
            if (alpha != 255u) {
                c = mul255(c, alpha);
            }
            if (c == 0) {
                continue;
            }
            u8 *p = row + static_cast<std::size_t>(x) * s.format.bytes_per_pixel;
            store_pixel(s.format, p, blend(load_pixel(s.format, p), shade(x, y), c, blend_mode));
        }
    }
}

// Four pixels mixed with weights tx (p10, p11) and ty (p01, p11), rounded.
Pixel bilinear_mix(Pixel p00, Pixel p10, Pixel p01, Pixel p11, double tx, double ty) {
    auto mix = [&](u8 a, u8 b, u8 c, u8 d) {
        const double top = a + (b - a) * tx, bottom = c + (d - c) * tx;
        return static_cast<u8>(std::lround(top + (bottom - top) * ty));
    };
    return {mix(p00.r, p10.r, p01.r, p11.r), mix(p00.g, p10.g, p01.g, p11.g),
            mix(p00.b, p10.b, p01.b, p11.b), mix(p00.a, p10.a, p01.a, p11.a)};
}

// The pixel of `img` at continuous image coordinates (u, v) (pixel centres at +0.5), bilinear
// or nearest, clamped to the edges.
Pixel sample(const ConstSurface &img, double u, double v, bool bilinear) {
    auto at = [&](long x, long y) {
        x = std::clamp(x, 0L, static_cast<long>(img.width) - 1);
        y = std::clamp(y, 0L, static_cast<long>(img.height) - 1);
        return load_pixel(img.format, img.pixels + static_cast<std::size_t>(y) * img.bytes_per_row +
                                          static_cast<std::size_t>(x) * img.format.bytes_per_pixel);
    };
    if (!bilinear) {
        return at(static_cast<long>(std::floor(u)), static_cast<long>(std::floor(v)));
    }
    const double fx = u - 0.5, fy = v - 0.5;
    const long ix = static_cast<long>(std::floor(fx)), iy = static_cast<long>(std::floor(fy));
    const double tx = fx - static_cast<double>(ix), ty = fy - static_cast<double>(iy);
    return bilinear_mix(at(ix, iy), at(ix + 1, iy), at(ix, iy + 1), at(ix + 1, iy + 1), tx, ty);
}

// --- images without rotation or shear ------------------------------------------------------------
//
// draw_image's generic path inverts the map and samples for every pixel. When the inverse has
// no rotation or shear (b == c == 0, exactly), a pixel's u depends on its column only and its v
// on its row only: inv.apply computes a * x + c * y + tx, and c * y is a zero. So each column's
// and each row's taps and weights are worked out once, from the same expressions as sample(),
// and every pixel gets the same bytes as the generic path:
//
// - The taps are sample()'s clamped indices.
// - The weights: when every column's and row's fraction is a multiple of 1/256 (any integer
//   scale up to 256 with whole-pixel offsets, and nearest sampling, whose weights are 0),
//   sample()'s doubles are exact (8-bit values times 8-bit fractions), and lround of the
//   exact, non-negative result is the integer formula's floor(x + 1/2). Otherwise the doubles
//   are computed as sample() computes them.
// - In the Normal blend mode a sample of four zero bytes changes nothing (scale(d, 255) is d),
//   so only the columns whose taps reach a non-zero pixel in the rows tapped are visited.
//   Taps are monotonic in x, so a run of non-zero source pixels is one range of columns.
// - Coverage, clip and alpha are composite()'s.

bool zero(Pixel p) { return (p.r | p.g | p.b | p.a) == 0; }

struct Taps {
    std::vector<u32> i0, i1; // the two clamped source indices (equal for nearest)
    std::vector<double> t;   // the weight of i1 (0 for nearest)
    std::vector<u32> w;      // t * 256 when `exact`
    bool exact = true;
};

// The taps of sample positions `pos` (u or v of each column or row) into `size` pixels.
Taps make_taps(const std::vector<double> &pos, u32 size, bool bilinear) {
    Taps taps;
    const std::size_t n = pos.size();
    taps.i0.resize(n);
    taps.i1.resize(n);
    taps.t.resize(n);
    taps.w.resize(n);
    const long last = static_cast<long>(size) - 1;
    for (std::size_t k = 0; k < n; k++) {
        const double p = pos[k];
        if (!bilinear) {
            taps.i0[k] = taps.i1[k] =
                static_cast<u32>(std::clamp(static_cast<long>(std::floor(p)), 0L, last));
            continue;
        }
        const double fp = p - 0.5;
        const long ip = static_cast<long>(std::floor(fp));
        const double t = fp - static_cast<double>(ip);
        taps.i0[k] = static_cast<u32>(std::clamp(ip, 0L, last));
        taps.i1[k] = static_cast<u32>(std::clamp(ip + 1, 0L, last));
        taps.t[k] = t;
        const double w = t * 256;
        if (w == std::floor(w) && w >= 0 && w < 256) {
            taps.w[k] = static_cast<u32>(w);
        } else {
            taps.exact = false;
        }
    }
    return taps;
}

void draw_image_axis_aligned(Surface &s, const GState &gs, const Coverage &c,
                             const ConstSurface &img, const Affine &inv, bool bilinear) {
    const int x0 = std::max(c.x0, 0), y0 = std::max(c.y0, 0);
    const int x1 = std::min(c.x1, static_cast<int>(s.width));
    const int y1 = std::min(c.y1, static_cast<int>(s.height));
    const u32 alpha = gs.alpha >= 1 ? 255u : static_cast<u32>(to_byte(gs.alpha));
    if (alpha == 0 || x1 <= x0 || y1 <= y0) {
        return;
    }
    // inv.apply({x + 0.5, y + 0.5}) is {a * (x + 0.5) + c * (y + 0.5) + tx, ...} with c * (y +
    // 0.5) a zero, so u is the same in every row and v in every column. The positions come from
    // the same expression as the generic path's (so any contraction into an FMA is the same).
    std::vector<double> us(static_cast<std::size_t>(x1 - x0)),
        vs(static_cast<std::size_t>(y1 - y0));
    for (int x = x0; x < x1; x++) {
        us[static_cast<std::size_t>(x - x0)] = inv.apply({x + 0.5, y0 + 0.5}).x;
    }
    for (int y = y0; y < y1; y++) {
        vs[static_cast<std::size_t>(y - y0)] = inv.apply({x0 + 0.5, y + 0.5}).y;
    }
    const Taps cols = make_taps(us, img.width, bilinear);
    const Taps rows = make_taps(vs, img.height, bilinear);
    const bool exact = cols.exact && rows.exact;
    const std::vector<u8> *clip = gs.clip.get();
    const u32 bpp = img.format.bytes_per_pixel;
    auto at = [&](u32 x, u32 y) {
        return load_pixel(img.format, img.pixels + static_cast<std::size_t>(y) * img.bytes_per_row +
                                          static_cast<std::size_t>(x) * bpp);
    };

    const bool mixed = bilinear && exact; // weights mixed in integers, through vmix
    std::vector<u32> vmix(mixed ? 4 * static_cast<std::size_t>(img.width) : 0);
    // The destination is premultiplied RGBA in that byte order (the compositor's).
    const Format &df = s.format;
    auto plain_format = [](const Format &f) {
        return f.bytes_per_pixel == 4 && f.r == 0 && f.g == 1 && f.b == 2 && f.a == 3 &&
               f.skip < 0 && f.premultiplied;
    };
    const bool plain = plain_format(df);
    const bool copyable = !bilinear && plain && plain_format(img.format) &&
                          gs.blend == k_blend_normal && c.full && clip == nullptr && alpha == 255u;

    // The runs of non-zero pixels of each source row tapped, found on first use. Outside the
    // Normal mode every pixel counts, and so it does when storing a pixel back unchanged could
    // change its bytes (a format with no alpha or with straight alpha). Nearest sampling tests
    // each pixel instead: a scan would read as many pixels as it saves.
    const bool skip_zero = bilinear && gs.blend == k_blend_normal && s.format.a >= 0 &&
                           s.format.premultiplied && s.format.skip < 0;
    std::vector<std::vector<std::pair<u32, u32>>> runs(skip_zero ? img.height : 0);
    std::vector<u8> scanned(skip_zero ? img.height : 0, 0);
    auto row_runs = [&](u32 y) -> const std::vector<std::pair<u32, u32>> & {
        if (!scanned[y]) {
            scanned[y] = 1;
            std::vector<std::pair<u32, u32>> &out = runs[y];
            u32 x = 0;
            while (x < img.width) {
                while (x < img.width && zero(at(x, y))) {
                    x++;
                }
                const u32 start = x;
                while (x < img.width && !zero(at(x, y))) {
                    x++;
                }
                if (x > start) {
                    out.emplace_back(start, x);
                }
            }
        }
        return runs[y];
    };
    // The columns (relative to x0) whose taps reach source columns [s0, s1): i0 in
    // [s0 - 1, s1 - 1], since i1 is i0 or i0 + 1. i0 is monotonic in x.
    const bool rising = inv.a > 0;
    const std::size_t ncols = cols.i0.size();
    auto columns_of = [&](u32 s0, u32 s1) {
        const u32 lo = s0 > 0 ? s0 - 1 : 0, hi = s1 - 1;
        std::size_t first, end;
        if (rising) {
            first = static_cast<std::size_t>(std::lower_bound(cols.i0.begin(), cols.i0.end(), lo) -
                                             cols.i0.begin());
            end = static_cast<std::size_t>(std::upper_bound(cols.i0.begin(), cols.i0.end(), hi) -
                                           cols.i0.begin());
        } else {
            first = static_cast<std::size_t>(
                std::lower_bound(cols.i0.begin(), cols.i0.end(), hi, std::greater<u32>()) -
                cols.i0.begin());
            end = static_cast<std::size_t>(
                std::upper_bound(cols.i0.begin(), cols.i0.end(), lo, std::greater<u32>()) -
                cols.i0.begin());
        }
        return std::pair<std::size_t, std::size_t>(first, std::max(first, end));
    };

    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (int y = y0; y < y1; y++) {
        const std::size_t ry = static_cast<std::size_t>(y - y0);
        const u32 sy0 = rows.i0[ry], sy1 = rows.i1[ry];
        spans.clear();
        if (!skip_zero) {
            spans.emplace_back(0, ncols);
        } else {
            for (const u32 sy : {sy0, sy1}) {
                for (const auto &run : row_runs(sy)) {
                    const auto span = columns_of(run.first, run.second);
                    if (span.second > span.first) {
                        spans.push_back(span);
                    }
                }
                if (sy1 == sy0) {
                    break;
                }
            }
            if (spans.empty()) {
                continue;
            }
            std::sort(spans.begin(), spans.end());
            std::size_t n = 0;
            for (const auto &span : spans) {
                if (n > 0 && span.first <= spans[n - 1].second) {
                    spans[n - 1].second = std::max(spans[n - 1].second, span.second);
                } else {
                    spans[n++] = span;
                }
            }
            spans.resize(n);
        }
        u8 *row = s.pixels + static_cast<std::size_t>(y) * s.bytes_per_row;
        const double ty = rows.t[ry];
        const u32 wy = rows.w[ry];
        if (mixed) {
            // The two tapped rows mixed once per source column: with a = (i0, sy0), b = (i1,
            // sy0), c = (i0, sy1), d = (i1, sy1) and integer weights, sample()'s
            // (top * 256 + (bottom - top) * wy) with top = a * 256 + (b - a) * wx and
            // bottom = c * 256 + (d - c) * wx is (256 - wx) * V(i0) + wx * V(i1), where
            // V(j) = (j, sy0) * (256 - wy) + (j, sy1) * wy.
            for (const auto &span : spans) {
                const u32 j0 = std::min(cols.i0[span.first], cols.i0[span.second - 1]);
                const u32 j1 = std::max(cols.i1[span.first], cols.i1[span.second - 1]);
                for (u32 j = j0; j <= j1; j++) {
                    const Pixel top = at(j, sy0), bottom = at(j, sy1);
                    u32 *v = &vmix[4 * static_cast<std::size_t>(j)];
                    v[0] = top.r * (256 - wy) + bottom.r * wy;
                    v[1] = top.g * (256 - wy) + bottom.g * wy;
                    v[2] = top.b * (256 - wy) + bottom.b * wy;
                    v[3] = top.a * (256 - wy) + bottom.a * wy;
                }
            }
        }
        if (copyable) {
            // Nearest sampling between two plain surfaces, every pixel fully covered: the
            // Normal case below with cv = 255, reading the source bytes as they are.
            const u8 *srow = img.pixels + static_cast<std::size_t>(sy0) * img.bytes_per_row;
            for (const auto &span : spans) {
                for (std::size_t k = span.first; k < span.second; k++) {
                    const u8 *q = srow + 4 * static_cast<std::size_t>(cols.i0[k]);
                    u8 *p = row + 4 * static_cast<std::size_t>(x0 + static_cast<int>(k));
                    if (q[3] == 255u) {
                        std::memcpy(p, q, 4);
                    } else if ((q[0] | q[1] | q[2] | q[3]) != 0) {
                        const u32 keep = 255u - q[3];
                        for (int i = 0; i < 4; i++) {
                            p[i] = static_cast<u8>(std::min(255u, q[i] + mul255(p[i], keep)));
                        }
                    }
                }
            }
            continue;
        }
        for (const auto &span : spans) {
            for (std::size_t k = span.first; k < span.second; k++) {
                const int x = x0 + static_cast<int>(k);
                u32 cv = c.at(x, y);
                if (clip != nullptr && cv != 0) {
                    cv = mul255(cv, (*clip)[static_cast<std::size_t>(y) * s.width + x]);
                }
                if (alpha != 255u) {
                    cv = mul255(cv, alpha);
                }
                if (cv == 0) {
                    continue;
                }
                Pixel px;
                if (!bilinear) {
                    px = at(cols.i0[k], sy0);
                } else if (mixed) {
                    const u32 w1 = cols.w[k], w0 = 256 - w1;
                    const u32 *v0 = &vmix[4 * static_cast<std::size_t>(cols.i0[k])];
                    const u32 *v1 = &vmix[4 * static_cast<std::size_t>(cols.i1[k])];
                    px = {static_cast<u8>((w0 * v0[0] + w1 * v1[0] + 32768) >> 16),
                          static_cast<u8>((w0 * v0[1] + w1 * v1[1] + 32768) >> 16),
                          static_cast<u8>((w0 * v0[2] + w1 * v1[2] + 32768) >> 16),
                          static_cast<u8>((w0 * v0[3] + w1 * v1[3] + 32768) >> 16)};
                } else {
                    px = bilinear_mix(at(cols.i0[k], sy0), at(cols.i1[k], sy0), at(cols.i0[k], sy1),
                                      at(cols.i1[k], sy1), cols.t[k], ty);
                }
                u8 *p = row + static_cast<std::size_t>(x) * s.format.bytes_per_pixel;
                if (plain && gs.blend == k_blend_normal) {
                    // blend()'s Normal case written out for this format: px for an opaque pixel
                    // fully covered (scale(x, 255) is x, scale(x, 0) is 0), the pixel under it
                    // for a zero one (stored as it was), else a = scale(px, cv) over it.
                    if (cv == 255u && px.a == 255u) {
                        p[0] = px.r;
                        p[1] = px.g;
                        p[2] = px.b;
                        p[3] = 255;
                        continue;
                    }
                    if (zero(px)) {
                        continue;
                    }
                    const Pixel a = cv == 255u ? px : scale(px, cv);
                    const u32 keep = 255u - a.a;
                    p[0] = static_cast<u8>(std::min(255u, a.r + mul255(p[0], keep)));
                    p[1] = static_cast<u8>(std::min(255u, a.g + mul255(p[1], keep)));
                    p[2] = static_cast<u8>(std::min(255u, a.b + mul255(p[2], keep)));
                    p[3] = static_cast<u8>(std::min(255u, a.a + mul255(p[3], keep)));
                    continue;
                }
                store_pixel(s.format, p, blend(load_pixel(s.format, p), px, cv, gs.blend));
            }
        }
    }
}

// A solid fill over a fully covered box (no clip, alpha 1) whose result does not depend on the
// pixel under it: Clear (scale(d, 0) is 0), Copy (s), or Normal with an opaque colour (s). Every
// pixel of the box gets the bytes composite() would store. False if the fill is not like that.
bool fill_constant(Surface &s, const GState &gs, u32 mode, const Coverage &c, Pixel color) {
    const u32 alpha = gs.alpha >= 1 ? 255u : static_cast<u32>(to_byte(gs.alpha));
    if (!c.full || gs.clip != nullptr || alpha != 255u) {
        return false;
    }
    Pixel out;
    if (mode == k_blend_clear) {
        out = Pixel{0, 0, 0, 0};
    } else if (mode == k_blend_copy || color.a == 255u) {
        out = color;
    } else {
        return false;
    }
    u8 pattern[4] = {0, 0, 0, 0};
    store_pixel(s.format, pattern, out); // writes every byte of a pixel
    const int x0 = std::max(c.x0, 0), y0 = std::max(c.y0, 0);
    const int x1 = std::min(c.x1, static_cast<int>(s.width));
    const int y1 = std::min(c.y1, static_cast<int>(s.height));
    const u32 bpp = s.format.bytes_per_pixel;
    for (int y = y0; y < y1; y++) {
        u8 *p = s.pixels + static_cast<std::size_t>(y) * s.bytes_per_row +
                static_cast<std::size_t>(x0) * bpp;
        if (bpp == 4) {
            for (int x = x0; x < x1; x++, p += 4) {
                std::memcpy(p, pattern, 4);
            }
        } else {
            for (int x = x0; x < x1; x++, p += bpp) {
                std::memcpy(p, pattern, bpp);
            }
        }
    }
    return true;
}

// Whether an affine map is a whole-pixel translation with axis flips only.
bool pixel_aligned(const Affine &m) {
    auto whole = [](double v) { return std::fabs(v - std::round(v)) < 1e-6; };
    return std::fabs(m.b) < 1e-9 && std::fabs(m.c) < 1e-9 && std::fabs(std::fabs(m.a) - 1) < 1e-9 &&
           std::fabs(std::fabs(m.d) - 1) < 1e-9 && whole(m.tx) && whole(m.ty);
}

} // namespace

void set_raster_shortcuts(bool on) { g_shortcuts = on; }
bool raster_shortcuts() { return g_shortcuts; }

// --- formats -----------------------------------------------------------------------------------

bool format_of(u32 info, u32 bpc, u32 bpp, Format *out) {
    if (bpc != 8 || (info & k_float_components) != 0) {
        return false;
    }
    const u32 alpha = info & k_alpha_info_mask, order = info & k_byte_order_mask;
    Format f;
    if (bpp == 24) {
        if (alpha != k_alpha_none || (order != 0 && order != k_byte_order_32_big)) {
            return false;
        }
        f.bytes_per_pixel = 3;
        f.r = 0;
        f.g = 1;
        f.b = 2;
        f.a = -1;
        *out = f;
        return true;
    }
    if (bpp != 32 ||
        (order != 0 && order != k_byte_order_32_big && order != k_byte_order_32_little)) {
        return false;
    }
    bool first = false, has_alpha = true;
    switch (alpha) {
    case k_alpha_premultiplied_last:
    case k_alpha_last:
        break;
    case k_alpha_premultiplied_first:
    case k_alpha_first:
        first = true;
        break;
    case k_alpha_none: // 32 bits without alpha: CG reads it as RGBX
    case k_alpha_none_skip_last:
        has_alpha = false;
        break;
    case k_alpha_none_skip_first:
        has_alpha = false;
        first = true;
        break;
    default:
        return false;
    }
    // Logical component order: X R G B (first) or R G B X (last); 32-bit little-endian
    // reverses it in memory.
    auto pos = [&](int logical) { return order == k_byte_order_32_little ? 3 - logical : logical; };
    const int base = first ? 1 : 0;
    f.bytes_per_pixel = 4;
    f.r = pos(base);
    f.g = pos(base + 1);
    f.b = pos(base + 2);
    const int x = pos(first ? 0 : 3);
    f.a = has_alpha ? x : -1;
    f.skip = has_alpha ? -1 : x;
    f.premultiplied = alpha == k_alpha_premultiplied_last || alpha == k_alpha_premultiplied_first;
    *out = f;
    return true;
}

bool context_format_ok(u32 info, u32 bpc, u32 bpp) {
    Format f;
    const u32 alpha = info & k_alpha_info_mask;
    return bpp == 32 && format_of(info, bpc, bpp, &f) &&
           (alpha == k_alpha_premultiplied_last || alpha == k_alpha_premultiplied_first ||
            alpha == k_alpha_none_skip_last || alpha == k_alpha_none_skip_first);
}

Pixel load_pixel(const Format &f, const u8 *p) {
    Pixel px{p[f.r], p[f.g], p[f.b], 255};
    if (f.a >= 0) {
        px.a = p[f.a];
        if (!f.premultiplied) {
            px.r = static_cast<u8>(mul255(px.r, px.a));
            px.g = static_cast<u8>(mul255(px.g, px.a));
            px.b = static_cast<u8>(mul255(px.b, px.a));
        }
    }
    return px;
}

void store_pixel(const Format &f, u8 *p, Pixel px) {
    if (f.a >= 0 && !f.premultiplied) {
        if (px.a == 0) {
            px.r = px.g = px.b = 0;
        } else if (px.a != 255) {
            auto un = [&](u8 c) {
                return static_cast<u8>(std::min(255u, (c * 255u + px.a / 2u) / px.a));
            };
            px.r = un(px.r);
            px.g = un(px.g);
            px.b = un(px.b);
        }
    }
    p[f.r] = px.r;
    p[f.g] = px.g;
    p[f.b] = px.b;
    if (f.a >= 0) {
        p[f.a] = px.a;
    }
    if (f.skip >= 0) {
        p[f.skip] = 0xFF;
    }
}

Pixel premultiply(const Color &c) {
    const double a = std::clamp(static_cast<double>(c.a), 0.0, 1.0);
    return {to_byte(c.r * a), to_byte(c.g * a), to_byte(c.b * a), to_byte(a)};
}

// --- geometry -----------------------------------------------------------------------------------

Affine Affine::then(const Affine &n) const {
    return {a * n.a + b * n.c, a * n.b + b * n.d,          c * n.a + d * n.c,
            c * n.b + d * n.d, tx * n.a + ty * n.c + n.tx, tx * n.b + ty * n.d + n.ty};
}

bool Affine::invert(Affine *out) const {
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-12 || !std::isfinite(det)) {
        return false;
    }
    const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
    *out = {ia, ib, ic, id, -(tx * ia + ty * ic), -(tx * ib + ty * id)};
    return true;
}

void Path::move_to(Point p) {
    Subpath sp;
    sp.points.push_back(p);
    subpaths.push_back(std::move(sp));
}

void Path::line_to(Point p) {
    if (subpaths.empty() || subpaths.back().closed) {
        // CG starts a new subpath at the end of a closed one (or at p with no current point).
        const Point start = subpaths.empty() ? p : subpaths.back().points.front();
        move_to(start);
    }
    subpaths.back().points.push_back(p);
}

void Path::close() {
    if (!subpaths.empty()) {
        subpaths.back().closed = true;
    }
}

void Path::add_rounded_rect(const Rect &r0, double radius) {
    Rect r = r0;
    if (r.w < 0) {
        r.x += r.w;
        r.w = -r.w;
    }
    if (r.h < 0) {
        r.y += r.h;
        r.h = -r.h;
    }
    radius = std::clamp(radius, 0.0, std::min(r.w, r.h) / 2);
    const int n = static_cast<int>(std::clamp(std::ceil(radius), 4.0, 32.0));
    const double pi = 3.14159265358979323846;
    // Corner centres and their start angles, clockwise in a y-down space from the top edge.
    const struct {
        double cx, cy, start;
    } corners[4] = {{r.x + r.w - radius, r.y + radius, -pi / 2},
                    {r.x + r.w - radius, r.y + r.h - radius, 0},
                    {r.x + radius, r.y + r.h - radius, pi / 2},
                    {r.x + radius, r.y + radius, pi}};
    move_to({r.x + radius, r.y});
    for (const auto &k : corners) {
        for (int i = 0; i <= n; i++) {
            const double t = k.start + (pi / 2) * i / n;
            line_to({k.cx + radius * std::cos(t), k.cy + radius * std::sin(t)});
        }
    }
    close();
}

bool Path::empty() const {
    for (const Subpath &sp : subpaths) {
        if (sp.points.size() >= 2) {
            return false;
        }
    }
    return true;
}

// --- gradients ----------------------------------------------------------------------------------

Color Gradient::at(float t) const {
    if (stops.empty()) {
        return {0, 0, 0, 0};
    }
    if (t <= stops.front().location) {
        return stops.front().color;
    }
    for (std::size_t i = 1; i < stops.size(); i++) {
        const Stop &a = stops[i - 1], &b = stops[i];
        if (t <= b.location) {
            const float span = b.location - a.location;
            const float f = span > 0 ? (t - a.location) / span : 1.0f;
            return {
                a.color.r + (b.color.r - a.color.r) * f, a.color.g + (b.color.g - a.color.g) * f,
                a.color.b + (b.color.b - a.color.b) * f, a.color.a + (b.color.a - a.color.a) * f};
        }
    }
    return stops.back().color;
}

// --- the canvas ---------------------------------------------------------------------------------

Canvas::Canvas(u32 width, u32 height) : width_(width), height_(height) {}

void Canvas::save() { saved_.push_back(gs_); }

bool Canvas::restore() {
    if (saved_.empty()) {
        return false;
    }
    gs_ = saved_.back();
    saved_.pop_back();
    return true;
}

Affine Canvas::user_to_raster() const {
    return gs_.ctm.then({1, 0, 0, -1, 0, static_cast<double>(height_)});
}

double Canvas::device_scale() const {
    const Affine &m = gs_.ctm;
    return std::sqrt(std::fabs(m.a * m.d - m.b * m.c));
}

void Canvas::add_path(const Path &user_path) {
    const Path p = transformed(user_path, user_to_raster());
    path_.subpaths.insert(path_.subpaths.end(), p.subpaths.begin(), p.subpaths.end());
}

void Canvas::clip_to_path() {
    const Coverage c = rasterize(path_, width_, height_);
    auto mask = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(width_) * height_, 0);
    const std::vector<u8> *old = gs_.clip.get();
    for (int y = c.y0; y < c.y1; y++) {
        for (int x = c.x0; x < c.x1; x++) {
            const std::size_t i = static_cast<std::size_t>(y) * width_ + x;
            const u32 v = c.at(x, y);
            (*mask)[i] = static_cast<u8>(old != nullptr ? mul255(v, (*old)[i]) : v);
        }
    }
    gs_.clip = std::move(mask);
    path_.subpaths.clear();
}

void Canvas::fill_path(Surface &s, const Path &user_path, u32 blend_mode) {
    const Coverage c = rasterize(transformed(user_path, user_to_raster()), width_, height_);
    if (c.empty()) {
        return;
    }
    const Pixel color = premultiply(gs_.fill);
    const u32 mode = blend_mode != UINT32_MAX ? blend_mode : gs_.blend;
    if (g_shortcuts && fill_constant(s, gs_, mode, c, color)) {
        return;
    }
    composite(
        s, gs_, mode, c.x0, c.y0, c.x1, c.y1, [&](int x, int y) { return c.at(x, y); },
        [&](int, int) { return color; });
}

void Canvas::stroke_path(Surface &s, const Path &user_path) {
    Path quads;
    const double hw = gs_.line_width / 2;
    for (const Subpath &sp : user_path.subpaths) {
        const std::size_t n = sp.points.size();
        const std::size_t segments = sp.closed ? n : (n > 0 ? n - 1 : 0);
        for (std::size_t i = 0; i < segments && n >= 2; i++) {
            const Point a = sp.points[i], b = sp.points[(i + 1) % n];
            const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
            if (len == 0) {
                continue;
            }
            const double nx = -dy / len * hw, ny = dx / len * hw;
            // Every quad turns the same way, so overlaps add up instead of cancelling.
            quads.move_to({a.x + nx, a.y + ny});
            quads.line_to({a.x - nx, a.y - ny});
            quads.line_to({b.x - nx, b.y - ny});
            quads.line_to({b.x + nx, b.y + ny});
            quads.close();
        }
    }
    const Color fill = gs_.fill;
    gs_.fill = gs_.stroke;
    fill_path(s, quads);
    gs_.fill = fill;
}

void Canvas::fill_rect(Surface &s, const Rect &r, u32 blend_mode) {
    fill_path(s, rect_path(r), blend_mode);
}

void Canvas::draw_image(Surface &s, const Rect &r, const ConstSurface &img, bool top_down) {
    if (img.width == 0 || img.height == 0 || r.w == 0 || r.h == 0) {
        return;
    }
    // Image space (u right, v down from the top row, in pixels) -> user -> raster.
    const double sx = r.w / img.width, sy = r.h / img.height;
    const Affine image_to_user =
        top_down ? Affine{sx, 0, 0, sy, r.x, r.y} : Affine{sx, 0, 0, -sy, r.x, r.y + r.h};
    const Affine m = image_to_user.then(user_to_raster());
    Affine inv;
    if (!m.invert(&inv)) {
        return;
    }
    const Coverage c = rasterize(
        transformed(
            rect_path({0, 0, static_cast<double>(img.width), static_cast<double>(img.height)}), m),
        width_, height_);
    if (c.empty()) {
        return;
    }
    const bool bilinear = !pixel_aligned(m);
    if (g_shortcuts && inv.b == 0 && inv.c == 0) {
        draw_image_axis_aligned(s, gs_, c, img, inv, bilinear);
        return;
    }
    composite(
        s, gs_, gs_.blend, c.x0, c.y0, c.x1, c.y1, [&](int x, int y) { return c.at(x, y); },
        [&](int x, int y) {
            const Point uv = inv.apply({x + 0.5, y + 0.5});
            return sample(img, uv.x, uv.y, bilinear);
        });
}

void Canvas::draw_linear_gradient(Surface &s, const Gradient &g, Point start, Point end,
                                  u32 options) {
    Affine inv;
    if (!user_to_raster().invert(&inv)) {
        return;
    }
    const double dx = end.x - start.x, dy = end.y - start.y, len2 = dx * dx + dy * dy;
    // t of each pixel, or < -1 for "not drawn" (outside the extended range).
    auto t_at = [&](int x, int y) {
        const Point u = inv.apply({x + 0.5, y + 0.5});
        double t = len2 > 0 ? ((u.x - start.x) * dx + (u.y - start.y) * dy) / len2 : 0;
        if (t < 0) {
            t = (options & k_gradient_before_start) != 0 ? 0 : -2;
        } else if (t > 1) {
            t = (options & k_gradient_after_end) != 0 ? 1 : -2;
        }
        return t;
    };
    composite(
        s, gs_, gs_.blend, 0, 0, static_cast<int>(width_), static_cast<int>(height_),
        [&](int x, int y) { return t_at(x, y) < -1 ? 0u : 255u; },
        [&](int x, int y) { return premultiply(g.at(static_cast<float>(t_at(x, y)))); });
}

void Canvas::draw_radial_gradient(Surface &s, const Gradient &g, Point c0, double r0, Point c1,
                                  double r1, u32 options) {
    Affine inv;
    if (!user_to_raster().invert(&inv)) {
        return;
    }
    const double cdx = c1.x - c0.x, cdy = c1.y - c0.y, dr = r1 - r0;
    const double a = cdx * cdx + cdy * cdy - dr * dr;
    // The largest t whose circle c(t), r(t) >= 0 passes through the pixel (as the HTML canvas
    // and Quartz define radial shadings); < -1 if none or outside the extended range.
    auto t_at = [&](int x, int y) {
        const Point u = inv.apply({x + 0.5, y + 0.5});
        const double px = u.x - c0.x, py = u.y - c0.y;
        const double b = px * cdx + py * cdy + r0 * dr;
        const double c = px * px + py * py - r0 * r0;
        double t;
        if (std::fabs(a) < 1e-9) {
            if (std::fabs(b) < 1e-12) {
                return -2.0;
            }
            t = c / (2 * b);
            if (r0 + t * dr < 0) {
                return -2.0;
            }
        } else {
            const double disc = b * b - a * c;
            if (disc < 0) {
                return -2.0;
            }
            const double sq = std::sqrt(disc);
            const double t1 = (b + sq) / a, t2 = (b - sq) / a;
            const double hi = std::max(t1, t2), lo = std::min(t1, t2);
            if (r0 + hi * dr >= 0) {
                t = hi;
            } else if (r0 + lo * dr >= 0) {
                t = lo;
            } else {
                return -2.0;
            }
        }
        if (t < 0) {
            return (options & k_gradient_before_start) != 0 ? 0.0 : -2.0;
        }
        if (t > 1) {
            return (options & k_gradient_after_end) != 0 ? 1.0 : -2.0;
        }
        return t;
    };
    composite(
        s, gs_, gs_.blend, 0, 0, static_cast<int>(width_), static_cast<int>(height_),
        [&](int x, int y) { return t_at(x, y) < -1 ? 0u : 255u; },
        [&](int x, int y) { return premultiply(g.at(static_cast<float>(t_at(x, y)))); });
}

void Canvas::draw_glyph(Surface &s, Point pen, const u8 *cov, int gw, int gh, int x0, int y0,
                        double ds, int embolden) {
    if (gw <= 0 || gh <= 0 || ds <= 0) {
        return;
    }
    const int ew = gw + embolden;
    // Coverage of the (emboldened) bitmap at whole bitmap pixel (bx, by).
    auto bitmap = [&](int bx, int by) -> u32 {
        if (by < 0 || by >= gh || bx < 0 || bx >= ew) {
            return 0;
        }
        u32 v = 0;
        for (int k = 0; k <= embolden; k++) {
            const int x = bx - k;
            if (x >= 0 && x < gw) {
                v = std::max<u32>(v, cov[static_cast<std::size_t>(by) * gw + x]);
            }
        }
        return v;
    };
    // Bitmap space (pixels, y down) -> user (UIKit's y-down text space) -> raster.
    const Affine bitmap_to_user{1 / ds, 0, 0, 1 / ds, pen.x + x0 / ds, pen.y + y0 / ds};
    const Affine m = bitmap_to_user.then(user_to_raster());
    Affine inv;
    if (!m.invert(&inv)) {
        return;
    }
    const Pixel color = premultiply(gs_.fill);
    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
    for (const Point corner :
         {Point{0, 0}, Point{static_cast<double>(ew), 0}, Point{0, static_cast<double>(gh)},
          Point{static_cast<double>(ew), static_cast<double>(gh)}}) {
        const Point p = m.apply(corner);
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    if (!std::isfinite(minx) || !std::isfinite(maxy)) {
        return;
    }
    const bool aligned = pixel_aligned(m);
    composite(
        s, gs_, gs_.blend, static_cast<int>(std::floor(minx)), static_cast<int>(std::floor(miny)),
        static_cast<int>(std::ceil(maxx)), static_cast<int>(std::ceil(maxy)),
        [&](int x, int y) -> u32 {
            const Point b = inv.apply({x + 0.5, y + 0.5});
            if (aligned) {
                return bitmap(static_cast<int>(std::floor(b.x)), static_cast<int>(std::floor(b.y)));
            }
            const double fx = b.x - 0.5, fy = b.y - 0.5;
            const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
            const double tx = fx - ix, ty = fy - iy;
            const double top =
                bitmap(ix, iy) + (static_cast<double>(bitmap(ix + 1, iy)) - bitmap(ix, iy)) * tx;
            const double bottom =
                bitmap(ix, iy + 1) +
                (static_cast<double>(bitmap(ix + 1, iy + 1)) - bitmap(ix, iy + 1)) * tx;
            return static_cast<u32>(std::lround(top + (bottom - top) * ty));
        },
        [&](int, int) { return color; });
}

} // namespace mt_cg
