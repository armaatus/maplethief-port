// The GPU backend on the Nintendo Switch (#242, ADR 0143), on the host: the shader library's DKSH
// programs are what sdl3-nx's deko3d backend loads (its DKO_LoadShader reads the same header
// fields), the library offers DKSH as SDL_GPU_SHADERFORMAT_PRIVATE, the Switch's window is made
// for SDL's GPU renderer exactly when the GPU draws or presents, and software stays the default.
// The GLSL bindings each DKSH was compiled from are checked when scripts/build-shaders runs
// (tools/mtport/shaderbuild.py, tests/test_shaderbuild.py). Nothing here needs a GPU.
#include <doctest/doctest.h>

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "gfx/gpu/shaders_compiled.h"
#include "gfx_backend.h"
#include "maplethief/gfx_gpu.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;

u32 word(const unsigned char *data, std::size_t at) {
    u32 v = 0;
    std::memcpy(&v, data + at, sizeof v); // DKSH is little endian, as every host we build on
    return v;
}

} // namespace

TEST_CASE("gfx dksh: every library shader has a DKSH program the deko3d backend loads (#242)") {
    std::size_t n = 0;
    for (const mt_gfx_gpu::ShaderBlob &b : mt_gfx_gpu::compiled::k_shaders) {
        CAPTURE(b.name);
        n++;
        REQUIRE(b.dksh != nullptr);
        REQUIRE(b.dksh_size >= 24);
        // DkshHeader: magic, header size, control size, code size, programs offset, count.
        CHECK(word(b.dksh, 0) == 0x48534B44u); // "DKSH"
        const u32 header = word(b.dksh, 4), control = word(b.dksh, 8), code = word(b.dksh, 12);
        const u32 programs = word(b.dksh, 16), count = word(b.dksh, 20);
        CHECK(header <= control);
        CHECK(std::size_t{control} + code == b.dksh_size);
        CHECK(control % 256 == 0); // both sections are whole 256-byte blocks (deko3d's dksh.h)
        CHECK(code % 256 == 0);
        REQUIRE(count == 1);
        REQUIRE(programs + 24 <= control);
        // DkshProgramHeader: the type (vertex 0, fragment 1), ..., the scratch per warp.
        CHECK(word(b.dksh, programs) == (b.stage == mt_gfx_gpu::MT_GFX_GPU_STAGE_VERTEX ? 0u : 1u));
        CHECK(word(b.dksh, programs + 20) <= 64u * 1024u); // sdl3-nx's DKO_PER_WARP_SCRATCH
    }
    CHECK(n == 7); // gfx.vert and .frag, present_copy, xbr, crt, lcd, colorblind
}

TEST_CASE("gfx dksh: the library offers DKSH as SDL's private format, besides the other three") {
    const SDL_GPUShaderFormat f = mt_gfx_gpu_shader_formats();
    CHECK((f & SDL_GPU_SHADERFORMAT_SPIRV) != 0);
    CHECK((f & SDL_GPU_SHADERFORMAT_DXIL) != 0);
    CHECK((f & SDL_GPU_SHADERFORMAT_MSL) != 0);
    CHECK((f & SDL_GPU_SHADERFORMAT_PRIVATE) != 0);
}

TEST_CASE("gfx dksh: the Switch's window is made for SDL's GPU renderer when it presents") {
    CHECK(fe::wants_switch_gpu_window(fe::GfxBackend::kGpu, false));
    CHECK(fe::wants_switch_gpu_window(fe::GfxBackend::kGpu, true));
    CHECK(fe::wants_switch_gpu_window(fe::GfxBackend::kSoftware, true)); // a filter's pass
    CHECK_FALSE(fe::wants_switch_gpu_window(fe::GfxBackend::kSoftware, false));
#if defined(__SWITCH__)
    CHECK(fe::kGpuOnlyWithGpuRenderer);
#else
    CHECK_FALSE(fe::kGpuOnlyWithGpuRenderer); // the desktop keeps ADR 0060's fallbacks
#endif
}

TEST_CASE("gfx dksh: gpu is the Switch's default, software everywhere else (#242, #244)") {
    // #242 left software the default until #244 measured whether the console needs the GPU; it
    // does (ADR 0145): the Switch's row is gpu, the desktop keeps the registry's software.
    CHECK(std::string(fe::platform_value("gfx_backend", fe::SettingsPlatform::kSwitch)) == "gpu");
    CHECK(fe::platform_value("gfx_backend", fe::SettingsPlatform::kDesktop) == nullptr);
#if defined(__SWITCH__)
    CHECK(fe::Settings{}.gfx_backend == fe::GfxBackend::kGpu);
#else
    CHECK(fe::Settings{}.gfx_backend == fe::GfxBackend::kSoftware);
#endif
}
