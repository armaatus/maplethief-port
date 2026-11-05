// The colour-blind filter (#158, ADR 0093) and the picture adjustments (#187); see
// color_filter.h.
#include "color_filter.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "gfx_backend.h"

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

void colorblind_reference(ColorblindFilter filter, const std::uint8_t in[3], std::uint8_t out[3]) {
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

// --- the picture adjustments (#187) -------------------------------------------------------------

namespace {

// Rec. 709's luminance weights (linear RGB): what saturation keeps.
constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};

float encode_gamma(float e, std::uint32_t gamma) {
    return gamma == kGammaDefault ? e : std::pow(e, 100.0f / static_cast<float>(gamma));
}

} // namespace

ColorGrade color_grade(const Settings &s) {
    return ColorGrade{s.colorblind_filter, s.brightness, s.contrast, s.saturation, s.gamma};
}

std::string color_grade_text(const ColorGrade &g) {
    if (g.is_identity()) {
        return "off";
    }
    std::string out;
    const auto add = [&out](const std::string &part) { out += (out.empty() ? "" : ", ") + part; };
    if (g.filter != ColorblindFilter::kOff) {
        add(colorblind_filter_name(g.filter));
    }
    const std::pair<const char *, std::uint32_t> percents[] = {
        {"brightness", g.brightness}, {"contrast", g.contrast}, {"saturation", g.saturation}};
    for (const auto &[name, v] : percents) {
        if (v != kPictureDefault) {
            add(std::string(name) + " " + std::to_string(v) + " %");
        }
    }
    if (g.gamma != kGammaDefault) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "gamma %u.%02u", g.gamma / 100, g.gamma % 100);
        add(buf);
    }
    return out;
}

ColorAffine picture_adjustment(std::uint32_t brightness, std::uint32_t contrast,
                               std::uint32_t saturation) {
    const float b = static_cast<float>(brightness) / 100.0f;
    const float c = static_cast<float>(contrast) / 100.0f;
    const float s = static_cast<float>(saturation) / 100.0f;
    const float pivot = srgb_to_linear(0.5f);
    ColorAffine a{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            const float sat = (i == j ? s : 0.0f) + (1.0f - s) * kLuma[j];
            a.m[i][j] = c * b * sat;
        }
        a.offset[i] = pivot * (1.0f - c);
    }
    return a;
}

ColorAffine color_grade_affine(const ColorGrade &g) {
    ColorAffine a = picture_adjustment(g.brightness, g.contrast, g.saturation);
    a.m = multiply(a.m, colorblind_matrix(g.filter));
    return a;
}

void color_grade_reference(const ColorGrade &g, const std::uint8_t in[3], std::uint8_t out[3]) {
    const ColorAffine a = color_grade_affine(g);
    float lin[3];
    for (int c = 0; c < 3; c++) {
        lin[c] = srgb_to_linear(static_cast<float>(in[c]) / 255.0f);
    }
    for (int c = 0; c < 3; c++) {
        const float v = a.m[c][0] * lin[0] + a.m[c][1] * lin[1] + a.m[c][2] * lin[2] + a.offset[c];
        const float e = encode_gamma(linear_to_srgb(std::clamp(v, 0.0f, 1.0f)), g.gamma);
        out[c] = static_cast<std::uint8_t>(std::lround(e * 255.0f));
    }
}

// --- the CPU way --------------------------------------------------------------------------------

