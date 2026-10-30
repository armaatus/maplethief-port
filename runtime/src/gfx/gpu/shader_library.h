// The compiled shader library (#120, ADR 0060): what shaders_compiled.h (written by
// scripts/build-shaders) lists for each HLSL source in runtime/src/gfx/gpu/shaders/.
#ifndef MAPLETHIEF_GFX_GPU_SHADER_LIBRARY_H
#define MAPLETHIEF_GFX_GPU_SHADER_LIBRARY_H

#include <cstddef>
#include <cstdint>

namespace mt_gfx_gpu {

enum ShaderStage : std::uint8_t { MT_GFX_GPU_STAGE_VERTEX, MT_GFX_GPU_STAGE_FRAGMENT };

struct ShaderBlob {
    const char *name; // "<source name>.<stage>", e.g. "gfx.frag"
    ShaderStage stage;
    const unsigned char *spirv;
    std::size_t spirv_size;
    const unsigned char *dxil;
    std::size_t dxil_size;
    const unsigned char *msl; // MSL source text (not NUL-terminated), entry point main0
    std::size_t msl_size;
    std::uint32_t samplers, storage_textures, storage_buffers, uniform_buffers;
};

} // namespace mt_gfx_gpu

#endif // MAPLETHIEF_GFX_GPU_SHADER_LIBRARY_H
