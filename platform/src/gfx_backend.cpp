// The frontend's half of the GPU backend (#120, ADR 0060); see gfx_backend.h.
#include "gfx_backend.h"

#include <SDL3/SDL.h>

#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"

namespace maplethief::frontend {

SDL_Renderer *GfxBackendHost::open(SDL_Window *window, GfxBackend want) {
    close();
    if (want == GfxBackend::kGpu) {
        device_ = mt_gfx_gpu_create_device();
        if (device_ == nullptr) {
            SDL_Log("gfx: the GPU backend is not available: no GPU device (%s); mt_gfx stays on "
                    "the software rasterizer",
                    SDL_GetError());
        } else if (mt_gfx_gpu_install(device_) != 0) {
            SDL_Log("gfx: the GPU backend is not available: it cannot start on %s; mt_gfx stays "
                    "on the software rasterizer",
                    SDL_GetGPUDeviceDriver(device_));
            SDL_DestroyGPUDevice(device_);
            device_ = nullptr;
        } else {
            renderer_ = SDL_CreateGPURenderer(device_, window);
            gpu_renderer_ = renderer_ != nullptr;
            if (renderer_ == nullptr) {
                SDL_Log("gfx: no GPU renderer for the window (%s): SDL's default renderer "
                        "presents, mt_gfx still draws on the GPU",
                        SDL_GetError());
            }
        }
    }
    if (renderer_ == nullptr) {
        renderer_ = SDL_CreateRenderer(window, nullptr);
        if (renderer_ == nullptr) {
            SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
            close();
            return nullptr;
        }
    }
    SDL_Log("gfx: mt_gfx draws with %s; the window's renderer is %s", mt_gfx_backend_name(),
            SDL_GetRendererName(renderer_));
    return renderer_;
}

void GfxBackendHost::close() {
    if (device_ != nullptr) {
        const mt_gfx_gpu_stats s = mt_gfx_gpu_get_stats();
        SDL_Log("gfx: GPU backend: %llu flushes, %llu batches, %llu vertices, %llu pipelines, "
                "%llu texture uploads, %llu target uploads, %llu read-backs (%.1f MB)%s",
                static_cast<unsigned long long>(s.flushes),
                static_cast<unsigned long long>(s.batches),
                static_cast<unsigned long long>(s.vertices),
                static_cast<unsigned long long>(s.pipelines),
                static_cast<unsigned long long>(s.texture_uploads),
                static_cast<unsigned long long>(s.target_uploads),
                static_cast<unsigned long long>(s.readbacks),
                static_cast<double>(s.readback_bytes) / 1e6,
                s.timed_flushes != 0 ? "" : " (MAPLETHIEF_GFX_GPU_TIMING=1 times the GPU)");
        if (s.timed_flushes != 0) {
            SDL_Log("gfx: GPU time per flush with draws: %.3f ms on average, %.3f ms at most, over "
                    "%llu flushes (fence-timed, upper bounds)",
                    static_cast<double>(s.gpu_ns) / 1e6 / static_cast<double>(s.timed_flushes),
                    static_cast<double>(s.max_gpu_ns) / 1e6,
                    static_cast<unsigned long long>(s.timed_flushes));
            SDL_Log("gfx: GPU time per frame: %.3f ms median over %llu frames (fence-timed, an "
                    "upper bound)",
                    static_cast<double>(s.median_frame_gpu_ns) / 1e6,
                    static_cast<unsigned long long>(s.timed_frames));
        }
        mt_gfx_gpu_uninstall();
    }
    if (renderer_ != nullptr) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (device_ != nullptr) {
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }
    gpu_renderer_ = false;
}

bool PresentPass::open(SDL_Renderer *renderer, const char *shader) {
    close();
    device_ = renderer != nullptr ? SDL_GetGPURendererDevice(renderer) : nullptr;
    if (device_ == nullptr) {
        SDL_Log("present: pass %s needs SDL's GPU renderer", shader);
        return false;
    }
    shader_ = mt_gfx_gpu_create_shader(device_, shader);
    if (shader_ == nullptr) {
        SDL_Log("present: no shader %s: %s", shader, SDL_GetError());
        close();
        return false;
    }
    SDL_GPURenderStateCreateInfo info{};
    info.fragment_shader = shader_;
    state_ = SDL_CreateGPURenderState(renderer, &info);
    if (state_ == nullptr) {
        SDL_Log("present: pass %s: %s", shader, SDL_GetError());
        close();
        return false;
    }
    return true;
}

void PresentPass::close() {
    if (state_ != nullptr) {
        SDL_DestroyGPURenderState(state_);
        state_ = nullptr;
    }
    if (shader_ != nullptr) {
        SDL_ReleaseGPUShader(device_, shader_);
        shader_ = nullptr;
    }
    device_ = nullptr;
}

} // namespace maplethief::frontend
