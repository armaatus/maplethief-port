// The present stage's colour step: the colour-blind filter (#158, ADR 0093), daltonisation by one
// 3x3 matrix in linear RGB (platform/src/color_filter.h: D = I + E (I - S), Machado et al. 2009's
// simulation S and Fidaner et al. 2005's error shift E), and the picture adjustments (#187):
// brightness, contrast and saturation folded into the same matrix with an offset, gamma on the
// encoded value. One pass for both. It is a fragment shader for SDL's GPU renderer with
// present_copy.frag's interface (ADR 0060), plus one uniform block. The Presenter draws the
// screen texture through it into a render target of the same size, one output pixel per texel,
// before the border and the picture (with any shader filter's pass) read that target.
//
// Texels are read with integer loads, so the scale mode's sampler doesn't change the result.

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState u_sampler : register(s0, space2);

// Set by the Presenter before the draw (ColorUniforms in platform/src/color_filter.h).
[[vk::binding(0, 3)]] cbuffer Colour : register(b0, space3) {
    float4 row0;        // the step's rows (xyz: A D) and offset (w); D alone, w 0, for the filter
    float4 row1;
    float4 row2;
    float4 source_size; // the screen image in texels: w, h, 1/w, 1/h
    float4 encode;      // x: 1 / gamma (#187; 1: the sRGB encoding alone)
};

struct Input {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_uv : TEXCOORD0;
};

// IEC 61966-2-1.
float3 srgb_to_linear(float3 v) {
    const float3 lo = v / 12.92;
    const float3 hi = pow((v + 0.055) / 1.055, 2.4);
    return float3(v.r <= 0.04045 ? lo.r : hi.r, v.g <= 0.04045 ? lo.g : hi.g,
                  v.b <= 0.04045 ? lo.b : hi.b);
}

float3 linear_to_srgb(float3 v) {
    const float3 lo = v * 12.92;
    const float3 hi = 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    return float3(v.r <= 0.0031308 ? lo.r : hi.r, v.g <= 0.0031308 ? lo.g : hi.g,
                  v.b <= 0.0031308 ? lo.b : hi.b);
}

float4 main(Input input) : SV_Target0 {
    const int2 c = clamp(int2(input.v_uv * source_size.xy), int2(0, 0), int2(source_size.xy) - 1);
    const float4 e = u_texture.Load(int3(c, 0));
    const float3 lin = srgb_to_linear(saturate(e.rgb));
    const float3 d = saturate(float3(dot(row0.xyz, lin), dot(row1.xyz, lin), dot(row2.xyz, lin)) +
                              float3(row0.w, row1.w, row2.w));
    float3 s = linear_to_srgb(d);
    if (encode.x != 1.0) {
        s = pow(s, encode.x); // gamma (#187)
    }
    return float4(s, e.a) * input.v_color;
}
