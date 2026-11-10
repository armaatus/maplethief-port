// Presentation (#58, ADR 0042): from the game's presented GL renderbuffer to the SDL window.
//
//   -[EAGLContext presentRenderbuffer:]  ->  Screen::take_gl_frame   (the GL present hook)
//       the renderbuffer's mt_gfx target, copied as the opaque CAEAGLLayer shows it
//   the UIKit turn hook                  ->  Screen::compose
//       mt_uikit_composite(FULL) at 480x320 x render scale, the GL frame in the GL view's place
//                                        ->  Presenter::upload, Presenter::draw
//       an SDL streaming texture (R, G, B, A bytes, blend none) drawn into place() of the
//       window's drawable (scaling.h: the scale mode, its filter), the border around it
//       (border.h, #157: the border colour, the frame blurred, or the player's art)
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
// With mt_gfx on the GPU, gpu_screen.h (#130, ADR 0068) takes Screen's place: the frame stays on
// the GPU and is composed with UIKit's overlay into the Presenter's texture (a render target).
//
// Scaling (#59, ADR 0043): scaling.h's place() is the one place that decides where the screen
// image lands in the drawable and how it is filtered; window_point_to_screen() is its inverse
// for mouse and touch input. The placement honours the window's safe area (#73, ADR 0055):
// draw() reads SDL_GetWindowSafeArea every frame (a rotation or SDL_EVENT_WINDOW_SAFE_AREA_CHANGED
// takes effect at the next frame), unless a test forces the insets (--safe-area).
#ifndef MAPLETHIEF_PLATFORM_PRESENT_H
#define MAPLETHIEF_PLATFORM_PRESENT_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "border.h"
#include "color_filter.h"
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
    // A new render scale (render_scale = auto, #152): the screen is composited at 480x320 times
    // it from the next compose on. The GL frame taken stays (the compositor scales any size).
    void set_scale(std::uint32_t render_scale);

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

    // The present hook's half: copies the frame's target as the opaque CAEAGLLayer shows it
    // (RGB as drawn, alpha 255). Returns false if the frame has no target.
    bool take_gl_frame(const mt_gles_frame &frame);
    // The same for a frame's pixels that the caller has (GPU presentation's read-back of the
    // frame it shows, gpu_screen.h): width x height RGBA8, top row first.
    void set_gl_frame(std::uint32_t index, const std::uint8_t *rgba, std::uint32_t width,
                      std::uint32_t height, std::uint32_t view);
    // The index of the last GL frame taken (mt_gles_frame.index), 0 if none.
    std::uint32_t gl_frame() const { return gl_index_; }
    // The index of the first GL frame taken that is not black (see is_black), 0 if none yet.
    std::uint32_t first_lit_gl_frame() const { return first_lit_gl_; }
    std::uint32_t gl_width() const { return gl_width_; }
    std::uint32_t gl_height() const { return gl_height_; }
    // The engine's view width (maplethief/widescreen.h) when the GL frame was taken: 480 unless
    // widescreen is on. compose() shows the frame's centre 480 points of it (gl_screen_columns).
    std::uint32_t gl_view() const { return gl_view_; }
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
    std::uint32_t gl_view_ = 480;
    std::vector<std::uint8_t> gl_, screen_;
};

// Widescreen (#86, ADR 0090): which columns of a GL frame gl_width x gl_height texels the
// 480 x 320-point screen image shows, when the engine drew its first `view` points (480 ... 568)
// of a layer wider than 480 points: the centre 480 points of them. A frame 480 points wide (or
// one whose height is not a whole number of 320-point rows) is shown whole.
struct GlColumns {
    std::uint32_t x = 0, width = 0;
};
GlColumns gl_screen_columns(std::uint32_t gl_width, std::uint32_t gl_height, std::uint32_t view);

// Widescreen (#86): the centred `points`-wide part of a rectangle that shows `wide_points`
// points across, its edges rounded to whole pixels (the whole rectangle when points >=
// wide_points).
Rect wide_part(const Rect &wide, std::uint32_t wide_points, std::uint32_t points);
// Widescreen's wings (ADR 0103): the parts of the wide picture's rectangle left and right of the
// screen image's (each empty, w 0, where there is none), whole pixels when both are: what the
// veil darkens while a port overlay (the settings menu, the remap overlay) is open over the
// screen image, so that the whole picture is dimmed behind it, not only its middle 480 points.
struct WingRects {
    Rect left, right;
};
WingRects wing_rects(const Rect &wide, const Rect &image);