void ColorTransform::set(const ColorGrade &grade) {
    grade_ = grade;
    const ColorAffine a = color_grade_affine(grade);
    // The filter alone: #158's 12 fraction bits (its bytes as before); with the adjustments 16,
    // as gamma's curve and a steep map make the darks sensitive to the coefficients' rounding.
    bits_ = grade.filter_only() ? 12 : 16;
    const double one = static_cast<double>(std::int64_t{1} << bits_);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            coeff_[i * 3 + j] = std::llround(static_cast<double>(a.m[i][j]) * one);
        }
        offset_[i] = std::llround(static_cast<double>(a.offset[i]) * 65535.0 * one);
    }
    if (decode_[255] == 0) {
        for (int v = 0; v < 256; v++) {
            decode_[v] = static_cast<std::uint16_t>(
                std::lround(srgb_to_linear(static_cast<float>(v) / 255.0f) * 65535.0f));
        }
    }
    if (encoded_gamma_ != grade.gamma) {
        encoded_gamma_ = grade.gamma;
        if (grade.gamma == kGammaDefault) {
            // As before #187: 4,096 steps, each at its centre (16 linear units of 65,535 a step).
            shift_ = 4;
            encode_.assign(4096, 0);
            for (int i = 0; i < 4096; i++) {
                const float lin = (static_cast<float>(i) * 16.0f + 7.5f) / 65535.0f;
                encode_[i] = static_cast<std::uint8_t>(
                    std::lround(linear_to_srgb(std::min(lin, 1.0f)) * 255.0f));
            }
        } else {
            // Every linear value: x^(1 / gamma) is steep near black.
            shift_ = 0;
            encode_.assign(65536, 0);
            for (int i = 0; i < 65536; i++) {
                const float lin = static_cast<float>(i) / 65535.0f;
                encode_[i] = static_cast<std::uint8_t>(
                    std::lround(encode_gamma(linear_to_srgb(lin), grade.gamma) * 255.0f));
            }
        }
    }
}

void ColorTransform::apply(const std::uint8_t *src, std::uint32_t src_stride, std::uint8_t *dst,
                           std::uint32_t dst_stride, std::uint32_t width,
                           std::uint32_t height) const {
    if (grade_.is_identity()) {
        for (std::uint32_t y = 0; y < height; y++) {
            if (src != dst) {
                std::memmove(dst + std::size_t{y} * dst_stride, src + std::size_t{y} * src_stride,
                             std::size_t{width} * 4);
            }
        }
        return;
    }
    const std::int64_t *k = coeff_;
    const std::int64_t *o = offset_;
    const std::uint8_t *encode = encode_.data();
    const int shift = shift_;
    const int bits = bits_;
    const std::int64_t half = std::int64_t{1} << (bits - 1);
    const auto channel = [encode, shift, bits, half](std::int64_t sum) {
        // sum: linear in 1/65535 times 2^bits (64 bits wide: brightness and contrast may take a
        // row's weights past 32 bits' reach). Rounded to 1/65535, clamped, encoded.
        const auto v =
            static_cast<std::int32_t>(std::clamp<std::int64_t>((sum + half) >> bits, 0, 65535));
        return encode[v >> shift];
    };
    for (std::uint32_t y = 0; y < height; y++) {
        const std::uint8_t *s = src + std::size_t{y} * src_stride;
        std::uint8_t *d = dst + std::size_t{y} * dst_stride;
        for (std::uint32_t x = 0; x < width; x++, s += 4, d += 4) {
            const std::int64_t r = decode_[s[0]];
            const std::int64_t g = decode_[s[1]];
            const std::int64_t b = decode_[s[2]];
            const std::uint8_t a = s[3];
            d[0] = channel(k[0] * r + k[1] * g + k[2] * b + o[0]);
            d[1] = channel(k[3] * r + k[4] * g + k[5] * b + o[1]);
            d[2] = channel(k[6] * r + k[7] * g + k[8] * b + o[2]);
            d[3] = a;
        }
    }
}

// --- the GPU way --------------------------------------------------------------------------------

bool ColorblindPass::open(SDL_Renderer *renderer) {
    close();
    if (renderer == nullptr || SDL_GetGPURendererDevice(renderer) == nullptr) {
        return false; // not SDL's GPU renderer: the caller uses the CPU way
    }
    // The renderer's own (present_shader): never released while it lives, so no later shader
    // can take its address in SDL's pipeline cache.
    SDL_GPUShader *shader = present_shader(renderer, "present_colorblind.frag");
    if (shader == nullptr) {
        SDL_Log("present: no shader present_colorblind.frag: %s", SDL_GetError());
        return false;
    }
    SDL_GPURenderStateCreateInfo info{};
    info.fragment_shader = shader;
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
}

bool ColorblindPass::set_uniforms(const ColorGrade &grade, std::uint32_t width,
                                  std::uint32_t height) {
    if (state_ == nullptr || width == 0 || height == 0) {
        return false;
    }
    const ColorAffine a = color_grade_affine(grade);
    ColorUniforms u{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            u.rows[i][j] = a.m[i][j];
        }
        u.rows[i][3] = a.offset[i];
    }
    u.encode[0] = 100.0f / static_cast<float>(grade.gamma);
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
