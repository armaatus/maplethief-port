// The port's minimap (#176, ADR 0098, docs/re/qol.md): the current map's platforms, ladders and
// walls, its portals and NPCs, and the hero, drawn by the frontend into the window. Read only:
// nothing here writes guest memory or changes what the game does.
//
// The data is the game's own, as loaded: CStateGame +0x2C is the map (CMMap), whose .att grid
// the .map loader 0xD6D1 allocated and CMMap::LoadAtt 0xD5B1 filled (docs/formats/att.md):
//
//   CMMap +0x04, +0x08   the tile's size in pixels (16 x 16)
//   CMMap +0x0C, +0x10   the attribute cell's size in pixels (8 x 8: half a tile)
//   CMMap +0x20          the map's width in tiles (the .obj cell's row length)
//   CMMap +0x28, +0x2C   the grid's width and height in cells (2w x 2h)
//   CMMap +0x3C, +0x40   the grid (one flag byte per cell, row-major) and its size in bytes
//   CMMap +0x48, +0x4C   the objects (12-byte Obj*: cell i16 +0, kind i16 +2, visible i16 +6)
//                        and their count (docs/formats/obj.md, CMMap::LoadObj 0xD259)
//   CStateGame +0x30     the hero (CHero): x +0x04, y +0x08 (its feet, in map pixels)
//
// Guest memory is untrusted data here: every read is range-checked against the image segments
// and the committed heap (GuestSpan), the grid's sides are capped (kMinimapMaxSide), it must fit
// in its buffer, and at most kMinimapMaxObjects objects are read. Anything out of range: no
// minimap this frame. Shown only in the field: a CStateGame (vtable 0x119B78) in game mode 0.
//
// Where it goes (minimap_layout): `border` in the bar to the left of the picture (the wide
// picture with widescreen, ADR 0090; the gear has the right bar), or below it on a window
// taller than 3:2, when the bar has room; else, and with `overlay`, over the screen image's
// top edge just right of the compass (Bag, Cash and the gear are at the top right). The screen
// image is the middle 480 points with widescreen too, so the overlay sits at the same place
// over the HUD's at 3:2 and wide.
//
// How it draws: the grid is reduced to the box (minimap_grid_pixels: each box pixel takes the
// most important flag of the cells it covers: foothold, then ladder, then wall), then the
// markers (minimap_dots) over it. render_minimap does both on the CPU (the golden image);
// MinimapLayer uploads the grid only when it changes and fills the dots with the renderer.
// Drawn after the read-backs, like the frame-time overlay: never in a shot, a golden or a
// capture (Presenter step 5, border.h).
#ifndef MAPLETHIEF_PLATFORM_MINIMAP_H
#define MAPLETHIEF_PLATFORM_MINIMAP_H

#include <cstdint>
#include <functional>
#include <vector>

#include "maplethief/recomp/cpu.h"
#include "scaling.h"
#include "settings.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace maplethief::frontend {

// Guest facts (docs/formats/att.md, docs/formats/obj.md).
inline constexpr std::uint32_t kMinimapStateMap = 0x2C;  // CStateGame: the CMMap
inline constexpr std::uint32_t kMinimapStateHero = 0x30; // CStateGame: the CHero
inline constexpr std::uint32_t kMapTileW = 0x04, kMapTileH = 0x08;
inline constexpr std::uint32_t kMapCellW = 0x0C, kMapCellH = 0x10;
inline constexpr std::uint32_t kMapTilesWide = 0x20;
inline constexpr std::uint32_t kMapAttW = 0x28, kMapAttH = 0x2C;
inline constexpr std::uint32_t kMapAtt = 0x3C, kMapAttSize = 0x40;
inline constexpr std::uint32_t kMapObjects = 0x48, kMapObjectCount = 0x4C;

// The .att flags (docs/formats/att.md, Values).
inline constexpr std::uint8_t kAttFoothold = 0x02, kAttLadder = 0x04, kAttWall = 0x10;

inline constexpr std::int32_t kMinimapMaxSide = 1024;  // cells (shipped: at most 400)
inline constexpr std::int32_t kMinimapMaxObjects = 64; // shipped: at most 23 records
inline constexpr std::int32_t kMinimapMaxCellSide = 64;

// `n` bytes of guest memory at `addr`, or nullptr if any of them is not game memory.
using GuestSpan = std::function<const std::uint8_t *(std::uint32_t addr, std::uint32_t n)>;
// The game's memory: an image segment or the committed heap (the whole range in one of them).
GuestSpan guest_span(const mt_cpu *cpu);

struct MinimapMarker {
    enum Kind : std::uint8_t { kPortal, kNpc };
    Kind kind = kPortal;
    std::int32_t x = 0, y = 0; // the object's anchor (its tile's bottom centre), map pixels
    bool operator==(const MinimapMarker &) const = default;
};