// Whether an RGBA8 image is black: at most 1% of its pixels have r + g + b above 24.
bool is_black(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
              std::uint32_t stride);

// How Presenter::check_readback judges the image that a present pass drew.
enum class PassCheck {
    kScaleMode, // as the scale mode's filter would draw it (the identity pass)
    kBlend,     // a blend of the texels a pixel covers and their neighbours, +-2 (edge smoothing)
    kBarsOnly,  // only the bars: the pass changes the colours (scanlines, an LCD grid)
};

// The present stage's pass for the next draws (#120, ADR 0060; #88's filters, ADR 0066):
// gfx_backend.h's PresentPass fills it.
struct PresentStage {
    SDL_GPURenderState *state = nullptr; // nullptr: SDL's own texture shader
    bool sizes = false; // push PresentSizes to fragment uniform slot 0 before each draw
    PassCheck check = PassCheck::kScaleMode;
};

// The uniform block of a pass with `sizes` (the shaders' `cbuffer Sizes`, set 3, b0).
struct PresentSizes {
    float source[4]; // the screen image in texels: w, h, 1/w, 1/h
    float output[4]; // the image's rectangle (Placement::rect) in output pixels: w, h, 1/w, 1/h
};

// The SDL half: a streaming texture of the screen image, drawn into the window.
class Presenter {
  public:
    Presenter() = default;
    Presenter(const Presenter &) = delete;
    Presenter &operator=(const Presenter &) = delete;
    ~Presenter() { stop(); }

    // Creates the image_w x image_h texture (R, G, B, A bytes: SDL_PIXELFORMAT_ABGR8888, blend
    // none). Returns false (logged) if it cannot. `target`: a render target instead of a
    // streaming texture, which GPU presentation (gpu_screen.h, #130) composes on the GPU;
    // upload() is then not used.
    bool start(SDL_Renderer *renderer, std::uint32_t image_w, std::uint32_t image_h,
               bool target = false);
    // The screen image's texture (nullptr before start).
    SDL_Texture *texture() const { return texture_; }
    // Frees the texture (before the renderer goes).
    void stop();

    // The scale mode and the bars' colour for the next draw() (default: integer, black).
    void set_scaling(ScaleMode mode, Color border);
    // What fills the bars from the next draw() (#157, border.h; default: color, the border
    // colour). `image`: border_image's file, read at the first draw that needs it.
    void set_border(BorderMode mode, const std::string &image = {});
    const Border &border() const { return border_; }
    // The colour step (color_filter.h; default: the identity, no step), the present stage's
    // first: the colour-blind filter (#158) and the picture adjustments (#187). On SDL's GPU
    // renderer it applies from the next draw(); on any other renderer (or without the shader) on
    // the CPU from the next upload().
    void set_grade(const ColorGrade &grade) { grade_ = grade; }
    const ColorGrade &grade() const { return grade_; }
    // The grade's colour-blind filter alone (the adjustments kept).
    void set_colorblind(ColorblindFilter filter) { grade_.filter = filter; }
    ColorblindFilter colorblind() const { return grade_.filter; }
    // How the last draw() applied it.
    enum class ColorPath { kNone, kCpu, kGpu };
    ColorPath color_path() const { return color_path_; }
    ColorblindFilter color_drawn() const { return color_drawn_.filter; }
    const ColorGrade &grade_drawn() const { return color_drawn_; }
    // The present stage's pass (#120, ADR 0060; gfx_backend.h's PresentPass): a fragment shader
    // that SDL's GPU renderer draws the image through, in place of its own; nullptr (the
    // default): SDL's. Only with SDL's GPU renderer. The pass replaces the scale mode's shader,
    // so it does its own filtering (the sampler is still the scale mode's).
    void set_render_state(SDL_GPURenderState *state) { stage_ = PresentStage{state}; }
    // The same with the pass's uniforms and read-back check (a filter, ADR 0066).
    void set_stage(const PresentStage &stage) { stage_ = stage; }
    const PresentStage &stage() const { return stage_; }
    ScaleMode scale_mode() const { return mode_; }
    Color border_color() const { return color_; }
    // Insets (window points) to use instead of the window's safe area (--safe-area, a test aid
    // for the desktop); std::nullopt: the window's (window_insets).
    void set_forced_insets(std::optional<Insets> insets) { forced_ = insets; }
    // The insets the last draw() placed the image with.
    const Insets &insets() const { return insets_; }

