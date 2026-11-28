// The port's minimap (#176, ADR 0098); see minimap.h.
#include "minimap.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "autosave.h"
#include "maplethief/cg.h"
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

// The live mobs (minimap_mobs): the list at kMinimapMobList, at most kMinimapMaxMobs items; a
// null or unmapped one, a dead one or feet outside the map give no marker.
void read_mobs(const GuestSpan &mem, MinimapView *v) {
    u32 list = 0, items = 0;
    i32 count = 0;
    if (!read_u32(mem, kMinimapMobList, &list) || list == 0 ||
        !read_i32(mem, list + 0x04, &count) || count <= 0 || !read_u32(mem, list + 0x08, &items) ||
        items == 0) {
        return;
    }
    const i32 n = std::min(count, kMinimapMaxMobs);
    for (i32 i = 0; i < n; i++) {
        u32 mob = 0, flags = 0;
        i32 x = 0, y = 0;
        if (!read_u32(mem, items + 4 * static_cast<u32>(i), &mob) || mob == 0 ||
            !read_i32(mem, mob + 0x04, &x) || !read_i32(mem, mob + 0x08, &y) ||
            !read_u32(mem, mob + 0x0C, &flags) || (flags & kMinimapMobDead) != 0 ||
            !in_range(x, 0, v->map_w()) || !in_range(y, 0, v->map_h())) {
            continue;
        }
        v->markers.push_back({MinimapMarker::kMob, x, y});
    }
}

// Whether `s` is well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF).
bool valid_utf8(const std::string &s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const u8 b = static_cast<u8>(s[i]);
        int len = 0;
        u32 cp = 0, min = 0;
        if (b < 0x80) {
            i++;
            continue;
        }
        if ((b & 0xE0) == 0xC0) {
            len = 2;
            cp = b & 0x1Fu;
            min = 0x80;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3;
            cp = b & 0x0Fu;
            min = 0x800;
        } else if ((b & 0xF8) == 0xF0) {
            len = 4;
            cp = b & 0x07u;
            min = 0x10000;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(len) > s.size()) {
            return false;
        }
        for (int k = 1; k < len; k++) {
            const u8 c = static_cast<u8>(s[i + static_cast<std::size_t>(k)]);
            if ((c & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
            return false;
        }
        i += static_cast<std::size_t>(len);
    }
    return true;
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

std::int32_t minimap_area_world_spot(std::int32_t area) {
    switch (area) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return 3;
    case 3:
        return 2;
    case 4:
    case 5:
        return 4;
    case 6:
    case 7:
    case 8:
        return 6;
    case 9:
    case 10:
        return 5;
    default:
        return -1;
    }
}

std::int32_t minimap_world_spot_group(std::int32_t spot) {
    if (spot >= 0 && spot <= 3) {
        return 0;
    }
    if (spot == 4 || spot == 5) {
        return 1;
    }
    return spot == 6 ? 2 : -1;
}

std::string minimap_map_name(const GuestSpan &mem, std::int32_t area, std::int32_t map) {
    const i32 group = minimap_world_spot_group(minimap_area_world_spot(area));
    i32 index = -1;
    if (group < 0 || !in_range(area, 0, kMinimapAreas - 1) ||
        !in_range(map, 0, kMinimapMapsPerArea - 1) ||
        !read_i32(mem, kMinimapSpotIndex + 4 * static_cast<u32>(area * kMinimapMapsPerArea + map),
                  &index) ||
        !in_range(index, 0, kMinimapNamesPerGroup - 1)) {
        return {};
    }
    const u8 *p = mem(kMinimapSpotNames + static_cast<u32>(group) * kMinimapNameGroupSize +
                          static_cast<u32>(index) * kMinimapNameSize,
                      kMinimapNameSize);
    if (p == nullptr) {
        return {};
    }
    const void *nul = std::memchr(p, 0, kMinimapNameSize);
    if (nul == nullptr) {
        return {};
    }
    std::string name(reinterpret_cast<const char *>(p),
                     static_cast<std::size_t>(static_cast<const u8 *>(nul) - p));
    return valid_utf8(name) ? name : std::string();
}

MinimapView read_minimap(const GuestSpan &mem, const MinimapExtras &extras) {
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
    if (extras.mobs) {
        read_mobs(mem, &v);
    }
    i32 area = 0, map_no = 0;
    if (extras.name && read_i32(mem, state + kMinimapStateArea, &area) &&
        read_i32(mem, state + kMinimapStateMapNo, &map_no)) {
        v.name = minimap_map_name(mem, area, map_no);
    }
    v.shown = true;
    return v;
}

MinimapAnchor minimap_anchor(Widescreen widescreen, WidescreenHud hud) {
    return widescreen != Widescreen::kOff && hud == WidescreenHud::kEdges ? MinimapAnchor::kCorner
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
             mk.kind == MinimapMarker::kPortal ? kMinimapPortal
             : mk.kind == MinimapMarker::kNpc  ? kMinimapNpc
                                               : kMinimapMob});
    }
    const Rect hero = minimap_hero_dot(v, l);
    out.push_back(
        {intersect(Rect{hero.x - 1, hero.y - 1, hero.w + 2, hero.h + 2}, l.box), kMinimapHeroEdge});
    out.push_back({hero, kMinimapHero});
    return out;
}

