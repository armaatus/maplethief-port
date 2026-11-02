// The colour-blind filter (#158, ADR 0093); see color_filter.h.
#include "color_filter.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "maplethief/gfx_gpu.h"

namespace maplethief::frontend {

namespace {

constexpr ColorMatrix kIdentity = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
constexpr ColorMatrix kZero = {};

// Machado, Oliveira & Fernandes 2009, Table 1, severity 1.0 (linear RGB).
constexpr ColorMatrix kProtanopia = {{{0.152286f, 1.052583f, -0.204868f},
                                      {0.114503f, 0.786281f, 0.099216f},
                                      {-0.003882f, -0.048116f, 1.051998f}}};
constexpr ColorMatrix kDeuteranopia = {{{0.367322f, 0.860646f, -0.227968f},
                                        {0.280085f, 0.672501f, 0.047413f},
                                        {-0.011820f, 0.042940f, 0.968881f}}};
constexpr ColorMatrix kTritanopia = {{{1.255528f, -0.076749f, -0.178779f},
                                      {-0.078411f, 0.930809f, 0.147602f},
                                      {0.004733f, 0.691367f, 0.303900f}}};

// Fidaner, Lin & Ozguven 2005: the error a red-green dichromat misses (mostly in red) shifted
// into green and blue; for tritanopia the blue error into red and green.
constexpr ColorMatrix kShiftRedGreen = {{{0, 0, 0}, {0.7f, 1, 0}, {0.7f, 0, 1}}};
constexpr ColorMatrix kShiftBlue = {{{1, 0, 0.7f}, {0, 1, 0.7f}, {0, 0, 0}}};

ColorMatrix multiply(const ColorMatrix &a, const ColorMatrix &b) {
    ColorMatrix out{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float s = 0;
            for (int k = 0; k < 3; k++) {
                s += a[i][k] * b[k][j];
            }
            out[i][j] = s;
        }
    }
    return out;
}

} // namespace

ColorMatrix colorblind_simulation(ColorblindFilter filter) {
    switch (filter) {
    case ColorblindFilter::kProtanopia:
        return kProtanopia;
    case ColorblindFilter::kDeuteranopia:
        return kDeuteranopia;
    case ColorblindFilter::kTritanopia:
        return kTritanopia;
    case ColorblindFilter::kOff:
        break;
    }
    return kIdentity;
}

ColorMatrix colorblind_error_shift(ColorblindFilter filter) {
    switch (filter) {
    case ColorblindFilter::kProtanopia:
    case ColorblindFilter::kDeuteranopia:
        return kShiftRedGreen;
    case ColorblindFilter::kTritanopia:
        return kShiftBlue;
    case ColorblindFilter::kOff:
        break;
    }
    return kZero;
}

ColorMatrix colorblind_matrix(ColorblindFilter filter) {
    const ColorMatrix s = colorblind_simulation(filter);
    ColorMatrix lost{}; // I - S
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            lost[i][j] = kIdentity[i][j] - s[i][j];
        }
    }
    ColorMatrix d = multiply(colorblind_error_shift(filter), lost);
    for (int i = 0; i < 3; i++) {
        d[i][i] += 1;
    }
    return d;
}

float srgb_to_linear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

void colorblind_reference(ColorblindFilter filter, const std::uint8_t in[3],
                          std::uint8_t out[3]) {
    const ColorMatrix d = colorblind_matrix(filter);
    float lin[3];
    for (int c = 0; c < 3; c++) {
        lin[c] = srgb_to_linear(static_cast<float>(in[c]) / 255.0f);
    }
    for (int c = 0; c < 3; c++) {
        const float v = d[c][0] * lin[0] + d[c][1] * lin[1] + d[c][2] * lin[2];
        const float e = linear_to_srgb(std::clamp(v, 0.0f, 1.0f));
        out[c] = static_cast<std::uint8_t>(std::lround(e * 255.0f));
    }
}

// --- the CPU way --------------------------------------------------------------------------------

