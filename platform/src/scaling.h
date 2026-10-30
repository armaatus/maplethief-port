// Scaling (#59, ADR 0043): where the screen image lands in the window's drawable, how it is
// filtered, and the inverse that maps window (mouse, touch) coordinates back to the game's
// screen points. Pure arithmetic, no SDL: present.cpp applies it, tests check it.
//
// Three spaces:
//   screen points   the game's landscape screen, 480 x 320 (ADR 0037; mt_uikit_touch takes
//                   these). The screen image is 480 x 320 x render scale texels.
//   drawable pixels the renderer's output (SDL_GetCurrentRenderOutputSize): on a HiDPI display
//                   the window's size times its pixel density. The placement lives here.
//   window points   SDL's window coordinates (SDL_GetWindowSize, mouse events). Drawable pixels
//                   = window points x density (per axis: pixel size / window size).
//
// The modes (the config's scale_mode, --scale-mode):
//   integer (default) the largest whole multiple k of the 480 x 320 screen that fits, centred,
//                     nearest. k scales the screen points, whatever the render scale N: the image
//                     is 480k x 320k pixels and each of its texels k/N pixels (whole when N
//                     divides k, always at N = 1). A drawable smaller than 480 x 320 falls back
//                     to fit.
//   fit               the largest rectangle of the screen's aspect ratio, whole pixels, centred,
//                     nearest.
//   smooth            the fit rectangle, bilinear (SDL_SCALEMODE_LINEAR).
//   sharp             the fit rectangle, SDL_SCALEMODE_PIXELART (SDL 3.4): nearest inside a
//                     texel, blended across texel edges (sharp bilinear) on the GPU renderers;
//                     SDL's software renderer draws it nearest.
// Around the image: bars in the border colour (border_color, --border-color; default black), or
// the frame blurred or the player's art (border, #157, ADR 0085: border.h).
//
// The safe area (#73, ADR 0055): on a phone with a notch, the Dynamic Island or a home
// indicator, the system covers strips along the window's edges (SDL_GetWindowSafeArea; on iOS
// in the current landscape orientation, so they swap sides when the phone turns over). The
// image is sized for the safe area and lies wholly inside it; only the bars extend under the
// strips. Placement: the mode's size for the safe area, centred in the whole drawable as with
// no insets, then moved (clamped) into the safe area if it overlaps a strip. Zero insets (the
// desktop) give exactly the placement without them.
#ifndef MAPLETHIEF_PLATFORM_SCALING_H
#define MAPLETHIEF_PLATFORM_SCALING_H

#include <cstdint>
#include <string_view>

namespace maplethief::frontend {

// The game's screen in points (MT_UIKIT_SCREEN_WIDTH x MT_UIKIT_SCREEN_HEIGHT).
inline constexpr int kScreenWidth = 480;
inline constexpr int kScreenHeight = 320;

enum class ScaleMode { kInteger, kFit, kSmooth, kSharp };

// "integer", "fit", "smooth", "sharp".
const char *scale_mode_name(ScaleMode mode);
// The name (ASCII case-insensitive) to the mode; false if it names none.
bool parse_scale_mode(std::string_view name, ScaleMode *mode);

// The texture's filter (SDL_SCALEMODE_NEAREST, _LINEAR, _PIXELART).
enum class Filter { kNearest, kLinear, kPixelArt };

// An opaque colour.
struct Color {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Color &o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Color &o) const { return !(*this == o); }
};

// "#RRGGBB" (hex digits in either case) to the colour; false otherwise.
bool parse_color(std::string_view text, Color *color);

// A rectangle in drawable pixels (x, y: top left).
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

// The largest rectangle with the image's aspect ratio that fits into the output, its size
// rounded to whole pixels, centred (bars on the other axis). The scale need not be an integer.
// Empty (all 0) if a size is 0 or negative.
Rect fit_rect(int output_w, int output_h, int image_w, int image_h);

