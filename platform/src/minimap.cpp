// The port's minimap (#176, ADR 0098); see minimap.h.
#include "minimap.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "autosave.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/uikit.h"
#include "present.h"

namespace maplethief::frontend {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 kCanvasState = 0x0C; // SkyCanvas: the engine's current IState
constexpr i32 kFieldMode = 0;
constexpr i32 kMaxTileSide = 128;

bool read_u32(const GuestSpan &mem, u32 addr, u32 *out) {
    const u8 *p = mem(addr, 4);
    if (p == nullptr) {
        return false;
    }
    std::memcpy(out, p, 4);
    return true;
}
bool read_i32(const GuestSpan &mem, u32 addr, i32 *out) {
    u32 v = 0;
    if (!read_u32(mem, addr, &v)) {
        return false;
    }
    *out = static_cast<i32>(v);
    return true;
}
bool in_range(i32 v, i32 lo, i32 hi) { return v >= lo && v <= hi; }

// The objects the minimap marks: portals (0-2; 3 is the hidden portal, never shown) and NPCs
// (4-59), visible ones only (docs/formats/obj.md, Kinds; a hidden object is not drawn either).
void read_markers(const GuestSpan &mem, u32 map, MinimapView *v) {
    i32 count = 0, tiles_w = 0, tw = 0, th = 0;
    u32 objects = 0;
    if (!read_i32(mem, map + kMapObjectCount, &count) || count <= 0 ||
        !read_u32(mem, map + kMapObjects, &objects) || objects == 0 ||
        !read_i32(mem, map + kMapTilesWide, &tiles_w) || !in_range(tiles_w, 1, kMinimapMaxSide) ||
        !read_i32(mem, map + kMapTileW, &tw) || !read_i32(mem, map + kMapTileH, &th) ||
        !in_range(tw, 1, kMaxTileSide) || !in_range(th, 1, kMaxTileSide)) {
        return;
    }
    const i32 n = std::min(count, kMinimapMaxObjects);
    for (i32 i = 0; i < n; i++) {
        u32 obj = 0;
        if (!read_u32(mem, objects + 4 * static_cast<u32>(i), &obj) || obj == 0) {
            continue;
        }
        const u8 *p = mem(obj, 8);
        if (p == nullptr) {
            continue;
        }
        std::int16_t cell = 0, kind = 0, visible = 0;
        std::memcpy(&cell, p, 2);
        std::memcpy(&kind, p + 2, 2);
        std::memcpy(&visible, p + 6, 2);
        if (cell < 0 || visible == 0 || kind < 0 || kind == 3 || kind > 59) {
            continue;
        }
        // The anchor: x = tx * tw + 8, y = (ty + 1) * th - 1 (0xBD0D, 0xBD41).
        const i32 tx = cell % tiles_w, ty = cell / tiles_w;
        v->markers.push_back({kind <= 2 ? MinimapMarker::kPortal : MinimapMarker::kNpc, tx * tw + 8,
                              (ty + 1) * th - 1});
    }
}

int priority(u8 flags) {
    if ((flags & kAttFoothold) != 0) {
        return 3;
    }
    if ((flags & kAttLadder) != 0) {
        return 2;
    }
    if ((flags & kAttWall) != 0) {
        return 1;
    }
    return 0;
}
constexpr MinimapColor kPriorityColor[] = {kMinimapBack, kMinimapWall, kMinimapLadder,
                                           kMinimapFoothold};

// The cells a box pixel row or column covers: [first, last] for each of `n` pixels.
struct Span {
    i32 first, last;
};
std::vector<Span> cover(i32 pixels, double per_cell, i32 cells) {
    std::vector<Span> out(static_cast<std::size_t>(std::max(pixels, 0)));
    for (i32 p = 0; p < pixels; p++) {
        const i32 a = std::clamp(static_cast<i32>(std::floor(p / per_cell)), 0, cells - 1);
        const i32 b = std::clamp(static_cast<i32>(std::ceil((p + 1) / per_cell)) - 1, a, cells - 1);
        out[static_cast<std::size_t>(p)] = {a, b};
    }
    return out;
}

Rect clamp_into(Rect r, const Rect &box) {
    r.w = std::min(r.w, box.w);
    r.h = std::min(r.h, box.h);
    r.x = std::clamp(r.x, box.x, box.x + box.w - r.w);
    r.y = std::clamp(r.y, box.y, box.y + box.h - r.h);
    return r;
}
Rect intersect(const Rect &a, const Rect &b) {
    const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}
// A square dot of `points` screen points centred on map pixel (x, y), clamped into the box.
Rect dot_at(const MinimapLayout &l, float map_x, float map_y, float points) {
    const float d = std::max(1.0f, std::round(points * l.point));
    float cx = 0, cy = 0;
    minimap_point(l, map_x, map_y, &cx, &cy);
    return clamp_into(Rect{std::round(cx - d / 2), std::round(cy - d / 2), d, d}, l.box);
}

// The scale that fits a map_w x map_h map into avail_w x avail_h, at most `cap`; 0 if none.
float fit_scale(float avail_w, float avail_h, i32 map_w, i32 map_h, float cap) {
    if (avail_w < 1 || avail_h < 1) {
        return 0;
    }
    return std::min(
        {cap, avail_w / static_cast<float>(map_w), avail_h / static_cast<float>(map_h)});
}
Rect box_of(float x, float y, float scale, i32 map_w, i32 map_h) {
    return Rect{x, y, std::max(1.0f, std::floor(static_cast<float>(map_w) * scale)),
                std::max(1.0f, std::floor(static_cast<float>(map_h) * scale))};
}

MinimapLayout place_box(Minimap mode, MinimapAnchor anchor, const Placement &image,
                        const Rect &wide, const Rect &visible, std::int32_t map_w,
                        std::int32_t map_h);

} // namespace

