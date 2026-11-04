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

#include <cstdint>
#include <string_view>

namespace maplethief::frontend {

struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

// The port's overlays' backdrop over the game (the settings menu, the remap overlay): black at
// this alpha; widescreen's wings get the same veil (Presenter::set_veil, ADR 0103).
inline constexpr std::uint8_t kOverlayBackdropAlpha = 170;

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

    // Text in the 5x7 font (printable ASCII; anything else draws as '?'), each font pixel
    // `size` points, top-left at (x, y). Returns the width drawn in points.
    int text(int x, int y, std::string_view s, Rgba c, int size = 1);
    // The width `text` would draw: 6 * size per character, less the last character's gap.
    static int text_width(std::string_view s, int size = 1);
    static constexpr int kGlyphHeight = 8; // with the descenders of g j p q y

  private:
    // Blends `c` into the pixel (px, py) of the image, if it is inside.
    void blend(int px, int py, Rgba c);

    std::uint8_t *px_;
    int width_, height_, scale_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_CANVAS_H
