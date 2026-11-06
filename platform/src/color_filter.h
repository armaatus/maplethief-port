// The colour-blind filter (#158, ADR 0093): colorblind_filter = off | protanopia | deuteranopia |
// tritanopia, the present stage's first step (present.h, ADR 0085's list). With the picture
// adjustments (#187, ADR 0093's addendum) it is one colour step, a ColorGrade.
//
// Daltonisation, not simulation. A simulation shows a viewer with normal colour vision what a
// dichromat sees; it takes information away and helps no player. Daltonisation moves the
// difference a dichromat cannot see into colours they can: per mode, in linear RGB,
//
//   S  the dichromat's view: Machado, Oliveira & Fernandes, "A Physiologically-based Model for
//      Simulation of Color Vision Deficiency", IEEE TVCG 15(6), 2009, severity 1.0 (their
//      published matrices)
//   E  the error shift of Fidaner, Lin & Ozguven, "Analysis of Color Blindness" (Stanford
//      EE368, 2005): for protanopia and deuteranopia the red error goes into green and blue,
//      for tritanopia the blue error into red and green
//   D = I + E (I - S): out = D in, clamped to [0, 1]
//
// The rows of S sum to 1, so greys (and black and white) are unchanged, as are colours the
// dichromat already sees as they are.
//
// The picture adjustments (#187): brightness, contrast and saturation in percent (50 to 150) and
// gamma (0.80 to 1.40), after D, in linear RGB, one affine map A x + o:
//
//   saturation  Sat = s I + (1 - s) [w; w; w], w Rec. 709's luminance weights (greys kept)
//   brightness  times b (the light, as an exposure)
//   contrast    about mid-grey (sRGB 0.5, linear p = 0.2140): c (x - p) + p
//   A = c b Sat D, o = p (1 - c)
//   gamma       on the encoded value: e^(1 / gamma) (above 1 the mid-tones are lighter)
//
// The defaults (100, 100, 100, 1.00) with the filter off are the identity: no step at all.
//
// Two ways to run it, the same matrix:
//   - on SDL's GPU renderer, ColorblindPass: present_colorblind.frag draws the screen texture
//     into an image-sized render target (sRGB decoded, D, encoded, in floats);
//   - on any other renderer, ColorTransform: on the CPU over the image before it is uploaded
//     (integer tables: the decoded value in 16 bits, D in 12, the encoding from 4,096 steps;
//     within 1 of the float result per channel).
#ifndef MAPLETHIEF_PLATFORM_COLOR_FILTER_H
#define MAPLETHIEF_PLATFORM_COLOR_FILTER_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "settings.h"

struct SDL_GPURenderState;
struct SDL_Renderer;