void ColorTransform::set(ColorblindFilter filter) {
    filter_ = filter;
    const ColorMatrix d = colorblind_matrix(filter);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            coeff_[i * 3 + j] = static_cast<std::int32_t>(std::lround(d[i][j] * 4096.0f));
        }
    }
    if (decode_[255] == 0) {
        for (int v = 0; v < 256; v++) {
            decode_[v] = static_cast<std::uint16_t>(
                std::lround(srgb_to_linear(static_cast<float>(v) / 255.0f) * 65535.0f));
        }
        for (int i = 0; i < 4096; i++) {
            // The step's centre (16 linear units of 65,535 a step).
            const float lin = (static_cast<float>(i) * 16.0f + 7.5f) / 65535.0f;
            encode_[i] = static_cast<std::uint8_t>(
                std::lround(linear_to_srgb(std::min(lin, 1.0f)) * 255.0f));
        }
    }
}

void ColorTransform::apply(const std::uint8_t *src, std::uint32_t src_stride, std::uint8_t *dst,
                           std::uint32_t dst_stride, std::uint32_t width,
                           std::uint32_t height) const {
    if (filter_ == ColorblindFilter::kOff) {
        for (std::uint32_t y = 0; y < height; y++) {
            if (src != dst) {
                std::memmove(dst + std::size_t{y} * dst_stride, src + std::size_t{y} * src_stride,
                             std::size_t{width} * 4);
            }
        }
        return;
    }
    const std::int32_t *k = coeff_;
    const auto channel = [this](std::int32_t sum) {
        // sum: linear in 1/65535 times 4096. Rounded to 1/65535, clamped, in 4,096 steps.
        const std::int32_t v = std::clamp((sum + 2048) >> 12, 0, 65535);
        return encode_[v >> 4];
    };
    for (std::uint32_t y = 0; y < height; y++) {
        const std::uint8_t *s = src + std::size_t{y} * src_stride;
        std::uint8_t *d = dst + std::size_t{y} * dst_stride;
        for (std::uint32_t x = 0; x < width; x++, s += 4, d += 4) {
            const std::int32_t r = decode_[s[0]];
            const std::int32_t g = decode_[s[1]];
            const std::int32_t b = decode_[s[2]];
            const std::uint8_t a = s[3];
            d[0] = channel(k[0] * r + k[1] * g + k[2] * b);
            d[1] = channel(k[3] * r + k[4] * g + k[5] * b);
            d[2] = channel(k[6] * r + k[7] * g + k[8] * b);
            d[3] = a;
        }
    }
}

// --- the GPU way --------------------------------------------------------------------------------

bool ColorblindPass::open(SDL_Renderer *renderer) {
    close();
    device_ = renderer != nullptr ? SDL_GetGPURendererDevice(renderer) : nullptr;
    if (device_ == nullptr) {
        return false; // not SDL's GPU renderer: the caller uses the CPU way
    }
    shader_ = mt_gfx_gpu_create_shader(device_, "present_colorblind.frag");
    if (shader_ == nullptr) {
        SDL_Log("present: no shader present_colorblind.frag: %s", SDL_GetError());
        close();
        return false;
    }
    SDL_GPURenderStateCreateInfo info{};
    info.fragment_shader = shader_;
    state_ = SDL_CreateGPURenderState(renderer, &info);
    if (state_ == nullptr) {
        SDL_Log("present: pass present_colorblind.frag: %s", SDL_GetError());
        close();
        return false;
    }
    return true;
}

void ColorblindPass::close() {
    if (state_ != nullptr) {
        SDL_DestroyGPURenderState(state_);
        state_ = nullptr;
    }
    if (shader_ != nullptr) {
        SDL_ReleaseGPUShader(device_, shader_);
        shader_ = nullptr;
    }
    device_ = nullptr;
}

bool ColorblindPass::set_uniforms(ColorblindFilter filter, std::uint32_t width,
                                  std::uint32_t height) {
    if (state_ == nullptr || width == 0 || height == 0) {
        return false;
    }
    const ColorMatrix d = colorblind_matrix(filter);
    ColorUniforms u{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            u.rows[i][j] = d[i][j];
        }
    }
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    u.source[0] = w;
    u.source[1] = h;
    u.source[2] = 1.0f / w;
    u.source[3] = 1.0f / h;
    if (!SDL_SetGPURenderStateFragmentUniforms(state_, 0, &u, sizeof u)) {
        SDL_Log("present: colour-blind filter uniforms: %s", SDL_GetError());
        return false;
    }
    return true;
}

} // namespace maplethief::frontend