MinimapNameStrip minimap_name_strip(const MinimapView &v, const MinimapLayout &l) {
    MinimapNameStrip n;
    if (!v.shown || !l.shown || v.name.empty() || l.box.h < 4 || l.box.w < 4) {
        return n;
    }
    // A line of NanumGothic is about 1.15 em high (its ascent to descent): the strip holds one
    // with a point to spare. It takes at most kMinimapNameShare of the box's height, the em
    // shrunk to fit, but not under kMinimapNameMinEm points while the box holds it.
    const float pad = std::max(1.0f, std::round(l.point));
    const float fit = std::floor((l.box.h * kMinimapNameShare - pad) / 1.2f);
    const float most = std::floor((l.box.h - pad) / 1.2f);
    const float em = std::min({std::round(kMinimapNameEm * l.point), most,
                               std::max(fit, std::round(kMinimapNameMinEm * l.point))});
    if (em < 4) {
        return n;
    }
    n.em = static_cast<std::int32_t>(em);
    n.strip = Rect{l.box.x, l.box.y, l.box.w, std::min(l.box.h, std::ceil(em * 1.2f) + pad)};
    n.text_x = l.box.x + pad;
    n.max_w = static_cast<std::int32_t>(l.box.w - pad);
    n.shown = n.max_w > 0;
    return n;
}

