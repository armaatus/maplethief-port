// The border (#157, ADR 0085): what fills the drawable around the 3:2 picture on a 16:9, 16:10
// or ultrawide screen. No gameplay change: the game still sees its 480 x 320 screen, and the
// placement (scaling.h) is the same in every mode.
//
//   color  the border colour (border_color): Presenter::draw's clear, exactly as before #157
//   blur   the current frame, stretched to cover the drawable about the picture's centre (so it
//          continues the picture into the bars), blurred and dimmed
//   image  the player's PNG (border_image), scaled to cover the drawable, over the border colour
//
// The present stage draws, in order, each step over the last (Presenter::draw):
//   W. widescreen's wide picture (#170, ADR 0090): with widescreen and the wings drawn, the GL
//      frame's first `view` points, the screen image over their middle 480 and the key pad's
//      strips (#168, premultiplied) over the wings, composed into one render target of view x
//      320 points at the screen texture's texels a point. The steps below then read that picture
//      in place of the screen image, so the grade, the blur, the scale mode's filter and the
//      pass see one continuous image (no seam at the wings' edges). Without wings (widescreen
//      off, a narrow view) nothing is composed and they read the screen image, as before
//   0. the colour-blind filter (#158, ADR 0093, color_filter.h): grades the image that steps 2
//      and 3 read (on the CPU at upload: the screen image, the wings' columns and the strips; or
//      through its GPU pass into an image-sized target: the screen image or the wide picture);
//      it draws nothing itself
//   1. the clear in the border colour (every mode; the minimised window stops here)
//   2. the border's step (Border::draw): the blur or the art over the whole drawable
//   3. the picture: the wide picture into the wings' rectangle, or the screen texture into the
//      placement, through SDL's texture shader with the scale mode's filter, or a filter's pass
//      (ADR 0066) with the image's texels as its source size. Should the wide picture have no
//      render target, the wings first (the scale mode's filter only), then the screen image
//   4. with widescreen and the touch key pad at the view's edges (#168), the parts of the pad's
//      strips that the wide picture doesn't hold (beyond a view narrower than the wide picture,
//      or all of them without it), over the bars (premultiplied)
// and then the window-only overlays (the focus frame, the frame-time overlay, the price line,
// the settings menu).
//
// The blur runs on the window's renderer with render targets, so it needs no shader, no
// read-back and no GPU renderer: it is the same on both backends (the screen texture is the
// CPU's upload or GPU presentation's render target), under every filter and scale mode.
//   - Down: the screen texture is halved, bilinear at 2:1 (each texel the mean of the 2 x 2 it
//     covers), level by level until a level is at most kBlurBaseWidth texels wide (about 16
//     screen points a texel at 480 wide).
//   - Up: the levels are doubled back, bilinear at 1:2, up to the first one at most kBlurTopWidth
//     wide (about 4 points a texel). Repeated 2x bilinear steps approach a quadratic B-spline, so
//     the result is smooth where a single 16x bilinear stretch would show its diamond pattern.
//   - That level is drawn with linear filtering over cover_about(the placement), dimmed to
//     kBlurDim / 255 (a colour mod) so that the picture and the port's overlays stand out.
// A renderer without render targets keeps the border colour (logged once).
//
// The art is decoded with the pinned stb_image as untrusted data (decode_border_image): at most
// kMaxBorderImageBytes, its header read before anything is decoded, each side at most
// kMaxBorderImageSide and at most kMaxBorderImagePixels pixels, and no larger than the
// renderer's largest texture. Drawn with linear filtering into cover_rect(the drawable), blended
// (straight alpha) over the border colour. A file that is missing, not a PNG, too large or
// broken keeps the border colour (logged once, with the reason).
#ifndef MAPLETHIEF_PLATFORM_BORDER_H
#define MAPLETHIEF_PLATFORM_BORDER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scaling.h"
#include "settings.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace maplethief::frontend {

inline constexpr int kBlurBaseWidth = 32;
inline constexpr int kBlurTopWidth = 128;
inline constexpr std::uint8_t kBlurDim = 150; // of 255: the blurred frame at about 59%

inline constexpr std::size_t kMaxBorderImageBytes = 32u << 20;
inline constexpr std::uint32_t kMaxBorderImageSide = 8192;
inline constexpr std::uint64_t kMaxBorderImagePixels = 7680ull * 4320; // 8K UHD

// The blur's levels for a source_w x source_h screen texture: levels[0] is half the source
// (rounded up), each next one half the last, the last at most kBlurBaseWidth wide (or 1 x 1).
// `top` is the level drawn over the drawable: the first at most kBlurTopWidth wide. Empty for an
// empty source.
struct BlurLevel {
    int w = 0, h = 0;
    bool operator==(const BlurLevel &) const = default;
};
struct BlurChain {
    std::vector<BlurLevel> levels;
    std::size_t top = 0;
};
BlurChain blur_chain(int source_w, int source_h);

// A decoded border image: width x height straight RGBA8, top row first.
struct BorderImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
// Decodes a PNG within the limits above. False (out untouched) with the reason otherwise.
bool decode_border_image(const std::uint8_t *data, std::size_t size, BorderImage *out,
                         std::string *why);
// Reads the file (UTF-8 path, its size checked before it is read) and decodes it.
bool load_border_image(const std::string &path, BorderImage *out, std::string *why);

class Border {
  public:
    Border() = default;
    Border(const Border &) = delete;
    Border &operator=(const Border &) = delete;
    ~Border() { release(); }

    // The mode and the art's file for the next draws. A new file is read at the next draw that
    // needs it (once: a failure is logged and keeps the border colour until the file changes).
    void set(BorderMode mode, const std::string &image_path);
    BorderMode mode() const { return mode_; }
    const std::string &image_path() const { return path_; }
    // What the last draw drew: blur or image, or color when the mode is color, the window was
    // minimised, or the mode could not be drawn.
    BorderMode drawn() const { return drawn_; }

    // Step 2: draws the border over the whole render target (output_w x output_h pixels: the
    // drawable, or the target a test draws into), after the clear, around `image` (the
    // placement's rectangle). `screen` is the screen texture (screen_w x screen_h): its scale
    // mode is put back afterwards, and so is the renderer's target. Nothing for color. False
    // (logged) if a draw failed.
    bool draw(SDL_Renderer *renderer, SDL_Texture *screen, int screen_w, int screen_h,
              const Rect &image, int output_w, int output_h);
    // Frees the textures (before their renderer goes). The mode and the file stay.
    void release();

    // The blur's level textures (tests).
    std::size_t levels() const { return levels_.size(); }

  private:
    bool ensure_chain(SDL_Renderer *renderer, int screen_w, int screen_h);
    bool ensure_art(SDL_Renderer *renderer);
    bool draw_blur(SDL_Renderer *renderer, SDL_Texture *screen, const Rect &image, int output_w,
                   int output_h);
    void free_chain();
    void free_art();

    BorderMode mode_ = BorderMode::kColor;
    BorderMode drawn_ = BorderMode::kColor;
    std::string path_;
    SDL_Renderer *renderer_ = nullptr; // the textures' renderer
    BlurChain chain_;
    int chain_w_ = 0, chain_h_ = 0;
    std::vector<SDL_Texture *> levels_;
    bool blur_failed_ = false, blur_logged_ = false;
    SDL_Texture *art_ = nullptr;
    int art_w_ = 0, art_h_ = 0;
    bool art_failed_ = false;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_BORDER_H
