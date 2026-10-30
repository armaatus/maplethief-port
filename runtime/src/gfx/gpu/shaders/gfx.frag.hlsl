// mt_gfx's GPU backend (#120, ADR 0060): the fragment stage of GL ES 1.1's fixed-function
// pipeline as the game uses it. The texel is modulated by the constant colour (GL_MODULATE; the
// game never calls glTexEnv). An untextured draw binds a 1x1 white texture, so the colour comes
// out unchanged. Blending is the pipeline's fixed-function blend state.
//
// Bindings, as SDL3 GPU wants them for a fragment shader (SDL_CreateGPUShader): the sampled
// texture and its sampler at (t0, s0) in space2. For SPIR-V they are one combined image sampler
// at set 2, binding 0.

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] Texture2D<float4> u_texture : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState u_sampler : register(s0, space2);

struct Input {
    [[vk::location(0)]] float2 texcoord : TEXCOORD0;
    [[vk::location(1)]] float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target0 {
    return u_texture.Sample(u_sampler, input.texcoord) * input.color;
}
