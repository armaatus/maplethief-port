// A small drawing surface for the port's own overlays (#70, ADR 0054): rectangles, discs,
// rings, thick diagonals, triangles and text in an original 5x7 pixel font (with descenders), drawn
// over an RGBA8 image (the composited screen, top row first; or with GPU presentation UIKit's
// premultiplied overlay layer, ADR 0068's addendum). Coordinates are the screen's
// points (480x320); each point is `scale` pixels of the image (the render scale), and every
// shape is tested per pixel with integer arithmetic only, so a drawing is the same bytes on
// every CPU and compiler (the overlay captures are checked by hash).
//
// Colours are straight RGBA; each blends "over" the image, which is opaque or premultiplied:
// colour = c * a + dst * (1 - a), alpha = a + dst_alpha * (1 - a), so an opaque image stays
// opaque and the same bytes as before.
#ifndef MAPLETHIEF_PLATFORM_CANVAS_H
#define MAPLETHIEF_PLATFORM_CANVAS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace maplethief::frontend {

struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

// The port's overlays' backdrop over the game (the settings menu, the remap overlay): black at
// this alpha; widescreen's wings get the same veil (Presenter::set_veil, ADR 0103).
inline constexpr std::uint8_t kOverlayBackdropAlpha = 170;

// The code point of the UTF-8 character at s[*at] (strict: no overlong forms, surrogates or
// values past U+10FFFF), moving *at past it; for a byte that starts no such character, -1 and
// *at moves one byte.
std::int32_t next_code_point(std::string_view s, std::size_t *at);

// Every text a Canvas draws, for tests (#372): its box in points (kGlyphHeight * size high) and
// how many of its characters drew as '?' without being one.
struct DrawnText {
    int x = 0, y = 0, w = 0, h = 0;
    std::string_view text;
    int missing = 0;
};
using TextObserver = void (*)(void *user, const DrawnText &t);
// Installs the observer (nullptr: none, the default).
void set_text_observer(TextObserver fn, void *user);

class Canvas {
  public:
    // `rgba`: width x height pixels, 4 bytes each, rows of width * 4 bytes; `scale` pixels per
    // point (at least 1).
    Canvas(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

    // The size in points.
    int width() const { return width_ / scale_; }
    int height() const { return height_ / scale_; }
    int scale() const { return scale_; }

    // A filled rectangle; a frame `t` points thick inside (x, y, w, h).
    void fill(int x, int y, int w, int h, Rgba c);
    void frame(int x, int y, int w, int h, int t, Rgba c);
    // A disc filling the d x d box at (x, y); a ring of it `t` points thick.
    void disc(int x, int y, int d, Rgba c);
    void ring(int x, int y, int d, int t, Rgba c);
    // A rectangle with round ends (a pill): w x h at (x, y), ends of radius h / 2.
    void pill(int x, int y, int w, int h, Rgba c);
    // The two diagonals of the d x d box at (x, y), `t` points thick (an X).
    void cross(int x, int y, int d, int t, Rgba c);
    // An upward triangle in the w x h box at (x, y) (apex at the top centre), filled, or its
    // outline `t` points thick.
    void triangle(int x, int y, int w, int h, Rgba c);
    void triangle_outline(int x, int y, int w, int h, int t, Rgba c);

    // UTF-8 text, top-left at (x, y), each font pixel `size` points. Printable ASCII is drawn in
    // the 5x7 font; any other character with the bundled fonts (NanumGothic, else Fira Sans:
    // mt_cg_bundled_glyph, #372, ADR 0206) at the image's resolution, centred in its cells, on
    // the 5x7 font's baseline. A character neither has, a control character and each byte that
    // is not UTF-8 draw as '?'. Returns the width drawn in points.
    int text(int x, int y, std::string_view s, Rgba c, int size = 1);
    // The width `text` would draw: 6 * size per cell, less the last cell's gap.
    static int text_width(std::string_view s, int size = 1);
    // Text is laid out in cells of 6 x 9 font pixels: one per character, two for an East Asian
    // wide one (Hangul, CJK, fullwidth forms). English is one cell per byte, as before.
    static int char_columns(char32_t cp);
    static int columns(std::string_view s);
    // The bytes of the longest prefix of `s` that fits in `cols` cells, ending at a whole
    // character.
    static std::size_t fit_columns(std::string_view s, int cols);
    // `s` if it draws at most `max_w` points wide at `size`, else its longest prefix that fits
    // with "..." (cut at a whole character), else "" (a translation's room, #372).
    static std::string fit_width(std::string_view s, int max_w, int size = 1);
    static constexpr int kGlyphHeight = 8; // with the descenders of g j p q y
    // The bundled fonts' em, in font pixels: a Hangul syllable fills the 5x7 font's height.
    static constexpr int kFontEm = 9;

  private:
    // Blends `c` into the pixel (px, py) of the image, if it is inside.
    void blend(int px, int py, Rgba c);

    std::uint8_t *px_;
    int width_, height_, scale_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_CANVAS_H