    // Uploads an image of the texture's size (RGBA8, top row first).
    bool upload(const std::uint8_t *rgba, std::uint32_t stride);
    // The present stage, each step over the last (border.h): with widescreen's wings, composes
    // the wide picture (#170); grades it or the screen image (the colour-blind filter); clears
    // the drawable to the border colour, draws the border's step (the blur or the art; nothing
    // for color), then the image at place() of the drawable's pixel size
    // (SDL_GetCurrentRenderOutputSize, so HiDPI-aware), inside the safe area (the forced insets,
    // else the window's), with the mode's filter or the stage's pass. The caller draws the
    // window's overlays and calls SDL_RenderPresent.
    bool draw();
    // Where the last draw() put the image.
    const Placement &placement() const { return placement_; }

    // Widescreen (#86, ADR 0090): the screen image is the centre 480 points of a picture `wide`
    // points wide (0 or 480: off, the default). draw() places the wide picture (integer mode
    // scales its points) and the screen image in its centre; placement() stays the screen
    // image's, so input, the focus highlight and check_readback's image are as before.
    void set_wide(std::uint32_t wide) { wide_ = wide; }
    // The veil (ADR 0103): with `alpha` > 0, draw() darkens widescreen's wings (wing_rects) with
    // black at that alpha over everything it draws there, as the port's overlays darken the
    // screen image (their backdrop, black at 170). 0 (the default): no veil.
    void set_veil(std::uint8_t alpha) { veil_ = alpha; }
    std::uint32_t wide() const { return wide_; }
    // Where the last draw() put the wide picture (the placement's rectangle when off).
    const Rect &wide_rect() const { return wide_rect_; }
    // The placement with the wide picture's rectangle (placement() when off): what the port's
    // window furniture beside the picture (the settings menu's gear, #89) keeps clear of, since
    // the wings are part of the picture.
    Placement wide_placement() const {
        Placement p = placement_;
        p.rect = wide_rect_;
        return p;
    }
    // The GL frame beside the screen image (the "wings"): `gl`, a texture of the GL layer at
    // `scale` texels a point whose first `view` points the engine drew, goes into the wide
    // picture's centre `view` points under the screen image's middle 480 (draw() composes the
    // wide picture, #170). nullptr or view <= 480: no wings (the bars' colour beside the screen
    // image). The texture's colours are taken as they are: the colour-blind filter's CPU way
    // can't grade it (the GPU way grades the composed picture).
    void set_wings(SDL_Texture *gl, std::uint32_t view, std::uint32_t scale);
    // The read-back path's wings: the GL frame's pixels (gl_w x gl_h RGBA8, top row first)
    // uploaded into the Presenter's own texture, set as above with scale gl_h / 320 (with the
    // colour-blind filter's CPU way, the wing columns graded first). view <= 480 clears the
    // wings without an upload. check_readback compares the wings with `rgba`: the caller keeps
    // it until then.
    bool upload_wings(const std::uint8_t *rgba, std::uint32_t gl_w, std::uint32_t gl_h,
                      std::uint32_t view);
    // Widescreen with the touch key pad (#168): the game pad's keys that the anchored layout
    // moved beside the screen image, composited into two strips (keypad_wings_composite: w x h
    // premultiplied RGBA8, the left wing's strip then the right's, each w / 2 texels wide for
    // the wide picture's (wide - 480) / 2 points). draw() puts them over the wings (into the
    // composed wide picture, #170; with the colour-blind filter's CPU way graded first). nullptr
    // clears them (nothing drawn: the wings as before). check_readback compares them with
    // `rgba`: the caller keeps it until then.
    bool upload_pad_wings(const std::uint8_t *rgba, std::uint32_t w, std::uint32_t h);
    bool pad_wings() const { return pad_wings_on_; }
    // Widescreen's wide picture (#170): whether the last draw() composed the wings, the screen
    // image and the pad's strips into one picture (a render target of view x 320 points at the
    // screen texture's texels a point) and drew that through the colour-blind filter, the
    // border's blur and the scale mode's filter or the pass, as one image; where it went (the
    // wings' rectangle, wide_part(wide_rect(), wide(), view)); its size in texels.
    bool picture_drawn() const { return picture_drawn_; }
    const Rect &picture_rect() const { return picture_rect_; }
    std::uint32_t picture_width() const { return picture_w_; }
    // The controller focus's highlight (#69, ADR 0051) over the drawn image: a frame around
    // the screen-point rect (x, y, w, h), placed by focus.h's focus_highlight. The window only:
    // call it after the read-back checks and captures, before SDL_RenderPresent.
    void draw_highlight(float x, float y, float w, float h);

