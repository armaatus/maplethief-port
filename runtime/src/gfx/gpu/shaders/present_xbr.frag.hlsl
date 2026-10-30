// The present stage's xBR-style filter (#88, ADR 0066): edge smoothing for pixel art at any
// output scale, in one pass. It is a fragment shader for SDL's GPU renderer with
// present_copy.frag's interface (ADR 0060), plus one uniform block.
//
// Each output pixel lies in one quadrant of a source texel E. The rule is Scale2x's (EPX)
// corner rule, applied to that quadrant. X is the neighbour across the quadrant's vertical edge
// and Y the one across its horizontal edge; Xo and Yo are the neighbours on the far sides. If
// X and Y have the same colour, X differs from Yo, and Y differs from Xo, an edge runs
// diagonally past E's corner. The corner beyond the 45-degree line through the two edge
// midpoints then takes X's colour. That is xBR's level-1 edge, not Scale2x's square quadrant.
// The line is anti-aliased over one output pixel, so it looks the same at 2x, 3x or a
// fractional fit. Everywhere else the output pixel is E, as nearest scaling would draw it:
// flat areas, isolated pixels, checkerboards and straight edges are unchanged.
//
// Texels are read with integer loads, clamped to the image, so the scale mode's sampler doesn't
// change the result. Colours count as the same when no channel differs by more than
// k_same (24/255): exact for pixel art, tolerant of the game's dithered gradients.

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

static const float k_same = 24.0 / 255.0;

float4 texel(int2 p) {
    return u_texture.Load(int3(clamp(p, int2(0, 0), int2(source_size.xy) - 1), 0));
}

bool same(float4 a, float4 b) {
    const float3 d = abs(a.rgb - b.rgb);
    return max(d.r, max(d.g, d.b)) <= k_same;
}

float4 main(Input input) : SV_Target0 {
    const float2 p = input.v_uv * source_size.xy; // texel space
    const float2 cell = floor(p);
    const float2 f = p - cell - 0.5; // from E's centre, -0.5 to 0.5
    const int2 c = int2(cell);
    const int2 d = int2(f.x < 0.0 ? -1 : 1, f.y < 0.0 ? -1 : 1);
    const float4 e = texel(c);
    const float4 x = texel(c + int2(d.x, 0));
    const float4 y = texel(c + int2(0, d.y));
    const float4 xo = texel(c - int2(d.x, 0));
    const float4 yo = texel(c - int2(0, d.y));
    float4 color = e;
    if (same(x, y) && !same(x, yo) && !same(y, xo) && !same(e, x)) {
        // The signed distance past the line |fx| + |fy| = 0.5, in texels, over the size of one
        // output pixel in texels: the share of the pixel on X's side.
        const float dist = (abs(f.x) + abs(f.y) - 0.5) * 0.70710678;
        const float pixel = max(source_size.x * output_size.z, source_size.y * output_size.w);
        color = lerp(e, x, saturate(dist / pixel + 0.5));
    }
    return color * input.v_color;
}
