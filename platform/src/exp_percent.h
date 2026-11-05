// The hero's EXP as a percentage beside the field HUD's bar (#191, ADR 0109,
// docs/re/hud-extras.md): "EXP 42.17%". The game draws the EXP only as a 2-point line under its
// HP / MP bar (HUD_Paint 0x5A465: 153 points wide at full EXP, at (163, 317)) and as a whole
// percentage in the popup menu's status tab (PopupMenu_PaintStatusTab 0x56321); the field shows no
// figure. Read only: nothing here writes guest memory or changes what the game does.
//
// The figures are the game's, as HUD_Paint reads them:
//
//   SkyCanvas +0x0C   the current state: a CStateGame (vtable 0x119B78) in game mode 0 (the field)
//   CStateGame +0x30  the hero (CHero): level +0x80, EXP +0x90 (what the level has gathered)
//   *0x11A9A8         hero.dat as loaded (docs/formats/data-tables.md): u16 rows +2, u16 stride
//                     +4, rows from +6; the row whose u8 key +0 is the level (the game's lookup
//                     0x3990D) holds the EXP the level needs, i32 at +1 (CHero::ExpToLevelUp
//                     0x65919 through the i32 accessor 0x385D1)
//
// Guest memory is untrusted data: every read is range-checked (GuestSpan, minimap.h), the
// table's size is capped and a level must find its row. Anything out of range: no figure.
//
// Where: the screen image's points just above the HUD's plate, right of nothing and left of the
// level badge: the box's right edge at kExpPercentRight, its bottom at kExpPercentBottom, over
// the HP gauge (x 155 ... 214). The HUD keeps its 3:2 place with widescreen (the port moves it
// into the middle, ADR 0090), and so does the box. The glyphs are the HUD labels' (ADR 0101):
// the port's 5x7 font, a font pixel about three quarters of a screen point (hud_label_unit), on
// the prompts' dark backing, drawn on its own layer at the window's pixels after the read-backs, like the
// minimap: never in a shot, a golden, a capture or the regression replay's hashes.
#ifndef MAPLETHIEF_PLATFORM_EXP_PERCENT_H
#define MAPLETHIEF_PLATFORM_EXP_PERCENT_H

#include <cstdint>
#include <string>
#include <vector>

#include "minimap.h"
#include "scaling.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace maplethief::frontend {

// Guest facts (docs/re/hud-extras.md).
inline constexpr std::uint32_t kExpHeroLevel = 0x80;      // CHero: the level
inline constexpr std::uint32_t kExpHeroExp = 0x90;        // CHero: the level's EXP so far
inline constexpr std::uint32_t kExpHeroTable = 0x11A9A8;  // hero.dat's buffer pointer
inline constexpr std::int32_t kExpMaxTableRows = 1024;    // shipped: 100
inline constexpr std::int32_t kExpMinTableStride = 5;     // the key and the EXP column
inline constexpr std::int32_t kExpMaxTableStride = 256;   // shipped: 18

// What the figure shows, read from the game in one frame.
struct ExpView {
    bool shown = false; // the field, with a hero whose level found its row
    std::int32_t level = 0;
    std::int32_t exp = 0;    // CHero +0x90
    std::int32_t needed = 0; // the level's row's EXP (ExpToLevelUp)
};
ExpView read_exp(const GuestSpan &mem);

// "EXP 42.17%": exp / needed in hundredths of a percent, rounded down, clamped to 0 ... 99.99
// (setExp levels up as soon as the EXP reaches the need, so the game never holds a full level).
// Empty when needed is 0 or less.
std::string exp_percent_text(std::int32_t exp, std::int32_t needed);

// Where (screen points of the screen image).
inline constexpr float kExpPercentRight = 213;  // the level badge starts at x 215
inline constexpr float kExpPercentBottom = 303; // the plate starts at y 304
inline constexpr float kExpPercentLeft = 150;   // the plate's left end: the box never passes it
// The text's backing: one glyph point around the 7 rows of digits and capitals.
inline constexpr int kExpPercentRows = 7;

// The box of `text` in glyph points (the canvas's), its backing included.
int exp_percent_units_w(const std::string &text);
inline constexpr int kExpPercentUnitsH = kExpPercentRows + 2;

// The box in screen points for a glyph unit of `unit` pixels and `pixels` pixels a point (the
// layer's: the placement's pixels a point, rounded, 1 to 8).
Rect exp_percent_points(const std::string &text, int unit, int pixels);
// The layer's pixels a screen point for `image` (the placement's rectangle).
int exp_percent_pixels(const Rect &image);
// The box in drawable pixels over `image`, on whole pixels.
Rect exp_percent_rect(const Rect &image, const std::string &text);

// The figure's pixels: premultiplied RGBA8 over transparent (`units_w` x kExpPercentUnitsH glyph points at `unit`
// pixels each, top row first), the backing and the text.
std::vector<std::uint8_t> render_exp_percent(const std::string &text, int unit);

// The figure on the window's renderer.
class ExpPercentLayer {
  public:
    ExpPercentLayer() = default;
    ExpPercentLayer(const ExpPercentLayer &) = delete;
    ExpPercentLayer &operator=(const ExpPercentLayer &) = delete;
    ~ExpPercentLayer() { stop(); }

    // Draws `text` (nothing when empty) at exp_percent_rect over `image`. Call after the
    // read-backs and captures, before SDL_RenderPresent. False (logged once) if the texture
    // cannot be made or drawn.
    bool draw(SDL_Renderer *renderer, const Rect &image, const std::string &text);
    // Frees the texture (before the renderer goes).
    void stop();
    // How often the figure was drawn anew (a new text or size), and drawn at all.
    unsigned redraws() const { return redraws_; }
    unsigned draws() const { return draws_; }

  private:
    SDL_Texture *texture_ = nullptr;
    std::string text_;
    int unit_ = 0, tex_w_ = 0, tex_h_ = 0;
    bool told_ = false;
    unsigned redraws_ = 0, draws_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_EXP_PERCENT_H
