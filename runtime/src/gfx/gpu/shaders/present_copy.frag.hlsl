// The present stage's identity pass (#120, ADR 0060). It is a fragment shader for SDL's GPU
// renderer (SDL_CreateGPURenderState), which the frontend's Presenter draws the screen image
// through: the texel times the vertex colour, which is what SDL's own texture_rgba shader
// computes, without SDL's colour scale. Post-process filters (#88) are shaders like this one: the
// same inputs and bindings, with a different body.
//
// The interface is the one SDL's renderer vertex shader (tri_texture.vert) feeds: v_color at
// COLOR0 (SPIR-V location 0) and v_uv at TEXCOORD0 (location 1). The screen texture and its
// sampler sit at (t0, s0) in space2, a combined image sampler at set 2, binding 0 for SPIR-V.
// More samplers would follow at t1/s1 and up (SDL_GPURenderStateCreateInfo.sampler_bindings),
// and uniforms at b0 and up in space3 (SDL_SetGPURenderStateFragmentUniforms).

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState u_sampler : register(s0, space2);

struct Input {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    return u_texture.Sample(u_sampler, input.v_uv) * input.v_color;
}
