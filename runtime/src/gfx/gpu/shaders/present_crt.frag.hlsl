// The present stage's CRT filter (#88, ADR 0066): scanlines and an aperture mask. It is a
// fragment shader for SDL's GPU renderer with present_copy.frag's interface (ADR 0060), plus one
// uniform block.
//
// - Each source row is a beam with a Gaussian profile across the row. Its width follows the
//   colour's brightness, so bright rows bloom and dark rows show their gaps. An output pixel
//   adds up the two nearest rows' beams.
// - Along the row, the beam blends neighbouring texels with a smoothstep weight: softer than
//   nearest, sharper than bilinear.
// - An aperture mask tints the output columns red, green and blue in turn, one output pixel
//   each.
// - The blending is done in linear light (gamma 2, so a square and a square root), and the
//   brightness lost to the gaps and the mask is given back.
// The look needs about 3 output pixels per source row, e.g. integer 3x and up.
//
// Texels are read with integer loads, clamped to the image, so the scale mode's sampler doesn't
// change the result.

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState u_sampler : register(s0, space2);

// Set by the Presenter before each draw (PresentSizes in platform/src/present.h).
[[vk::binding(0, 3)]] cbuffer Sizes : register(b0, space3) {
    float4 source_size; // the screen image in texels: w, h, 1/w, 1/h
    float4 output_size; // the image's rectangle in output pixels: w, h, 1/w, 1/h
};

struct Input {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_uv : TEXCOORD0;
};

static const float k_beam_dark = 0.24;   // the beam's sigma for black, in rows
static const float k_beam_bright = 0.42; // and for white
static const float k_mask_dim = 0.72;    // the two other channels in a mask column
static const float k_gain = 1.42;        // gives back what the gaps and the mask take

float3 texel(int2 p) {
    const float3 c = u_texture.Load(int3(clamp(p, int2(0, 0), int2(source_size.xy) - 1), 0)).rgb;
    return c * c;
}

// A source row between texel `at` and its right neighbour, `fx` (0 to 1) of the way, in linear
// light.
float3 row(int2 at, float fx) {
    return lerp(texel(at), texel(at + int2(1, 0)), smoothstep(0.0, 1.0, fx));
}

// A beam of colour c, `dist` rows from its centre.
float3 beam(float3 c, float dist) {
    const float3 sigma = lerp(k_beam_dark.xxx, k_beam_bright.xxx, sqrt(c));
    return c * exp(-0.5 * dist * dist / (sigma * sigma));
}

float4 main(Input input) : SV_Target0 {
    const float2 p = input.v_uv * source_size.xy - 0.5; // from the texel centres
    const float2 i = floor(p);
    const float2 f = p - i;
    const int2 at = int2(i);
    float3 color = beam(row(at, f.x), f.y) + beam(row(at + int2(0, 1), f.x), 1.0 - f.y);
    const float column = floor(input.v_uv.x * output_size.x);
    const float m = column - 3.0 * floor(column / 3.0);
    const float3 mask = m < 0.5   ? float3(1.0, k_mask_dim, k_mask_dim)
                        : m < 1.5 ? float3(k_mask_dim, 1.0, k_mask_dim)
                                  : float3(k_mask_dim, k_mask_dim, 1.0);
    color = saturate(color * mask * k_gain);
    return float4(sqrt(color), 1.0) * input.v_color;
}