GuestSpan guest_span(const mt_cpu *cpu) {
    return [cpu](u32 addr, u32 n) -> const u8 * {
        const std::uint64_t end = std::uint64_t{addr} + n;
        mt_mem *mem = cpu != nullptr ? mt_mem_from_cpu(cpu) : nullptr;
        if (mem == nullptr || cpu->mem == nullptr || addr == 0 || n == 0 || end > cpu->mem_size) {
            return nullptr;
        }
        u32 count = 0;
        const mt_segment *segs = mt_mem_segments(mem, &count);
        for (u32 i = 0; segs != nullptr && i < count; i++) {
            if (addr >= segs[i].vmaddr && end <= std::uint64_t{segs[i].vmaddr} + segs[i].vmsize) {
                return cpu->mem + addr;
            }
        }
        mt_heap_stats heap{};
        mt_heap_get_stats(mem, &heap);
        return addr >= MT_GUEST_HEAP_BASE && end <= heap.committed ? cpu->mem + addr : nullptr;
    };
}

MinimapView read_minimap(const GuestSpan &mem) {
    MinimapView v;
    u32 canvas = 0, state = 0, vptr = 0, map = 0, hero = 0, att = 0;
    i32 mode = -1, cols = 0, rows = 0, size = 0, cell_w = 0, cell_h = 0;
    if (!read_u32(mem, kAutosaveCanvas, &canvas) || canvas == 0 ||
        !read_u32(mem, canvas + kCanvasState, &state) || state == 0 ||
        !read_u32(mem, state, &vptr) || vptr != kAutosaveGameVptr ||
        !read_i32(mem, kAutosaveGameMode, &mode) || mode != kFieldMode ||
        !read_u32(mem, state + kMinimapStateMap, &map) || map == 0 ||
        !read_u32(mem, state + kMinimapStateHero, &hero) || hero == 0) {
        return v;
    }
    if (!read_i32(mem, map + kMapAttW, &cols) || !read_i32(mem, map + kMapAttH, &rows) ||
        !read_u32(mem, map + kMapAtt, &att) || !read_i32(mem, map + kMapAttSize, &size) ||
        !read_i32(mem, map + kMapCellW, &cell_w) || !read_i32(mem, map + kMapCellH, &cell_h) ||
        !in_range(cols, 1, kMinimapMaxSide) || !in_range(rows, 1, kMinimapMaxSide) ||
        !in_range(cell_w, 1, kMinimapMaxCellSide) || !in_range(cell_h, 1, kMinimapMaxCellSide) ||
        std::int64_t{size} < std::int64_t{cols} * rows || att == 0 ||
        !read_i32(mem, hero + 0x04, &v.hero_x) || !read_i32(mem, hero + 0x08, &v.hero_y)) {
        return v;
    }
    const u32 cells = static_cast<u32>(cols) * static_cast<u32>(rows);
    const u8 *grid = mem(att, cells);
    if (grid == nullptr) {
        return v;
    }
    v.cols = cols;
    v.rows = rows;
    v.cell_w = cell_w;
    v.cell_h = cell_h;
    v.grid.assign(grid, grid + cells);
    read_markers(mem, map, &v);
    v.shown = true;
    return v;
}