namespace maplethief::frontend {

// A 3x3 matrix on linear RGB column vectors, rows first.
using ColorMatrix = std::array<std::array<float, 3>, 3>;

// S: Machado et al.'s simulation at severity 1.0 (the identity for kOff).
ColorMatrix colorblind_simulation(ColorblindFilter filter);
// E: Fidaner et al.'s error shift (zero for kOff).
ColorMatrix colorblind_error_shift(ColorblindFilter filter);
// D = I + E (I - S): what the filter applies (the identity for kOff).
ColorMatrix colorblind_matrix(ColorblindFilter filter);

// The sRGB transfer functions (IEC 61966-2-1), on [0, 1].
float srgb_to_linear(float v);
float linear_to_srgb(float v);

// The filter on one RGB8 pixel in floats: the reference both ways are checked against.
void colorblind_reference(ColorblindFilter filter, const std::uint8_t in[3], std::uint8_t out[3]);

// The present stage's colour step (#158, #187): the colour-blind filter and the picture
// adjustments (settings.h's brightness, contrast, saturation, gamma).
struct ColorGrade {
    ColorblindFilter filter = ColorblindFilter::kOff;
    std::uint32_t brightness = kPictureDefault; // percent
    std::uint32_t contrast = kPictureDefault;   // percent
    std::uint32_t saturation = kPictureDefault; // percent
    std::uint32_t gamma = kGammaDefault;        // hundredths
    // The defaults with the filter off: nothing to do (no step, no target, no transform).
    bool is_identity() const { return *this == ColorGrade{}; }
    // Only the filter (the adjustments at their defaults).
    bool filter_only() const { return ColorGrade{filter} == *this; }
    bool operator==(const ColorGrade &) const = default;
};
// The grade the settings ask for.
ColorGrade color_grade(const Settings &s);
// "deuteranopia, brightness 110 %, gamma 1.20": what is not the default ("off" for the identity).
std::string color_grade_text(const ColorGrade &g);

// The step's affine map on linear RGB: m x + offset.
struct ColorAffine {
    ColorMatrix m;
    std::array<float, 3> offset;
};
// The adjustments' map alone (A, o above; D the identity).
ColorAffine picture_adjustment(std::uint32_t brightness, std::uint32_t contrast,
                               std::uint32_t saturation);
// The whole step's: A D, o.
ColorAffine color_grade_affine(const ColorGrade &g);
// The grade on one RGB8 pixel in floats (the reference).
void color_grade_reference(const ColorGrade &g, const std::uint8_t in[3], std::uint8_t out[3]);

// The CPU way: the grade over an RGBA8 image (alpha copied).
class ColorTransform {
  public:
    ColorTransform() { set(ColorGrade{}); }
    void set(ColorblindFilter filter) { set(ColorGrade{filter}); }
    void set(const ColorGrade &grade);
    ColorblindFilter filter() const { return grade_.filter; }
    const ColorGrade &grade() const { return grade_; }
    // `src` and `dst` are width x height RGBA8 with their strides; they may be the same image.
    // The identity copies.
    void apply(const std::uint8_t *src, std::uint32_t src_stride, std::uint8_t *dst,
               std::uint32_t dst_stride, std::uint32_t width, std::uint32_t height) const;

  private:
    ColorGrade grade_;
    int bits_ = 12;                  // the coefficients' fraction bits (16 with the adjustments)
    std::int64_t coeff_[9] = {};     // the matrix in 1/2^bits
    std::int64_t offset_[3] = {};    // the offset in 1/65535 times 2^bits
    std::uint16_t decode_[256] = {}; // sRGB byte -> linear in 1/65535
    // Linear -> sRGB byte (with gamma): 4,096 steps of 16 (`shift_` 4) at gamma 1, as before
    // #187; with another gamma every 1/65535 (shift 0), as its curve is steep in the darks.
    std::vector<std::uint8_t> encode_;
    int shift_ = 4;
    std::uint32_t encoded_gamma_ = 0; // the gamma encode_ is for (0: none yet)
};

// The uniform block of present_colorblind.frag (`cbuffer Colour`, set 3, b0).
struct ColorUniforms {
    float rows[3][4]; // the step's rows (x, y, z) and offset (w)
    float source[4];  // the image in texels: w, h, 1/w, 1/h
    float encode[4];  // 1 / gamma (x; y, z, w unused)
};

// The GPU way: present_colorblind.frag as an SDL_GPURenderState of SDL's GPU renderer. The
// Presenter draws the screen texture through it into a render target of the image's size.
class ColorblindPass {
  public:
    ColorblindPass() = default;
    ColorblindPass(const ColorblindPass &) = delete;
    ColorblindPass &operator=(const ColorblindPass &) = delete;
    ~ColorblindPass() { close(); }

    // Makes the pass on `renderer`. False (logged) when it is not SDL's GPU renderer or the
    // shader can't be made.
    bool open(SDL_Renderer *renderer);
    // Before the renderer goes.
    void close();
    SDL_GPURenderState *state() const { return state_; }
    // Pushes the grade's matrix, offset and gamma and the image's size for the next draw.
    bool set_uniforms(const ColorGrade &grade, std::uint32_t width, std::uint32_t height);
    bool set_uniforms(ColorblindFilter filter, std::uint32_t width, std::uint32_t height) {
        return set_uniforms(ColorGrade{filter}, width, height);
    }

  private:
    SDL_GPURenderState *state_ = nullptr; // its shader is the renderer's (present_shader)
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_COLOR_FILTER_H