// What the minimap shows, read from the game in one frame.
struct MinimapView {
    bool shown = false;                  // the field, with a map and a hero that read cleanly
    std::int32_t cols = 0, rows = 0;     // the grid in cells
    std::int32_t cell_w = 8, cell_h = 8; // a cell in map pixels
    std::vector<std::uint8_t> grid;      // cols x rows flags, top row first
    std::int32_t hero_x = 0, hero_y = 0; // the hero's feet, map pixels
    std::vector<MinimapMarker> markers;  // portals (kinds 0-2) and visible NPCs (4-59)
    std::int32_t map_w() const { return cols * cell_w; } // map pixels
    std::int32_t map_h() const { return rows * cell_h; }
};
MinimapView read_minimap(const GuestSpan &mem);

// Where the minimap goes in the drawable: `box` (whole pixels), and `scale` drawable pixels per
// map pixel. Not shown when the mode is off or the map is empty.
struct MinimapLayout {
    bool shown = false;
    bool in_border = false; // in a bar (border mode with room), else over the picture
    Rect box;
    float scale = 0;
    float point = 1; // drawable pixels per screen point (the dots' size)
};
// `image`: the screen image's placement (Presenter::placement), its safe area the bars' limit;
// `wide`: the wide picture's rectangle (Presenter::wide_rect; the placement's when off).
MinimapLayout minimap_layout(Minimap mode, const Placement &image, const Rect &wide,
                             std::int32_t map_w, std::int32_t map_h);

// Sizes in screen points.
inline constexpr float kMinimapMargin = 4;     // from the bar's or picture's edges
inline constexpr float kMinimapOverlayX = 46;  // the overlay's left edge: right of the compass
inline constexpr float kMinimapOverlayW = 150; // the overlay's largest box
inline constexpr float kMinimapOverlayH = 64;
inline constexpr float kMinimapMinBorderW = 48;   // a bar narrower than this: the overlay
inline constexpr float kMinimapMinBorderH = 24;   // a bar lower than this: the overlay
inline constexpr float kMinimapMaxScale = 0.125f; // points per map pixel: a tile at most 2
inline constexpr float kMinimapHeroDot = 4, kMinimapMarkerDot = 3;

// Map pixel (x, y) in the drawable (the box's top left plus x, y times the scale).
void minimap_point(const MinimapLayout &l, float map_x, float map_y, float *x, float *y);

// The colours (straight RGBA).
struct MinimapColor {
    std::uint8_t r, g, b, a;
};
inline constexpr MinimapColor kMinimapBack{16, 20, 28, 176};
inline constexpr MinimapColor kMinimapFoothold{236, 236, 226, 255};
inline constexpr MinimapColor kMinimapLadder{232, 182, 72, 255};
inline constexpr MinimapColor kMinimapWall{124, 134, 146, 255};
inline constexpr MinimapColor kMinimapPortal{84, 164, 255, 255};
inline constexpr MinimapColor kMinimapNpc{112, 222, 112, 255};
inline constexpr MinimapColor kMinimapHero{255, 72, 56, 255};
inline constexpr MinimapColor kMinimapHeroEdge{255, 255, 255, 255};

// A filled rectangle in drawable pixels, whole pixels, inside the box.
struct MinimapDot {
    Rect rect;
    MinimapColor color;
};
// The markers, then the hero (a white edge, then its fill) over them: what is drawn over the
// grid, in order. The dots are centred a cell above the anchor (the body, not the feet) and
// clamped into the box.
std::vector<MinimapDot> minimap_dots(const MinimapView &v, const MinimapLayout &l);
// The hero's dot (its fill), for the tests.
Rect minimap_hero_dot(const MinimapView &v, const MinimapLayout &l);

// The grid reduced to the box: box.w x box.h straight RGBA8, top row first.
std::vector<std::uint8_t> minimap_grid_pixels(const MinimapView &v, const MinimapLayout &l);
// The grid and the dots, over a transparent image of the box's size (the dots relative to it).
std::vector<std::uint8_t> render_minimap(const MinimapView &v, const MinimapLayout &l);

// The minimap on the window's renderer.
class MinimapLayer {
  public:
    MinimapLayer() = default;
    MinimapLayer(const MinimapLayer &) = delete;
    MinimapLayer &operator=(const MinimapLayer &) = delete;
    ~MinimapLayer() { stop(); }

    // Draws `v` at `l` (nothing when either is not shown). Leaves the renderer's draw colour and
    // blend mode as they were. False if a draw failed (logged once).
    bool draw(SDL_Renderer *renderer, const MinimapView &v, const MinimapLayout &l);
    // Frees the texture (before the renderer goes).
    void stop();

    struct Stats {
        std::uint64_t draws = 0;   // frames it was drawn
        std::uint64_t uploads = 0; // grid uploads (a new map or box)
        bool last_border = false;  // the last draw was in a bar
    };
    const Stats &stats() const { return stats_; }

  private:
    SDL_Texture *texture_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    std::vector<std::uint8_t> grid_; // the uploaded grid's flags and size
    std::int32_t cols_ = 0, rows_ = 0;
    bool failed_logged_ = false;
    Stats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_MINIMAP_H
