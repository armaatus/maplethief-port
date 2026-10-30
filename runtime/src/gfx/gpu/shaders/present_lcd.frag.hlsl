// The present stage's LCD filter (#88, ADR 0066): every source texel is a cell of a handheld's
// LCD, with darker gaps between the cells. It is a fragment shader for SDL's GPU renderer with
// present_copy.frag's interface (ADR 0060), plus one uniform block.
//
// An output pixel shows its texel, as nearest scaling would. Near the cell's edge it is darkened
// down to k_gap at the edge itself. The gap is k_gap_share of a cell wide, but at least one
// output pixel, so it shows at every scale and is even at a fractional fit. The look needs about
// 3 output pixels per texel, e.g. integer 3x and up.
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

static const float k_gap = 0.6;       // the brightness at a cell's edge
static const float k_gap_share = 0.2; // the gap's width, in cells

float4 main(Input input) : SV_Target0 {
    const float2 p = input.v_uv * source_size.xy;
    const float2 cell = floor(p);
    const float2 f = p - cell; // 0 to 1 across the cell
    const int2 c = clamp(int2(cell), int2(0, 0), int2(source_size.xy) - 1);
    const float4 e = u_texture.Load(int3(c, 0));
    // Output pixels per texel, and the distance to the cell's nearest edge in output pixels.
    const float2 per_texel = output_size.xy * source_size.zw;
    const float2 edge = min(f, 1.0 - f) * per_texel;
    const float2 gap = max(per_texel * k_gap_share, 1.0);
    const float2 lit = saturate(edge / gap);
    const float shade = lerp(k_gap, 1.0, min(lit.x, lit.y));
    return float4(e.rgb * shade, e.a) * input.v_color;
}