// The smallest rectangle with the image's aspect ratio that covers the output (the border art,
// #157): its size rounded up to whole pixels, centred, cropped by the output on one axis. Empty
// (all 0) if a size is 0 or negative.
Rect cover_rect(int output_w, int output_h, int image_w, int image_h);
// `image` (a placement's rectangle) scaled about its centre until it covers the output (the
// blurred border, #157: the frame stretched so that it continues the picture into the bars).
// Empty if `image` is.
Rect cover_about(const Rect &image, int output_w, int output_h);

// Where and how the screen image is drawn into the drawable.
struct Placement {
    Rect rect; // in drawable pixels; empty: nothing to draw into
    Filter filter = Filter::kNearest;
    ScaleMode mode = ScaleMode::kInteger; // the mode asked for
    int integer_scale = 0; // integer mode: k (the image is 480k x 320k); 0 if it fell back to fit
                           // or the mode is not integer
    Rect safe;             // the safe area it was placed in (the whole output without insets)
};

// The placement of the screen image in an output_w x output_h drawable (pixels). The image is
// image_w x image_h texels (480 x 320 x render scale): its aspect ratio sets the fit modes'
// rectangle; integer mode scales the 480 x 320 screen points.
Placement place(ScaleMode mode, int output_w, int output_h, int image_w, int image_h);

// The safe-area insets: how far the system's strips reach in from each edge of the window, in
// window points (SDL_GetWindowSafeArea's complement). Negative or non-finite sides count as 0.
struct Insets {
    float top = 0, left = 0, bottom = 0, right = 0;
    bool any() const { return top > 0 || left > 0 || bottom > 0 || right > 0; }
    bool operator==(const Insets &o) const {
        return top == o.top && left == o.left && bottom == o.bottom && right == o.right;
    }
    bool operator!=(const Insets &o) const { return !(*this == o); }
};

// "T,L,B,R" (window points, each 0 to 4096, decimals allowed) to insets; false otherwise. The
// --safe-area test flag.
bool parse_insets(std::string_view text, Insets *insets);

// The safe area in drawable pixels: the output less each inset x density (density_x for left
// and right, density_y for top and bottom; <= 0 counts as 1), rounded up to whole pixels so
// the image never reaches into a strip. Empty (w or h 0) if the insets cover the output.
Rect safe_rect(int output_w, int output_h, const Insets &insets, float density_x, float density_y);

// The placement inside the safe area: as place() above, sized for the safe area, centred in
// the whole output, then clamped into the safe area (scaling.h's header). Placement::safe is
// the safe area; with zero insets the result equals place()'s.
Placement place(ScaleMode mode, int output_w, int output_h, int image_w, int image_h,
                const Insets &insets, float density_x, float density_y);

// A point in the game's screen points. `inside` is false over the bars (or with no image); x
// and y are then still the extrapolated screen coordinates (outside 0..480 x 0..320), for a
// caller that wants to clamp a drag that leaves the image.
struct ScreenPoint {
    float x = 0, y = 0;
    bool inside = false;
};

// Drawable pixel coordinates (continuous: the pixel (i, j) covers [i, i + 1) x [j, j + 1)) to
// screen points, the inverse of screen_to_drawable. inside: 0 <= x < 480 and 0 <= y < 320.
ScreenPoint drawable_to_screen(const Placement &p, float px, float py);
// Screen points to drawable pixel coordinates (the forward map of the image).
void screen_to_drawable(const Placement &p, float sx, float sy, float *px, float *py);

// Window points (SDL mouse and touch-as-mouse coordinates) to screen points: drawable = window
// x density (density_x = drawable width / window width, likewise y; 2 on a Retina display),
// then drawable_to_screen. The function M7's mouse input (#65) calls before mt_uikit_touch.
ScreenPoint window_to_screen(const Placement &p, float wx, float wy, float density_x,
                             float density_y);
// The forward map: screen points to window points.
void screen_to_window(const Placement &p, float sx, float sy, float density_x, float density_y,
                      float *wx, float *wy);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SCALING_H