    // Reads the drawn output back (before SDL_RenderPresent) and checks that it shows `rgba`
    // (the uploaded image, RGB only: the window has no alpha) and the bars. When the last draw
    // composed widescreen's wide picture (picture_drawn, #170), the picture is read back too:
    // its screen columns must be `rgba` exactly (graded, under the colour-blind filter's CPU
    // way), its wing columns the uploaded GL frame with the pad's strips over them (+-2; not
    // checked for set_wings' GPU texture), and the output is checked against the whole picture
    // (graded on the CPU, +-2, under the GPU way) by the rules below over picture_rect():
    //   - every output pixel outside the placement has the border colour (only when the last
    //     draw drew no blur or art: those bars are not checked, and the report says so);
    //   - every output pixel inside it shows the image: nearest filtering, exactly one of the
    //     texels the pixel covers (one, except where a texel edge crosses the pixel at a
    //     fractional scale); linear and pixel-art filtering, a colour within the range of
    //     those texels and their neighbours per channel (+-2), which any bilinear blend is.
    // Under a present pass, the image is judged by the stage's PassCheck instead: kBlend as
    // linear filtering, kBarsOnly not at all (only the bars).
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
    Color color_; // border_color
    std::optional<Insets> forced_;
    Insets insets_;
    Placement placement_;
    PresentStage stage_;
    Border border_;
    // Widescreen (#86): the wide picture's points, its rectangle, the wings.
    std::uint32_t wide_ = 0;
    Rect wide_rect_;
    std::uint8_t veil_ = 0;
    SDL_Texture *wings_ = nullptr;     // set_wings' (not owned) or wings_own_
    SDL_Texture *wings_own_ = nullptr; // upload_wings' streaming texture
    std::uint32_t wings_view_ = 0, wings_scale_ = 1;
    // Widescreen's wide picture (#170): composed into picture_ by compose_picture(view).
    bool compose_picture(std::uint32_t view);
    void free_picture();
    SDL_Texture *picture_ = nullptr;        // the composed wide picture (a render target)
    SDL_Texture *graded_picture_ = nullptr; // its grade (the colour-blind filter's GPU way)
    std::uint32_t picture_w_ = 0, picture_h_ = 0;
    std::uint32_t picture_view_ = 0, picture_mid_ = 0; // its points; the image's first column
    bool picture_drawn_ = false, picture_failed_ = false;
    Rect picture_rect_;
    const std::uint8_t *wings_src_ = nullptr; // upload_wings' pixels (the caller's), for checks
    std::uint32_t wings_src_w_ = 0;
    ColorGrade wings_graded_;               // the CPU way's grade of the wings
    std::vector<std::uint8_t> wings_cols_;  // the graded wing columns
    const std::uint8_t *pad_src_ = nullptr; // upload_pad_wings' pixels (the caller's)
    ColorGrade pad_graded_;
    std::vector<std::uint8_t> pad_px_; // the graded strips
    // The colour step (#158, #187).
    bool color_on_gpu(); // the pass is open on this renderer (opened at the first need)
    // `from` (w x h) through the pass into `*into` (made or remade at that size).
    bool grade(SDL_Texture *from, std::uint32_t w, std::uint32_t h, SDL_Texture **into);
    ColorGrade grade_;
    ColorblindPass color_pass_;
    bool color_pass_failed_ = false;
    SDL_Texture *graded_ = nullptr; // the GPU way's target, the image's size
    ColorTransform color_cpu_;
    std::vector<std::uint8_t> graded_px_; // the CPU way's image
    ColorGrade uploaded_;                 // what the texture's pixels have
    ColorPath color_path_ = ColorPath::kNone;
    ColorGrade color_drawn_;
    // Widescreen with the key pad (#168): upload_pad_wings' texture and its size.
    SDL_Texture *pad_wings_ = nullptr;
    bool pad_wings_on_ = false;
    std::uint32_t pad_w_ = 0, pad_h_ = 0;
};

// The window's safe-area insets in window points: its size less SDL_GetWindowSafeArea (zero
// on the desktop, and if either is unknown).
Insets window_insets(SDL_Window *window);

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
