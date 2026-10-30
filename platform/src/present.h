// Presentation (#58, ADR 0042): from the game's presented GL renderbuffer to the SDL window.
//
//   -[EAGLContext presentRenderbuffer:]  ->  Screen::take_gl_frame   (the GL present hook)
//       the renderbuffer's mt_gfx target, copied as the opaque CAEAGLLayer shows it
//   the UIKit turn hook                  ->  Screen::compose
//       mt_uikit_composite(FULL) at 480x320 x render scale, the GL frame in the GL view's place
//                                        ->  Presenter::upload, Presenter::draw
//       an SDL streaming texture (R, G, B, A bytes, blend none) drawn into place() of the
//       window's drawable (scaling.h: the scale mode, its filter), the border colour around it
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
// Scaling (#59, ADR 0043): scaling.h's place() is the one place that decides where the screen
// image lands in the drawable and how it is filtered; window_point_to_screen() is its inverse
// for mouse and touch input.
#ifndef MAPLETHIEF_PLATFORM_PRESENT_H
#define MAPLETHIEF_PLATFORM_PRESENT_H

#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/gles.h"
#include "maplethief/recomp/recomp.h"
#include "scaling.h"

struct SDL_GPURenderState;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace maplethief::frontend {

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

    // The scale mode and the bars' colour for the next draw() (default: integer, black).
    void set_scaling(ScaleMode mode, Color border);
    // The present stage's pass (#120, ADR 0060; gfx_backend.h's PresentPass): a fragment shader
    // that SDL's GPU renderer draws the image through, in place of its own; nullptr (the
    // default): SDL's. Only with SDL's GPU renderer. The pass replaces the scale mode's shader,
    // so it does its own filtering (the sampler is still the scale mode's).
    void set_render_state(SDL_GPURenderState *state) { render_state_ = state; }
    ScaleMode scale_mode() const { return mode_; }
    Color border_color() const { return border_; }

    // Uploads an image of the texture's size (RGBA8, top row first).
    bool upload(const std::uint8_t *rgba, std::uint32_t stride);
    // Clears the drawable to the border colour and draws the texture at place() of the
    // drawable's pixel size (SDL_GetCurrentRenderOutputSize, so HiDPI-aware) with the mode's
    // filter. The caller calls SDL_RenderPresent.
    bool draw();
    // Where the last draw() put the image.
    const Placement &placement() const { return placement_; }
    // The controller focus's highlight (#69, ADR 0051) over the drawn image: a frame around
    // the screen-point rect (x, y, w, h), placed by focus.h's focus_highlight. The window only:
    // call it after the read-back checks and captures, before SDL_RenderPresent.
    void draw_highlight(float x, float y, float w, float h);

    // Reads the drawn output back (before SDL_RenderPresent) and checks that it shows `rgba`
    // (the uploaded image, RGB only: the window has no alpha) and the bars:
    //   - every output pixel outside the placement has the border colour;
    //   - every output pixel inside it shows the image: nearest filtering, exactly one of the
    //     texels the pixel covers (one, except where a texel edge crosses the pixel at a
    //     fractional scale); linear and pixel-art filtering, a colour within the range of
    //     those texels and their neighbours per channel (+-2), which any bilinear blend is.
    // Returns the number of mismatched pixels (-1 if nothing could be read); `report` gets a
    // summary.
    long check_readback(const std::uint8_t *rgba, std::uint32_t stride, std::string *report);
    // Reads the drawn output back (before SDL_RenderPresent) into a PNG file.
    bool save_readback(const std::string &path);

  private:
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;
    ScaleMode mode_ = ScaleMode::kInteger;
    Color border_;
    Placement placement_;
    SDL_GPURenderState *render_state_ = nullptr;
};

// The window's pixel density per axis: its pixel size over its size (SDL_GetWindowSizeInPixels,
// SDL_GetWindowSize); 1 if either is unknown.
void window_density(SDL_Window *window, float *density_x, float *density_y);

// A window point (an SDL mouse event's x, y) to the game's screen points through `placement`
// (Presenter::placement()) and the window's density: scaling.h's window_to_screen. What M7's
// mouse input (#65) hands to mt_uikit_touch when `inside`.
ScreenPoint window_point_to_screen(SDL_Window *window, const Placement &placement, float x,
                                   float y);
// The forward map: a screen point to window points.
void screen_to_window_point(SDL_Window *window, const Placement &placement, float sx, float sy,
                            float *x, float *y);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PRESENT_H