MinimapAnchor minimap_anchor(Widescreen widescreen, WidescreenHud hud) {
    return widescreen == Widescreen::kAuto && hud == WidescreenHud::kEdges
               ? MinimapAnchor::kCorner
               : MinimapAnchor::kCompass;
}

Rect minimap_visible(const Placement &image, const Rect &wide, std::uint32_t wide_points,
                     std::uint32_t view) {
    if (wide_points <= static_cast<std::uint32_t>(kScreenWidth) || wide.w <= 0 || wide.h <= 0) {
        return image.rect;
    }
    return wide_part(wide, wide_points,
                     std::clamp(view, static_cast<std::uint32_t>(kScreenWidth), wide_points));
}

MinimapLayout minimap_layout(Minimap mode, const Placement &image, const Rect &wide,
                             std::int32_t map_w, std::int32_t map_h) {
    return minimap_layout(mode, MinimapAnchor::kCompass, image, wide, image.rect, map_w, map_h);
}

MinimapLayout minimap_layout(Minimap mode, MinimapAnchor anchor, const Placement &image,
                             const Rect &wide, const Rect &visible, std::int32_t map_w,
                             std::int32_t map_h) {
    MinimapLayout l = place_box(mode, anchor, image, wide, visible, map_w, map_h);
    if (l.shown) {
        const float k = l.point;
        l.points = Rect{(l.box.x - image.rect.x) / k, (l.box.y - image.rect.y) / k, l.box.w / k,
                        l.box.h / k};
        if (l.corner) {
            // The compass right of the box, a margin between them; never left of its frame.
            l.compass_dx =
                std::max(0.0f, std::ceil(l.points.x + l.points.w + kMinimapMargin - 0.001f));
        }
    }
    return l;
}

std::uint32_t minimap_compass_view() {
    std::uint32_t subs[64];
    const std::uint32_t n = std::min<std::uint32_t>(
        mt_uikit_subviews(mt_input_pad_panel_view(0), subs, std::size(subs)), std::size(subs));
    for (std::uint32_t i = 0; i < n; i++) {
        float fr[4] = {};
        if (mt_uikit_screen_frame(subs[i], fr) != 0 && fr[0] + fr[2] / 2 >= 0 &&
            fr[1] + fr[3] / 2 >= 0 && fr[0] + fr[2] / 2 < kMinimapCompassSide &&
            fr[1] + fr[3] / 2 < kMinimapCompassSide) {
            return subs[i];
        }
    }
    return 0;
}

bool minimap_move_compass(float dx) {
    const std::uint32_t view = minimap_compass_view();
    if (view == 0) {
        return false;
    }
    const float m[6] = {1, 0, 0, 1, dx, 0};
    return mt_uikit_set_host_transform(view, dx != 0 ? m : nullptr) != 0;
}

std::vector<InputRedirect> minimap_touch_redirects(const MinimapLayout &l) {
    std::vector<InputRedirect> out;
    if (!l.shown || !l.corner) {
        return out;
    }
    const Rect compass{0, 0, kMinimapCompassSide, kMinimapCompassSide};
    if (l.compass_dx > 0) {
        out.push_back({Rect{l.compass_dx, 0, kMinimapCompassSide, kMinimapCompassSide}, compass});
    }
    out.push_back({l.points, compass});
    return out;
}

std::vector<Rect> minimap_frame(const MinimapLayout &l) {
    if (!l.shown) {
        return {};
    }
    const float t = std::max(1.0f, std::round(l.point));
    const Rect &b = l.box;
    return {Rect{b.x - t, b.y - t, b.w + 2 * t, t}, Rect{b.x - t, b.y + b.h, b.w + 2 * t, t},
            Rect{b.x - t, b.y, t, b.h}, Rect{b.x + b.w, b.y, t, b.h}};
}

