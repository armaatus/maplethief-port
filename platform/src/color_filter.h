// The colour-blind filter (#158, ADR 0093): colorblind_filter = off | protanopia | deuteranopia |
// tritanopia, the present stage's first step (present.h, ADR 0085's list).
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

#include "settings.h"

struct SDL_GPUDevice;
struct SDL_GPURenderState;
struct SDL_GPUShader;
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

// The CPU way: the filter over an RGBA8 image (alpha copied).
class ColorTransform {
  public:
    ColorTransform() { set(ColorblindFilter::kOff); }
    void set(ColorblindFilter filter);
    ColorblindFilter filter() const { return filter_; }
    // `src` and `dst` are width x height RGBA8 with their strides; they may be the same image.
    // kOff copies.
    void apply(const std::uint8_t *src, std::uint32_t src_stride, std::uint8_t *dst,
               std::uint32_t dst_stride, std::uint32_t width, std::uint32_t height) const;

  private:
    ColorblindFilter filter_ = ColorblindFilter::kOff;
    std::int32_t coeff_[9] = {};   // D in 1/4096
    std::uint16_t decode_[256] = {};  // sRGB byte -> linear in 1/65535
    std::uint8_t encode_[4096] = {}; // linear in 1/4096 steps -> sRGB byte
};

// The uniform block of present_colorblind.frag (`cbuffer Colour`, set 3, b0).
struct ColorUniforms {
    float rows[3][4]; // D's rows (x, y, z; w unused)
    float source[4];  // the image in texels: w, h, 1/w, 1/h
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
    // Pushes the filter's matrix and the image's size for the next draw.
    bool set_uniforms(ColorblindFilter filter, std::uint32_t width, std::uint32_t height);

  private:
    SDL_GPUDevice *device_ = nullptr;
    SDL_GPUShader *shader_ = nullptr;
    SDL_GPURenderState *state_ = nullptr;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_COLOR_FILTER_H
