// mt_gfx's GPU backend (#120, ADR 0060): the vertex stage of GL ES 1.1's fixed-function pipeline.
// The GLES layer has already transformed the vertices to clip space, so this only passes them
// on. One change is needed: GL's clip volume is -w <= z <= w, but SDL3 GPU's (Metal, D3D12,
// Vulkan) is 0 <= z <= w, so z becomes (z + w) / 2. The GPU then clips the same volume as
// mt_gfx's software rasterizer does.
//
// Vertex buffer 0, one vertex per 28 bytes (gpu_backend.cpp, GpuVertex): location 0 the
// clip-space position (float4), 1 the texture coordinates (float2), 2 the constant colour
// (ubyte4 normalised). SDL3 GPU wants the TEXCOORDn semantics in order (SDL_CreateGPUShader).
// scripts/build-shaders compiles this file; the headers it writes are committed.

struct Input {
    [[vk::location(0)]] float4 position : TEXCOORD0;
    [[vk::location(1)]] float2 texcoord : TEXCOORD1;
    [[vk::location(2)]] float4 color : TEXCOORD2;
};

struct Output {
    [[vk::location(0)]] float2 texcoord : TEXCOORD0;
    [[vk::location(1)]] float4 color : TEXCOORD1;
    float4 position : SV_Position;
};

Output main(Input input) {
    Output output;
    output.position = float4(input.position.xy, (input.position.z + input.position.w) * 0.5,
                             input.position.w);
    output.texcoord = input.texcoord;
    output.color = input.color;
    return output;
}