namespace {

MinimapLayout place_box(Minimap mode, MinimapAnchor anchor, const Placement &image,
                        const Rect &wide, const Rect &visible, std::int32_t map_w,
                        std::int32_t map_h) {
    MinimapLayout l;
    if (mode == Minimap::kOff || map_w <= 0 || map_h <= 0 || image.rect.w <= 0 ||
        image.rect.h <= 0) {
        return l;
    }
    const float k = image.rect.w / 480.0f;
    const float m = std::round(kMinimapMargin * k);
    const float cap = kMinimapMaxScale * k;
    l.point = k;
    if (mode == Minimap::kBorder) {
        const Rect &safe = image.safe.w > 0 ? image.safe : wide;
        // The bar to the left of the (wide) picture: the box at its top, centred across it.
        const float left = wide.x - safe.x;
        float best = 0;
        if (left >= kMinimapMinBorderW * k + 2 * m) {
            const float s = fit_scale(left - 2 * m, wide.h / 2 - m, map_w, map_h, cap);
            if (s > best) {
                best = s;
                l.box = box_of(0, wide.y + m, s, map_w, map_h);
                l.box.x = safe.x + std::floor((left - l.box.w) / 2);
            }
        }
        // The bar below it (a window taller than the picture): at its left, centred down it.
        const float below = safe.y + safe.h - (wide.y + wide.h);
        if (below >= kMinimapMinBorderH * k + 2 * m) {
            const float s = fit_scale(wide.w / 2 - m, below - 2 * m, map_w, map_h, cap);
            if (s > best) {
                best = s;
                l.box = box_of(wide.x + m, 0, s, map_w, map_h);
                l.box.y = wide.y + wide.h + std::floor((below - l.box.h) / 2);
            }
        }
        if (best > 0) {
            l.shown = l.in_border = true;
            l.scale = best;
            return l;
        }
    }
    if (anchor == MinimapAnchor::kCorner && visible.w > 0 && visible.h > 0) {
        // The visible view's top left corner, a margin in, inside the safe area.
        // At most kMinimapCornerW x kMinimapCornerH points, ending above the messages (y = 52)
        // and where the moved compass still ends at kMinimapCornerRight.
        const Rect &safe = image.safe.w > 0 ? image.safe : visible;
        const float x = std::max(visible.x, safe.x) + m, y = std::max(visible.y, safe.y) + m;
        const float right = image.rect.x + (kMinimapCornerRight - kMinimapCompassSide) * k - m;
        const float bottom = image.rect.y + (kMinimapMargin + kMinimapCornerH) * k;
        const float s = fit_scale(std::min(kMinimapCornerW * k, std::floor(right - x)),
                                  std::floor(bottom - y), map_w, map_h, cap);
        if (s <= 0) {
            return l;
        }
        l.box = box_of(x, y, s, map_w, map_h);
        l.scale = s;
        l.shown = l.corner = true;
        return l;
    }
    const float s = fit_scale(kMinimapOverlayW * k, kMinimapOverlayH * k, map_w, map_h, cap);
    if (s <= 0) {
        return l;
    }
    l.box =
        box_of(image.rect.x + std::round(kMinimapOverlayX * k), image.rect.y + m, s, map_w, map_h);
    l.scale = s;
    l.shown = true;
    return l;
}

} // namespace

void minimap_point(const MinimapLayout &l, float map_x, float map_y, float *x, float *y) {
    *x = l.box.x + map_x * l.scale;
    *y = l.box.y + map_y * l.scale;
}

Rect minimap_hero_dot(const MinimapView &v, const MinimapLayout &l) {
    return dot_at(l, static_cast<float>(v.hero_x),
                  static_cast<float>(v.hero_y) - static_cast<float>(v.cell_h), kMinimapHeroDot);
}

std::vector<MinimapDot> minimap_dots(const MinimapView &v, const MinimapLayout &l) {
    std::vector<MinimapDot> out;
    if (!v.shown || !l.shown) {
        return out;
    }
    for (const MinimapMarker &mk : v.markers) {
        out.push_back(
            {dot_at(l, static_cast<float>(mk.x),
                    static_cast<float>(mk.y) - static_cast<float>(v.cell_h), kMinimapMarkerDot),
             mk.kind == MinimapMarker::kPortal ? kMinimapPortal : kMinimapNpc});
    }
    const Rect hero = minimap_hero_dot(v, l);
    out.push_back(
        {intersect(Rect{hero.x - 1, hero.y - 1, hero.w + 2, hero.h + 2}, l.box), kMinimapHeroEdge});
    out.push_back({hero, kMinimapHero});
    return out;
}