std::vector<std::uint8_t> minimap_name_pixels(const std::string &name, std::int32_t em,
                                              std::int32_t max_w, std::int32_t *w,
                                              std::int32_t *h) {
    *w = *h = 0;
    const u8 color[4] = {kMinimapNameText.r, kMinimapNameText.g, kMinimapNameText.b,
                         kMinimapNameText.a};
    mt_cg_line line{};
    if (name.empty() ||
        mt_cg_bundled_line(name.data(), name.size(), em, max_w, color, &line, nullptr, 0) == 0) {
        return {};
    }
    std::vector<std::uint8_t> px(static_cast<std::size_t>(line.width) *
                                 static_cast<std::size_t>(line.height) * 4);
    if (mt_cg_bundled_line(name.data(), name.size(), em, max_w, color, &line, px.data(),
                           px.size()) == 0) {
        return {};
    }
    *w = line.width;
    *h = line.height;
    return px;
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
    const MinimapNameStrip n = minimap_name_strip(v, l);
    if (!n.shown) {
        return out;
    }
    // Straight-alpha "over" of `c` onto the pixel at (x, y) of the box.
    auto over = [&](i32 x, i32 y, const u8 *c) {
        if (x < 0 || y < 0 || x >= w || y >= static_cast<i32>(l.box.h) || c[3] == 0) {
            return;
        }
        u8 *px = &out[(static_cast<std::size_t>(y) * w + x) * 4];
        const int sa = c[3], da = px[3] * (255 - sa) / 255, oa = sa + da;
        for (int k = 0; k < 3; k++) {
            px[k] = static_cast<u8>(oa == 0 ? 0 : (c[k] * sa + px[k] * da + oa / 2) / oa);
        }
        px[3] = static_cast<u8>(oa);
    };
    const u8 back[4] = {kMinimapNameBack.r, kMinimapNameBack.g, kMinimapNameBack.b,
                        kMinimapNameBack.a};
    const i32 sx = static_cast<i32>(n.strip.x - l.box.x),
              sy = static_cast<i32>(n.strip.y - l.box.y);
    for (i32 y = sy; y < sy + static_cast<i32>(n.strip.h); y++) {
        for (i32 x = sx; x < sx + static_cast<i32>(n.strip.w); x++) {
            over(x, y, back);
        }
    }
    i32 tw = 0, th = 0;
    const std::vector<u8> text = minimap_name_pixels(v.name, n.em, n.max_w, &tw, &th);
    const i32 tx = static_cast<i32>(n.text_x - l.box.x);
    const i32 ty = sy + (static_cast<i32>(n.strip.h) - th) / 2;
    for (i32 y = 0; y < th; y++) {
        for (i32 x = 0; x < tw; x++) {
            over(tx + x, ty + y, &text[(static_cast<std::size_t>(y) * tw + x) * 4]);
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
    const MinimapNameStrip n = minimap_name_strip(v, l);
    if (n.shown) {
        const SDL_FRect strip{n.strip.x, n.strip.y, n.strip.w, n.strip.h};
        SDL_SetRenderDrawColor(renderer, kMinimapNameBack.r, kMinimapNameBack.g, kMinimapNameBack.b,
                               kMinimapNameBack.a);
        ok = SDL_RenderFillRect(renderer, &strip) && ok;
        if (const NameTexture *t = name_texture(renderer, v.name, n.em, n.max_w)) {
            const SDL_FRect text{n.text_x,
                                 n.strip.y + std::floor((n.strip.h - static_cast<float>(t->h)) / 2),
                                 static_cast<float>(t->w), static_cast<float>(t->h)};
            ok = SDL_RenderTexture(renderer, t->texture, nullptr, &text) && ok;
        }
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

const MinimapLayer::NameTexture *MinimapLayer::name_texture(SDL_Renderer *renderer,
                                                            const std::string &text,
                                                            std::int32_t em, std::int32_t max_w) {
    for (const NameTexture &t : names_) {
        if (t.text == text && t.em == em && t.max_w == max_w) {
            return t.texture != nullptr ? &t : nullptr;
        }
    }
    if (names_.size() >= kNameCacheSize) {
        if (names_.front().texture != nullptr) {
            SDL_DestroyTexture(names_.front().texture);
        }
        names_.erase(names_.begin());
    }
    NameTexture t{text, em, max_w, nullptr, 0, 0};
    const std::vector<std::uint8_t> px = minimap_name_pixels(text, em, max_w, &t.w, &t.h);
    if (!px.empty()) {
        t.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                      t.w, t.h);
        if (t.texture != nullptr && !SDL_UpdateTexture(t.texture, nullptr, px.data(), t.w * 4)) {
            SDL_DestroyTexture(t.texture);
            t.texture = nullptr;
        }
        if (t.texture != nullptr) {
            SDL_SetTextureBlendMode(t.texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t.texture, SDL_SCALEMODE_NEAREST);
        }
    }
    stats_.name_uploads++;
    names_.push_back(t); // kept even when empty: no retry every frame
    return names_.back().texture != nullptr ? &names_.back() : nullptr;
}

void MinimapLayer::stop() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    for (const NameTexture &t : names_) {
        if (t.texture != nullptr) {
            SDL_DestroyTexture(t.texture);
        }
    }
    names_.clear();
    renderer_ = nullptr;
    tex_w_ = tex_h_ = 0;
    grid_.clear();
    cols_ = rows_ = 0;
}

} // namespace maplethief::frontend
