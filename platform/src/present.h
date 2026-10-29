// Presentation (#58, ADR 0042): from the game's presented GL renderbuffer to the SDL window.
//
//   -[EAGLContext presentRenderbuffer:]  ->  Screen::take_gl_frame   (the GL present hook)
//       the renderbuffer's mt_gfx target, copied as the opaque CAEAGLLayer shows it
//   the UIKit turn hook                  ->  Screen::compose
//       mt_uikit_composite(FULL) at 480x320 x render scale, the GL frame in the GL view's place
//                                        ->  Presenter::upload, Presenter::draw
//       an SDL streaming texture (R, G, B, A bytes, blend none, nearest) drawn into
//       present_rect() of the window's drawable, black around it
//
// Retained backing. The game asks for none and never calls glClear, but mt_gfx never clears
// a target either: what the game drew stays in the renderbuffer until it draws over it, and
// each present shows the target exactly as drawn. Screen::take_gl_frame copies it at the
// present, so later draws (before the next present) cannot tear the shown frame.
//
// Orientation. The game draws landscape directly: its EAGLView is turned (transform
// (0, -1, 1, 0), bounds 480x320), the renderbuffer is 480x320 and the projection is
// glOrthox(0, 480, 0, 320, -1, 1). mt_gfx stores clip-space y = +1 (GL's top) in its first
// row, which is the top row first, upright image that mt_uikit_gl_frame wants, so the frame
// goes into the compositor unchanged.
//
// The seam for #59 (scaling modes, HiDPI, mouse mapping): present_rect() is the one place that
// decides where the screen image lands in the drawable and how it is filtered. Today it is a
// plain aspect fit (fit_rect) with nearest filtering.
#ifndef MAPLETHIEF_PLATFORM_PRESENT_H
#define MAPLETHIEF_PLATFORM_PRESENT_H

#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/gles.h"
#include "maplethief/recomp/recomp.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace maplethief::frontend {

// A rectangle in output pixels (x, y: top left).
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

// The largest rectangle with the image's aspect ratio that fits into the output, centred (black
// bars on the other axis). The scale need not be an integer. Empty (all 0) if a size is 0 or
// negative.
Rect fit_rect(int output_w, int output_h, int image_w, int image_h);

// Where and how the screen image is drawn into the drawable.
struct Placement {
    Rect rect;
    bool nearest = true; // nearest filtering; false: linear
};

// The seam (#59): the placement of an image_w x image_h screen image in an output_w x
// output_h drawable (pixels, i.e. HiDPI-aware). Today fit_rect, nearest.
Placement present_rect(int output_w, int output_h, int image_w, int image_h);

// The screen image: the latest presented GL frame and the composited screen. No SDL in it, so
// tests drive it on a synthetic guest.
class Screen {
  public:
    // render_scale: the renderbuffers' scale (mt_gles_config.render_scale), 1 to 8; the screen
    // is composited at 480x320 times it.
    explicit Screen(std::uint32_t render_scale);

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

    // The present hook's half: copies the frame's target as the opaque CAEAGLLayer shows it
    // (RGB as drawn, alpha 255). Returns false if the frame has no target.
    bool take_gl_frame(const mt_gles_frame &frame);
    // The index of the last GL frame taken (mt_gles_frame.index), 0 if none.
    std::uint32_t gl_frame() const { return gl_index_; }
    // The index of the first GL frame taken that is not black (see is_black), 0 if none yet.
    std::uint32_t first_lit_gl_frame() const { return first_lit_gl_; }
    std::uint32_t gl_width() const { return gl_width_; }
    std::uint32_t gl_height() const { return gl_height_; }
    // The copy: gl_width x gl_height RGBA8, top row first.
    const std::vector<std::uint8_t> &gl_pixels() const { return gl_; }

    // Composites the screen at time `now` (FULL mode) with the GL frame (if one was taken) in
    // the GL view's place. Returns false if UIKit has nothing to composite (no application or
    // window).
    bool compose(mt_cpu *cpu, double now);
    // The composited screen: width x height premultiplied RGBA8, top row first, opaque.
    const std::vector<std::uint8_t> &pixels() const { return screen_; }

  private:
    std::uint32_t width_, height_;
    std::uint32_t gl_index_ = 0, first_lit_gl_ = 0;
    std::uint32_t gl_width_ = 0, gl_height_ = 0;
    std::vector<std::uint8_t> gl_, screen_;
};

// Whether an RGBA8 image is black: at most 1% of its pixels have r + g + b above 24.
bool is_black(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
              std::uint32_t stride);

// The SDL half: a streaming texture of the screen image, drawn into the window.
class Presenter {
  public:
    Presenter() = default;
    Presenter(const Presenter &) = delete;
    Presenter &operator=(const Presenter &) = delete;
    ~Presenter() { stop(); }

    // Creates the image_w x image_h texture (R, G, B, A bytes: SDL_PIXELFORMAT_ABGR8888, blend
    // none). Returns false (logged) if it cannot.
    bool start(SDL_Renderer *renderer, std::uint32_t image_w, std::uint32_t image_h);
    // Frees the texture (before the renderer goes).
    void stop();

    // Uploads an image of the texture's size (RGBA8, top row first).
    bool upload(const std::uint8_t *rgba, std::uint32_t stride);
    // Clears the drawable to black and draws the texture at present_rect() of the drawable's
    // pixel size (SDL_GetCurrentRenderOutputSize). The caller calls SDL_RenderPresent.
    bool draw();
    // Where the last draw() put the image.
    const Placement &placement() const { return placement_; }

    // Reads the drawn output back (before SDL_RenderPresent) and checks that it shows `rgba`
    // (the uploaded image, RGB only: the window has no alpha): the output pixel under each
    // image pixel's centre, and black outside the placement. Returns the number of mismatched
    // pixels (-1 if nothing could be read); `report` gets a summary.
    long check_readback(const std::uint8_t *rgba, std::uint32_t stride, std::string *report);
    // Reads the drawn output back (before SDL_RenderPresent) into a PNG file.
    bool save_readback(const std::string &path);

  private:
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;
    Placement placement_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PRESENT_H