std::vector<std::uint8_t> minimap_grid_pixels(const MinimapView &v, const MinimapLayout &l) {
    const i32 w = static_cast<i32>(l.box.w), h = static_cast<i32>(l.box.h);
    std::vector<std::uint8_t> out;
    if (!v.shown || !l.shown || w <= 0 || h <= 0 || v.cols <= 0 || v.rows <= 0 ||
        v.grid.size() != static_cast<std::size_t>(v.cols) * static_cast<std::size_t>(v.rows)) {
        return out;
    }
    out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const std::vector<Span> xs = cover(w, static_cast<double>(l.scale) * v.cell_w, v.cols);
    const std::vector<Span> ys = cover(h, static_cast<double>(l.scale) * v.cell_h, v.rows);
    for (i32 y = 0; y < h; y++) {
        const Span sy = ys[static_cast<std::size_t>(y)];
        for (i32 x = 0; x < w; x++) {
            const Span sx = xs[static_cast<std::size_t>(x)];
            int best = 0;
            for (i32 cy = sy.first; cy <= sy.last && best < 3; cy++) {
                const u8 *row = v.grid.data() + static_cast<std::size_t>(cy) * v.cols;
                for (i32 cx = sx.first; cx <= sx.last && best < 3; cx++) {
                    best = std::max(best, priority(row[cx]));
                }
            }
            const MinimapColor c = kPriorityColor[best];
            std::uint8_t *px = &out[(static_cast<std::size_t>(y) * w + x) * 4];
            px[0] = c.r;
            px[1] = c.g;
            px[2] = c.b;
            px[3] = c.a;
        }
    }
    return out;
}

std::vector<std::uint8_t> render_minimap(const MinimapView &v, const MinimapLayout &l) {
    std::vector<std::uint8_t> out = minimap_grid_pixels(v, l);
    if (out.empty()) {
        return out;
    }
    const i32 w = static_cast<i32>(l.box.w);
    for (const MinimapDot &d : minimap_dots(v, l)) {
        const i32 x0 = static_cast<i32>(d.rect.x - l.box.x),
                  y0 = static_cast<i32>(d.rect.y - l.box.y);
        for (i32 y = y0; y < y0 + static_cast<i32>(d.rect.h); y++) {
            for (i32 x = x0; x < x0 + static_cast<i32>(d.rect.w); x++) {
                std::uint8_t *px = &out[(static_cast<std::size_t>(y) * w + x) * 4];
                px[0] = d.color.r;
                px[1] = d.color.g;
                px[2] = d.color.b;
                px[3] = d.color.a;
            }
        }
    }
    return out;
}

bool MinimapLayer::draw(SDL_Renderer *renderer, const MinimapView &v, const MinimapLayout &l) {
    if (renderer == nullptr || !v.shown || !l.shown) {
        return true;
    }
    const int w = static_cast<int>(l.box.w), h = static_cast<int>(l.box.h);
    auto fail = [this](const char *what) {
        if (!failed_logged_) {
            SDL_Log("minimap: %s: %s", what, SDL_GetError());
            failed_logged_ = true;
        }
        return false;
    };
    if (renderer != renderer_ || w != tex_w_ || h != tex_h_ || texture_ == nullptr) {
        stop();
        texture_ =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (texture_ == nullptr) {
            return fail("cannot create its texture");
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
        renderer_ = renderer;
        tex_w_ = w;
        tex_h_ = h;
    }
    if (v.cols != cols_ || v.rows != rows_ || v.grid != grid_) {
        const std::vector<std::uint8_t> px = minimap_grid_pixels(v, l);
        if (px.empty() || !SDL_UpdateTexture(texture_, nullptr, px.data(), w * 4)) {
            return fail("cannot upload the grid");
        }
        grid_ = v.grid;
        cols_ = v.cols;
        rows_ = v.rows;
        stats_.uploads++;
    }
    const SDL_FRect dst{l.box.x, l.box.y, l.box.w, l.box.h};
    bool ok = SDL_RenderTexture(renderer, texture_, nullptr, &dst);
    Uint8 r = 0, g = 0, b = 0, a = 0;
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_GetRenderDrawBlendMode(renderer, &blend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, kMinimapEdge.r, kMinimapEdge.g, kMinimapEdge.b,
                           kMinimapEdge.a);
    for (const Rect &f : minimap_frame(l)) {
        const SDL_FRect rect{f.x, f.y, f.w, f.h};
        ok = SDL_RenderFillRect(renderer, &rect) && ok;
    }
    for (const MinimapDot &d : minimap_dots(v, l)) {
        const SDL_FRect rect{d.rect.x, d.rect.y, d.rect.w, d.rect.h};
        SDL_SetRenderDrawColor(renderer, d.color.r, d.color.g, d.color.b, d.color.a);
        ok = SDL_RenderFillRect(renderer, &rect) && ok;
    }
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_SetRenderDrawBlendMode(renderer, blend);
    if (!ok) {
        return fail("cannot draw");
    }
    stats_.draws++;
    stats_.last_border = l.in_border;
    return true;
}

void MinimapLayer::stop() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    renderer_ = nullptr;
    tex_w_ = tex_h_ = 0;
    grid_.clear();
    cols_ = rows_ = 0;
}

} // namespace maplethief::frontend
