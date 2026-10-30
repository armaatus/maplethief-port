// The frontend's half of the GPU backend (#120, ADR 0060). settings.ini's gfx_backend chooses
// what draws the game's GL frames and which renderer the window gets. The present stage's passes
// live here too: fragment shaders that the Presenter draws the screen image through (#88's
// filters plug in here).
//
//   software  SDL's default renderer for the window (metal, direct3d11/12, vulkan, opengl, ...;
//             SDL picks), and mt_gfx on the CPU: the reference, and the default.
//   gpu       an SDL GPU device that can run the shader library (maplethief/gfx_gpu.h),
//             mt_gfx's GPU backend on it, and SDL's GPU renderer on the same device for the
//             window. If the window can't have that renderer (SDL's dummy video driver has no
//             surface for it), the window falls back to SDL's default renderer, and mt_gfx
//             still draws on the GPU. Without a GPU device it is the software path, and the
//             log says so: "gfx: the GPU backend is not available: ...".
//
// Nothing above mt_gfx's interface changes with the backend: the game's GL frames still reach
// Screen::take_gl_frame as pixels (mt_gfx_target_cpixels reads the GPU's result back), and the
// compositor and the Presenter work as before.
#ifndef MAPLETHIEF_PLATFORM_GFX_BACKEND_H
#define MAPLETHIEF_PLATFORM_GFX_BACKEND_H

#include "settings.h"

struct SDL_GPUDevice;
struct SDL_GPURenderState;
struct SDL_GPUShader;
struct SDL_Renderer;
struct SDL_Window;

namespace maplethief::frontend {

class GfxBackendHost {
  public:
    GfxBackendHost() = default;
    GfxBackendHost(const GfxBackendHost &) = delete;
    GfxBackendHost &operator=(const GfxBackendHost &) = delete;
    ~GfxBackendHost() { close(); }

    // Makes the window's renderer for `want` (see above) and logs what it got. Returns nullptr
    // (logged) if not even SDL's default renderer could be made.
    SDL_Renderer *open(SDL_Window *window, GfxBackend want);
    // Puts mt_gfx back on the software rasterizer (reading its targets back), destroys the
    // renderer, then the device. Logs the GPU backend's counters if it ran.
    void close();

    // mt_gfx draws on the GPU.
    bool gpu() const { return device_ != nullptr; }
    // The window's renderer is SDL's GPU renderer (so present passes work).
    bool gpu_renderer() const { return gpu_renderer_; }
    SDL_Renderer *renderer() const { return renderer_; }

  private:
    SDL_Renderer *renderer_ = nullptr;
    SDL_GPUDevice *device_ = nullptr;
    bool gpu_renderer_ = false;
};

// A pass of the present stage: a fragment shader from the shader library that SDL's GPU renderer
// draws the screen image through (SDL_GPURenderState), instead of its own texture shader. The
// shader sees the screen texture at slot 0 and SDL's vertex colour and texture coordinates
// (runtime/src/gfx/gpu/shaders/present_copy.frag.hlsl is the identity pass and the template).
// More samplers and uniforms (a filter's look-up tables, the output size) can go in the
// SDL_GPURenderStateCreateInfo that open() builds.
class PresentPass {
  public:
    PresentPass() = default;
    PresentPass(const PresentPass &) = delete;
    PresentPass &operator=(const PresentPass &) = delete;
    ~PresentPass() { close(); }

    // Makes the pass from the library's fragment shader `shader` (e.g. "present_copy.frag").
    // Returns false (logged) if `renderer` is not SDL's GPU renderer or the shader can't be made.
    bool open(SDL_Renderer *renderer, const char *shader);
    // Before the renderer goes.
    void close();
    SDL_GPURenderState *state() const { return state_; }

  private:
    SDL_GPUDevice *device_ = nullptr;
    SDL_GPUShader *shader_ = nullptr;
    SDL_GPURenderState *state_ = nullptr;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GFX_BACKEND_H
