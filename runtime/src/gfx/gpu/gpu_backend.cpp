// mt_gfx's GPU backend (#120, ADR 0060, maplethief/gfx_gpu.h): the draws of mt_gfx_submit on
// SDL3 GPU.
//
// A frame's draws are queued as batches into one vertex array, and a flush runs them. Each
// flush is one command buffer:
//   1. a copy pass: targets the CPU wrote, textures whose GPU copy is stale, and the vertices;
//   2. one render pass per run of batches on the same target (load and store, never clear: a
//      target keeps its pixels like the software one), one draw call per batch;
//   3. for a CPU read: a copy pass that downloads the target, then a fence wait and a copy into
//      the target's pixels.
// The CPU copy (mt_gfx_target.pixels) and the GPU copy (TargetSide.texture) take turns being
// the current one: cpu_newer after the CPU wrote, gpu_newer after draws ran on the GPU.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "diag/diag_internal.h"
#include "gfx/gfx_internal.h"
#include "gfx/gpu/shaders_compiled.h"
#include "maplethief/diag.h"
#include "maplethief/gfx_gpu.h"

namespace mt_gfx_gpu {
namespace {

using mt_gfx::Access;
using mt_gfx::Draw;
using mt_gfx::Texture;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

void logf(const char *fmt, const char *a = "", const char *b = "") {
    char line[400];
    std::snprintf(line, sizeof line, fmt, a, b);
    mt_diag_log(line);
}

// One vertex as the vertex shader reads it (gfx.vert.hlsl): 28 bytes.
struct GpuVertex {
    float x, y, z, w;
    float s, t;
    u8 rgba[4];
};
static_assert(sizeof(GpuVertex) == 28, "the vertex layout of gfx.vert.hlsl");

// The timed frames whose GPU time is kept for the median (an hour at 60 Hz is 216,000).
constexpr std::size_t k_max_timed_frames = std::size_t{1} << 20;

// The eleven GL ES 1.1 blend factors, as indices 0..10 and as SDL's.
constexpr u32 k_factors[11] = {MT_GFX_ZERO,
                               MT_GFX_ONE,
                               MT_GFX_SRC_COLOR,
                               MT_GFX_ONE_MINUS_SRC_COLOR,
                               MT_GFX_SRC_ALPHA,
                               MT_GFX_ONE_MINUS_SRC_ALPHA,
                               MT_GFX_DST_ALPHA,
                               MT_GFX_ONE_MINUS_DST_ALPHA,
                               MT_GFX_DST_COLOR,
                               MT_GFX_ONE_MINUS_DST_COLOR,
                               MT_GFX_SRC_ALPHA_SATURATE};
constexpr SDL_GPUBlendFactor k_sdl_factors[11] = {
    SDL_GPU_BLENDFACTOR_ZERO,
    SDL_GPU_BLENDFACTOR_ONE,
    SDL_GPU_BLENDFACTOR_SRC_COLOR,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR,
    SDL_GPU_BLENDFACTOR_SRC_ALPHA,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    SDL_GPU_BLENDFACTOR_DST_ALPHA,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA,
    SDL_GPU_BLENDFACTOR_DST_COLOR,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR,
    SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE,
};

u32 factor_index(u32 factor) {
    for (u32 i = 0; i < 11; ++i) {
        if (k_factors[i] == factor) {
            return i;
        }
    }
    return 1; // validated by gfx.cpp; not reached
}

// A pipeline: bit 0 lines (else triangles), bit 1 blending, bits 2-5 and 6-9 the factors.
u32 pipeline_key(bool lines, const Draw &d) {
    u32 key = lines ? 1u : 0u;
    if (d.blend) {
        key |= 2u | factor_index(d.src_factor) << 2 | factor_index(d.dst_factor) << 6;
    }
    return key;
}

// A sampler: bit 0 linear minification, 1 linear magnification, 2 and 3 clamped s and t.
u32 sampler_key(const Texture *t) {
    if (t == nullptr) {
        return 0; // the white texel: anything samples it the same
    }
    u32 key = mt_gfx::base_filter(t->min_filter) == MT_GFX_LINEAR ? 1u : 0u;
    key |= t->mag_filter == MT_GFX_LINEAR ? 2u : 0u;
    key |= t->wrap_s == MT_GFX_CLAMP_TO_EDGE ? 4u : 0u;
    key |= t->wrap_t == MT_GFX_CLAMP_TO_EDGE ? 8u : 0u;
    return key;
}

struct Batch {
    mt_gfx_target *target = nullptr;
    u32 pipeline = 0;
    u32 texture = 0; // handle, 0: the white texel
    u32 sampler = 0;
    SDL_GPUViewport viewport{};
    SDL_Rect scissor{};
    u32 first = 0, count = 0;
};

// What the backend keeps per target (mt_gfx_target.backend).
struct TargetSide {
    SDL_GPUTexture *texture = nullptr;
    SDL_GPUTransferBuffer *download = nullptr; // made at the first read-back
    bool cpu_newer = true;                     // the CPU's pixels must go up before the next draw
    bool gpu_newer = false; // the GPU's pixels must come down before the next CPU read
    bool queued = false;    // a batch in the queue draws into it
};

struct GpuTexture {
    SDL_GPUTexture *texture = nullptr;
    u32 width = 0, height = 0;
    u64 generation = ~0ull; // of the texels uploaded
};

bool env_on(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

const ShaderBlob *find_blob(const char *name) {
    for (const ShaderBlob &b : compiled::k_shaders) {
        if (std::strcmp(b.name, name) == 0) {
            return &b;
        }
    }
    return nullptr;
}

SDL_GPUShader *create_shader(SDL_GPUDevice *device, const char *name) {
    const ShaderBlob *blob = find_blob(name);
    if (blob == nullptr) {
        logf("gfx-gpu: no shader '%s' in the library", name);
        return nullptr;
    }
    const SDL_GPUShaderFormat have = SDL_GetGPUShaderFormats(device);
    SDL_GPUShaderCreateInfo info{};
    if ((have & SDL_GPU_SHADERFORMAT_MSL) != 0) {
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.code = blob->msl;
        info.code_size = blob->msl_size;
        info.entrypoint = "main0";
    } else if ((have & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = blob->dxil;
        info.code_size = blob->dxil_size;
        info.entrypoint = "main";
    } else if ((have & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = blob->spirv;
        info.code_size = blob->spirv_size;
        info.entrypoint = "main";
    } else {
        logf("gfx-gpu: the device takes none of the library's shader formats (%s)", name);
        return nullptr;
    }
    info.stage = blob->stage == MT_GFX_GPU_STAGE_VERTEX ? SDL_GPU_SHADERSTAGE_VERTEX
                                                        : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = blob->samplers;
    info.num_storage_textures = blob->storage_textures;
    info.num_storage_buffers = blob->storage_buffers;
    info.num_uniform_buffers = blob->uniform_buffers;
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) {
        logf("gfx-gpu: shader %s: %s", name, SDL_GetError());
    }
    return shader;
}

class GpuBackend final : public mt_gfx::Backend {
  public:
    explicit GpuBackend(SDL_GPUDevice *device) : device_(device) {
        std::snprintf(name_, sizeof name_, "gpu (%s)", SDL_GetGPUDeviceDriver(device));
        timing_ = env_on("MAPLETHIEF_GFX_GPU_TIMING");
    }
    GpuBackend(const GpuBackend &) = delete;
    GpuBackend &operator=(const GpuBackend &) = delete;
    ~GpuBackend() override { release_all(); }

    bool start();
    void set_timing(bool on) { timing_ = on; }
    // The counters, with the median of the timed frames' GPU time.
    mt_gfx_gpu_stats stats() const {
        mt_gfx_gpu_stats s = stats_;
        s.timed_frames = frame_ns_.size();
        if (!frame_ns_.empty()) {
            std::vector<u64> sorted = frame_ns_;
            const auto mid = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
            std::nth_element(sorted.begin(), mid, sorted.end());
            s.median_frame_gpu_ns = *mid;
        }
        return s;
    }
    bool flush(mt_gfx_target *download = nullptr);
    SDL_GPUTexture *target_texture(mt_gfx_target &t);

    const char *name() const noexcept override { return name_; }
    void submit(mt_gfx_target &target, const Draw &draw, mt_gfx_counters &count) noexcept override;
    void access(mt_gfx_target &target, Access access) noexcept override;
    void forget(mt_gfx_target &target) noexcept override;
    void texture_changing(u32 handle, bool destroyed) noexcept override;
    void end_frame() noexcept override;

  private:
    TargetSide *side(mt_gfx_target &t);
    SDL_GPUGraphicsPipeline *pipeline(u32 key);
    SDL_GPUSampler *sampler(u32 key);
    SDL_GPUTexture *texture_for(u32 handle);
    bool upload(SDL_GPUCopyPass *copy, SDL_GPUTexture *dst, const u8 *rgba, u32 w, u32 h);
    void release_all();
    void drop_queue();

    SDL_GPUDevice *device_;
    char name_[64] = "gpu";
    bool timing_ = false;
    bool failed_ = false; // logged once
    SDL_GPUShader *vs_ = nullptr, *fs_ = nullptr;
    std::unordered_map<u32, SDL_GPUGraphicsPipeline *> pipelines_;
    SDL_GPUSampler *samplers_[16] = {};
    SDL_GPUTexture *white_ = nullptr;
    bool white_uploaded_ = false;
    std::unordered_map<u32, GpuTexture> textures_;
    std::unordered_set<u32> queued_textures_;
    std::vector<GpuVertex> vertices_;
    std::vector<Batch> batches_;
    std::vector<TargetSide *> sides_;
    SDL_GPUBuffer *vbuf_ = nullptr;
    SDL_GPUTransferBuffer *vtransfer_ = nullptr;
    u32 vcap_ = 0; // bytes of both
    u64 frame_gpu_ns_ = 0;
    std::vector<u64> frame_ns_; // each timed frame's GPU time (at most k_max_timed_frames)
    mt_gfx_gpu_stats stats_{};
};

bool GpuBackend::start() {
    vs_ = create_shader(device_, "gfx.vert");
    fs_ = create_shader(device_, "gfx.frag");
    if (vs_ == nullptr || fs_ == nullptr) {
        return false;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = 1;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    white_ = SDL_CreateGPUTexture(device_, &info);
    // The common pipelines up front, so that a bad device fails here, not at the first frame.
    Draw opaque;
    Draw over;
    over.blend = true;
    over.src_factor = MT_GFX_SRC_ALPHA;
    over.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
    return white_ != nullptr && pipeline(pipeline_key(false, opaque)) != nullptr &&
           pipeline(pipeline_key(false, over)) != nullptr && sampler(0) != nullptr;
}

void GpuBackend::release_all() {
    drop_queue();
    for (TargetSide *s : sides_) {
        if (s->texture != nullptr) {
            SDL_ReleaseGPUTexture(device_, s->texture);
        }
        if (s->download != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, s->download);
        }
        delete s;
    }
    sides_.clear();
    for (auto &[handle, t] : textures_) {
        SDL_ReleaseGPUTexture(device_, t.texture);
    }
    textures_.clear();
    for (auto &[key, p] : pipelines_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, p);
    }
    pipelines_.clear();
    for (SDL_GPUSampler *&s : samplers_) {
        if (s != nullptr) {
            SDL_ReleaseGPUSampler(device_, s);
            s = nullptr;
        }
    }
    if (white_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, white_);
        white_ = nullptr;
    }
    if (vbuf_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, vbuf_);
        vbuf_ = nullptr;
    }
    if (vtransfer_ != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, vtransfer_);
        vtransfer_ = nullptr;
    }
    if (vs_ != nullptr) {
        SDL_ReleaseGPUShader(device_, vs_);
        vs_ = nullptr;
    }
    if (fs_ != nullptr) {
        SDL_ReleaseGPUShader(device_, fs_);
        fs_ = nullptr;
    }
}

void GpuBackend::drop_queue() {
    for (const Batch &b : batches_) {
        static_cast<TargetSide *>(b.target->backend)->queued = false;
    }
    batches_.clear();
    vertices_.clear();
    queued_textures_.clear();
}

TargetSide *GpuBackend::side(mt_gfx_target &t) {
    if (t.backend == nullptr) {
        auto *s = new (std::nothrow) TargetSide;
        if (s == nullptr) {
            return nullptr;
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = t.width;
        info.height = t.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        s->texture = SDL_CreateGPUTexture(device_, &info);
        if (s->texture == nullptr) {
            logf("gfx-gpu: no target texture: %s", SDL_GetError());
            delete s;
            return nullptr;
        }
        sides_.push_back(s);
        t.backend = s;
    }
    return static_cast<TargetSide *>(t.backend);
}

SDL_GPUGraphicsPipeline *GpuBackend::pipeline(u32 key) {
    const auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
        return it->second;
    }
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(GpuVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[3] = {};
    attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVertex, x)};
    attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, s)};
    attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuVertex, rgba)};

    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUColorTargetBlendState &blend = color.blend_state;
    blend.enable_blend = (key & 2u) != 0;
    if (blend.enable_blend) {
        const SDL_GPUBlendFactor src = k_sdl_factors[(key >> 2) & 15u];
        const SDL_GPUBlendFactor dst = k_sdl_factors[(key >> 6) & 15u];
        // GL ES applies the factors to all four channels; SDL takes the alpha factors apart.
        // SRC_ALPHA_SATURATE's alpha factor is 1 in GL ES and in all three APIs.
        blend.src_color_blendfactor = blend.src_alpha_blendfactor = src;
        blend.dst_color_blendfactor = blend.dst_alpha_blendfactor = dst;
        blend.color_blend_op = blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs_;
    info.fragment_shader = fs_;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = 3;
    info.primitive_type =
        (key & 1u) != 0 ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true; // clip z like GL, do not clamp it
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(device_, &info);
    if (p == nullptr) {
        logf("gfx-gpu: no pipeline: %s", SDL_GetError());
        return nullptr;
    }
    pipelines_.emplace(key, p);
    stats_.pipelines = pipelines_.size();
    return p;
}

SDL_GPUSampler *GpuBackend::sampler(u32 key) {
    if (samplers_[key] == nullptr) {
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter = (key & 1u) != 0 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        info.mag_filter = (key & 2u) != 0 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST; // one level: the base filters
        info.address_mode_u = (key & 4u) != 0 ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
                                              : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        info.address_mode_v = (key & 8u) != 0 ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
                                              : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.min_lod = 0;
        info.max_lod = 0;
        samplers_[key] = SDL_CreateGPUSampler(device_, &info);
        if (samplers_[key] == nullptr) {
            logf("gfx-gpu: no sampler: %s", SDL_GetError());
        }
    }
    return samplers_[key];
}

void GpuBackend::submit(mt_gfx_target &target, const Draw &d, mt_gfx_counters &count) noexcept {
    const u32 prim = d.src->primitive;
    const bool lines =
        prim == MT_GFX_LINES || prim == MT_GFX_LINE_STRIP || prim == MT_GFX_LINE_LOOP;
    const mt_gfx_vertex *v = d.src->vertices;
    const u32 n = d.src->count;

    // The scissor: the viewport cut to the target (pixels, image rows y down).
    const i64 x0 = std::max<i64>(0, static_cast<i64>(d.vp_x));
    const i64 x1 = std::min<i64>(target.width, static_cast<i64>(d.vp_x + d.vp_w));
    const i64 top = static_cast<i64>(target.height) - static_cast<i64>(d.vp_y + d.vp_h);
    const i64 y0 = std::max<i64>(0, top);
    const i64 y1 =
        std::min<i64>(target.height, static_cast<i64>(target.height) - static_cast<i64>(d.vp_y));
    const bool visible = d.vp_w > 0 && d.vp_h > 0 && x0 < x1 && y0 < y1 && !failed_;

    const std::size_t first = vertices_.size();
    u64 triangles = 0, lines_n = 0;
    auto finite = [](const mt_gfx_vertex &p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
               std::isfinite(p.w) && std::isfinite(p.s) && std::isfinite(p.t);
    };
    // Where the software rasterizer's rules and the GPU's differ only in rounding, the vertices
    // are nudged so that the GPU rounds the same way (ADR 0060):
    //   - Positions are snapped to the software's 1/16-pixel grid in window coordinates (and
    //     mapped back to clip space), so coverage and the interpolated texture coordinates
    //     start from the same points. GPUs snap to 1/256 of a pixel, which holds that grid.
    //   - Line endpoints then move by (-1/256, -1/256) of a pixel: GL ES's diamond-exit rule
    //     perturbs them by (-e, -e^2), which decides lines that run exactly along pixel edges.
    //   - Texture coordinates move by +1/1024 texel. A nearest sample exactly on a texel edge
    //     is the texel above it in the software, but the GPU's float interpolation can land
    //     a hair below it. Bilinear weights are 8-bit, so the shift doesn't show there.
    const double sx_bias =
        d.texture != nullptr ? 1.0 / (1024.0 * static_cast<double>(d.texture->width)) : 0.0;
    const double st_bias =
        d.texture != nullptr ? 1.0 / (1024.0 * static_cast<double>(d.texture->height)) : 0.0;
    const double line_shift = lines ? 1.0 / 256.0 : 0.0;
    auto put = [&](const mt_gfx_vertex &p) {
        GpuVertex g;
        g.x = p.x;
        g.y = p.y;
        g.z = p.z;
        g.w = p.w;
        // Only vertices in front of the eye and inside the software's guard band
        // (|x|, |y| <= 16 w); the GPU clips the others itself.
        if (p.w > 0 && std::fabs(p.x) <= 16.0f * p.w && std::fabs(p.y) <= 16.0f * p.w) {
            // The software's mapping (gfx.cpp, to_raster), in float like it...
            const float wx = (p.x / p.w + 1.0f) * 0.5f * d.vp_w + d.vp_x;
            const float wy = (p.y / p.w + 1.0f) * 0.5f * d.vp_h + d.vp_y;
            const double snx =
                std::floor(static_cast<double>(wx * 16.0f + 0.5f)) / 16.0 - line_shift;
            const double sny =
                std::floor(static_cast<double>(wy * 16.0f + 0.5f)) / 16.0 - line_shift;
            // ...and back to clip space, in double.
            const double w = p.w;
            g.x = static_cast<float>(((snx - d.vp_x) / d.vp_w * 2.0 - 1.0) * w);
            g.y = static_cast<float>(((sny - d.vp_y) / d.vp_h * 2.0 - 1.0) * w);
        }
        g.s = static_cast<float>(p.s + sx_bias);
        g.t = static_cast<float>(p.t + st_bias);
        std::memcpy(g.rgba, d.color, 4);
        vertices_.push_back(g);
    };
    try {
        auto tri = [&](u32 a, u32 b, u32 c) {
            ++triangles;
            // The software rasterizer drops a triangle with a vertex that is not finite.
            if (visible && finite(v[a]) && finite(v[b]) && finite(v[c])) {
                put(v[a]);
                put(v[b]);
                put(v[c]);
            }
        };
        auto seg = [&](u32 a, u32 b) {
            ++lines_n;
            if (visible && finite(v[a]) && finite(v[b])) {
                put(v[a]);
                put(v[b]);
            }
        };
        if (prim == MT_GFX_LINES) {
            for (u32 i = 0; i + 1 < n; i += 2) {
                seg(i, i + 1);
            }
        } else if (prim == MT_GFX_LINE_STRIP || prim == MT_GFX_LINE_LOOP) {
            for (u32 i = 0; i + 1 < n; ++i) {
                seg(i, i + 1);
            }
            if (prim == MT_GFX_LINE_LOOP && n >= 2) {
                seg(n - 1, 0);
            }
        } else if (prim == MT_GFX_TRIANGLES) {
            for (u32 i = 0; i + 2 < n; i += 3) {
                tri(i, i + 1, i + 2);
            }
        } else if (prim == MT_GFX_TRIANGLE_STRIP) {
            for (u32 i = 0; i + 2 < n; ++i) {
                if ((i & 1u) == 0) {
                    tri(i, i + 1, i + 2);
                } else {
                    tri(i + 1, i, i + 2);
                }
            }
        } else {
            for (u32 i = 1; i + 1 < n; ++i) {
                tri(0, i, i + 1);
            }
        }
    } catch (const std::bad_alloc &) {
        vertices_.resize(first);
    }
    count.triangles += triangles;
    count.lines += lines_n;
    const std::size_t added = vertices_.size() - first;
    if (added == 0) {
        return;
    }
    TargetSide *s = side(target);
    if (s == nullptr) {
        vertices_.resize(first);
        return;
    }
    Batch b;
    b.target = &target;
    b.pipeline = pipeline_key(lines, d);
    b.texture = d.texture != nullptr ? d.texture_handle : 0;
    b.sampler = sampler_key(d.texture);
    b.viewport.x = d.vp_x;
    b.viewport.y = static_cast<float>(target.height) - d.vp_y - d.vp_h;
    b.viewport.w = d.vp_w;
    b.viewport.h = d.vp_h;
    b.viewport.min_depth = 0;
    b.viewport.max_depth = 1;
    b.scissor = {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
                 static_cast<int>(y1 - y0)};
    b.first = static_cast<u32>(first);
    b.count = static_cast<u32>(added);
    if (!batches_.empty()) {
        Batch &last = batches_.back();
        if (last.target == b.target && last.pipeline == b.pipeline && last.texture == b.texture &&
            last.sampler == b.sampler &&
            std::memcmp(&last.viewport, &b.viewport, sizeof b.viewport) == 0 &&
            std::memcmp(&last.scissor, &b.scissor, sizeof b.scissor) == 0 &&
            last.first + last.count == b.first) {
            last.count += b.count;
            return;
        }
    }
    try {
        batches_.push_back(b);
        if (b.texture != 0) {
            queued_textures_.insert(b.texture);
        }
    } catch (const std::bad_alloc &) {
        vertices_.resize(first);
        return;
    }
    s->queued = true;
}

bool GpuBackend::upload(SDL_GPUCopyPass *copy, SDL_GPUTexture *dst, const u8 *rgba, u32 w, u32 h) {
    const u32 bytes = w * h * 4u;
    SDL_GPUTransferBufferCreateInfo tinfo{};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    if (tb == nullptr) {
        return false;
    }
    void *mem = SDL_MapGPUTransferBuffer(device_, tb, false);
    if (mem == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return false;
    }
    std::memcpy(mem, rgba, bytes);
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row = w;
    src.rows_per_layer = h;
    SDL_GPUTextureRegion region{};
    region.texture = dst;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &region, false);
    SDL_ReleaseGPUTransferBuffer(device_, tb); // freed once the command buffer is done with it
    stats_.upload_bytes += bytes;
    return true;
}

// The GPU copy of a texture, uploaded (in the coming copy pass) if stale. 0: the white texel.
SDL_GPUTexture *GpuBackend::texture_for(u32 handle) {
    if (handle == 0) {
        return white_;
    }
    const auto it = textures_.find(handle);
    return it != textures_.end() ? it->second.texture : white_;
}

bool GpuBackend::flush(mt_gfx_target *download) {
    TargetSide *dside =
        download != nullptr ? static_cast<TargetSide *>(download->backend) : nullptr;
    const bool read = dside != nullptr && (dside->gpu_newer || dside->queued);
    if (batches_.empty() && !read) {
        return true;
    }
    if (failed_) {
        drop_queue();
        return false;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        logf("gfx-gpu: no command buffer: %s (the GPU backend draws nothing more)", SDL_GetError());
        failed_ = true;
        drop_queue();
        return false;
    }
    const bool draws = !batches_.empty();
    if (draws) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        if (!white_uploaded_) {
            const u8 white[4] = {255, 255, 255, 255};
            white_uploaded_ = upload(copy, white_, white, 1, 1);
        }
        // Targets the CPU wrote since the GPU last drew them.
        for (const Batch &b : batches_) {
            auto *s = static_cast<TargetSide *>(b.target->backend);
            if (s->cpu_newer) {
                upload(copy, s->texture, b.target->pixels.data(), b.target->width,
                       b.target->height);
                s->cpu_newer = false;
                ++stats_.target_uploads;
            }
        }
        // Textures whose image changed since their upload.
        for (u32 handle : queued_textures_) {
            const Texture *t = mt_gfx::find_texture(handle);
            if (t == nullptr || t->texels.empty()) {
                continue; // cannot happen: gfx.cpp validated it and texture_changing flushed
            }
            GpuTexture &g = textures_[handle];
            if (g.texture != nullptr && g.generation == t->generation) {
                continue;
            }
            if (g.texture == nullptr || g.width != t->width || g.height != t->height) {
                if (g.texture != nullptr) {
                    SDL_ReleaseGPUTexture(device_, g.texture);
                }
                SDL_GPUTextureCreateInfo info{};
                info.type = SDL_GPU_TEXTURETYPE_2D;
                info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
                info.width = t->width;
                info.height = t->height;
                info.layer_count_or_depth = 1;
                info.num_levels = 1;
                g.texture = SDL_CreateGPUTexture(device_, &info);
                g.width = t->width;
                g.height = t->height;
            }
            if (g.texture != nullptr &&
                upload(copy, g.texture, t->texels.data(), t->width, t->height)) {
                g.generation = t->generation;
                ++stats_.texture_uploads;
            }
        }
        // The vertices.
        const u32 bytes = static_cast<u32>(vertices_.size() * sizeof(GpuVertex));
        if (bytes > vcap_) {
            u32 cap = std::max<u32>(vcap_ * 2, 64u * 1024u);
            while (cap < bytes) {
                cap *= 2;
            }
            if (vbuf_ != nullptr) {
                SDL_ReleaseGPUBuffer(device_, vbuf_);
            }
            if (vtransfer_ != nullptr) {
                SDL_ReleaseGPUTransferBuffer(device_, vtransfer_);
            }
            SDL_GPUBufferCreateInfo binfo{};
            binfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            binfo.size = cap;
            vbuf_ = SDL_CreateGPUBuffer(device_, &binfo);
            SDL_GPUTransferBufferCreateInfo tinfo{};
            tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tinfo.size = cap;
            vtransfer_ = SDL_CreateGPUTransferBuffer(device_, &tinfo);
            vcap_ = vbuf_ != nullptr && vtransfer_ != nullptr ? cap : 0;
        }
        void *mem = vcap_ != 0 ? SDL_MapGPUTransferBuffer(device_, vtransfer_, true) : nullptr;
        if (mem != nullptr) {
            std::memcpy(mem, vertices_.data(), bytes);
            SDL_UnmapGPUTransferBuffer(device_, vtransfer_);
            SDL_GPUTransferBufferLocation src{vtransfer_, 0};
            SDL_GPUBufferRegion dst{vbuf_, 0, bytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            stats_.upload_bytes += bytes;
        }
        SDL_EndGPUCopyPass(copy);
        if (mem == nullptr) {
            logf("gfx-gpu: no vertex buffer: %s (the GPU backend draws nothing more)",
                 SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            failed_ = true;
            drop_queue();
            return false;
        }

        // The draws: one render pass per run of batches on the same target.
        SDL_GPURenderPass *pass = nullptr;
        const mt_gfx_target *current = nullptr;
        u32 bound_pipeline = ~0u, bound_texture = ~0u, bound_sampler = ~0u;
        for (const Batch &b : batches_) {
            if (b.target != current) {
                if (pass != nullptr) {
                    SDL_EndGPURenderPass(pass);
                }
                auto *s = static_cast<TargetSide *>(b.target->backend);
                SDL_GPUColorTargetInfo ct{};
                ct.texture = s->texture;
                ct.load_op = SDL_GPU_LOADOP_LOAD; // a target is never cleared
                ct.store_op = SDL_GPU_STOREOP_STORE;
                pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
                const SDL_GPUBufferBinding vb{vbuf_, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                current = b.target;
                bound_pipeline = bound_texture = bound_sampler = ~0u;
            }
            if (b.pipeline != bound_pipeline) {
                SDL_GPUGraphicsPipeline *p = pipeline(b.pipeline);
                if (p == nullptr) {
                    continue;
                }
                SDL_BindGPUGraphicsPipeline(pass, p);
                bound_pipeline = b.pipeline;
            }
            if (b.texture != bound_texture || b.sampler != bound_sampler) {
                SDL_GPUTextureSamplerBinding tsb{texture_for(b.texture), sampler(b.sampler)};
                if (tsb.sampler == nullptr) {
                    continue;
                }
                SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
                bound_texture = b.texture;
                bound_sampler = b.sampler;
            }
            SDL_SetGPUViewport(pass, &b.viewport);
            SDL_SetGPUScissor(pass, &b.scissor);
            SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
            ++stats_.batches;
        }
        if (pass != nullptr) {
            SDL_EndGPURenderPass(pass);
        }
        for (const Batch &b : batches_) {
            auto *s = static_cast<TargetSide *>(b.target->backend);
            s->gpu_newer = true;
            s->queued = false;
        }
        stats_.vertices += vertices_.size();
        ++stats_.flushes;
        batches_.clear();
        vertices_.clear();
        queued_textures_.clear();
    }

    // The read-back.
    const u32 rbytes = read ? download->width * download->height * 4u : 0;
    if (read) {
        if (dside->download == nullptr) {
            SDL_GPUTransferBufferCreateInfo tinfo{};
            tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            tinfo.size = rbytes;
            dside->download = SDL_CreateGPUTransferBuffer(device_, &tinfo);
        }
        if (dside->download != nullptr) {
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion region{};
            region.texture = dside->texture;
            region.w = download->width;
            region.h = download->height;
            region.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = dside->download;
            dst.pixels_per_row = download->width;
            dst.rows_per_layer = download->height;
            SDL_DownloadFromGPUTexture(copy, &region, &dst);
            SDL_EndGPUCopyPass(copy);
        }
    }

    const bool wait = read || (timing_ && draws);
    if (!wait) {
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            logf("gfx-gpu: submit failed: %s", SDL_GetError());
            return false;
        }
        return true;
    }
    const auto submitted = std::chrono::steady_clock::now();
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        logf("gfx-gpu: submit failed: %s", SDL_GetError());
        return false;
    }
    const bool ok = SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
    const auto done = std::chrono::steady_clock::now();
    if (timing_ && draws) {
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(done - submitted).count());
        ++stats_.timed_flushes;
        stats_.gpu_ns += ns;
        stats_.max_gpu_ns = std::max(stats_.max_gpu_ns, ns);
        frame_gpu_ns_ += ns;
    }
    if (!ok) {
        logf("gfx-gpu: fence wait failed: %s", SDL_GetError());
        return false;
    }
    if (read && dside->download != nullptr) {
        const void *mem = SDL_MapGPUTransferBuffer(device_, dside->download, false);
        if (mem != nullptr) {
            std::memcpy(download->pixels.data(), mem, rbytes);
            SDL_UnmapGPUTransferBuffer(device_, dside->download);
            dside->gpu_newer = false;
            ++stats_.readbacks;
            stats_.readback_bytes += rbytes;
        } else {
            logf("gfx-gpu: read-back failed: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

void GpuBackend::access(mt_gfx_target &target, Access access) noexcept {
    auto *s = static_cast<TargetSide *>(target.backend);
    if (access == Access::kOverwrite) {
        // The CPU replaces every pixel: run what is queued (it may draw other targets), but
        // read nothing back.
        flush();
        s->gpu_newer = false;
    } else {
        flush(&target);
    }
    if (access != Access::kRead) {
        s->cpu_newer = true;
    }
}

void GpuBackend::forget(mt_gfx_target &target) noexcept {
    auto *s = static_cast<TargetSide *>(target.backend);
    if (s == nullptr) {
        return;
    }
    if (s->queued) {
        flush(); // the other targets' batches are interleaved with this one's
    }
    if (s->texture != nullptr) {
        SDL_ReleaseGPUTexture(device_, s->texture);
    }
    if (s->download != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, s->download);
    }
    sides_.erase(std::remove(sides_.begin(), sides_.end(), s), sides_.end());
    delete s;
    target.backend = nullptr;
}

void GpuBackend::texture_changing(u32 handle, bool destroyed) noexcept {
    if (queued_textures_.count(handle) != 0) {
        flush();
    }
    if (destroyed) {
        const auto it = textures_.find(handle);
        if (it != textures_.end()) {
            SDL_ReleaseGPUTexture(device_, it->second.texture);
            textures_.erase(it);
        }
    }
}

void GpuBackend::end_frame() noexcept {
    flush();
    stats_.last_frame_gpu_ns = frame_gpu_ns_;
    if (timing_ && frame_gpu_ns_ != 0 && frame_ns_.size() < k_max_timed_frames) {
        frame_ns_.push_back(frame_gpu_ns_);
    }
    if (timing_ && mt_trace_enabled("gfx")) {
        mt_tracef("gfx", "gpu: %.3f ms GPU (%llu batches so far)",
                  static_cast<double>(frame_gpu_ns_) / 1e6,
                  static_cast<unsigned long long>(stats_.batches));
    }
    frame_gpu_ns_ = 0;
}

// For a host that presents on the GPU (#130, ADR 0068): the target's GPU texture with its
// current pixels. The queued draws run first. A target without a GPU side gets one, and pixels
// the CPU wrote since the GPU last drew it go up now (as the next flush would upload them), so
// the texture never shows stale pixels. Nothing is read back.
SDL_GPUTexture *GpuBackend::target_texture(mt_gfx_target &t) {
    if (failed_) {
        return nullptr;
    }
    TargetSide *s = side(t);
    if (s == nullptr) {
        return nullptr;
    }
    if (!flush()) {
        return nullptr;
    }
    if (s->cpu_newer) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            logf("gfx-gpu: no command buffer: %s", SDL_GetError());
            return nullptr;
        }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        const bool ok = upload(copy, s->texture, t.pixels.data(), t.width, t.height);
        SDL_EndGPUCopyPass(copy);
        if (!SDL_SubmitGPUCommandBuffer(cmd) || !ok) {
            logf("gfx-gpu: target upload failed: %s", SDL_GetError());
            return nullptr;
        }
        s->cpu_newer = false;
        ++stats_.target_uploads;
    }
    return s->texture;
}

std::unique_ptr<GpuBackend> &installed() {
    static std::unique_ptr<GpuBackend> b;
    return b;
}

} // namespace
} // namespace mt_gfx_gpu

using namespace mt_gfx_gpu;

extern "C" SDL_GPUShaderFormat mt_gfx_gpu_shader_formats(void) noexcept {
    return SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
}

extern "C" SDL_GPUDevice *mt_gfx_gpu_create_device(void) noexcept {
    // SDL GPU needs the video subsystem, even without a window (a test, a headless run).
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        logf("gfx-gpu: no GPU device: no video subsystem: %s", SDL_GetError());
        return nullptr;
    }
    const char *driver = std::getenv("MAPLETHIEF_GPU_DRIVER");
    SDL_GPUDevice *device =
        SDL_CreateGPUDevice(mt_gfx_gpu_shader_formats(), false,
                            driver != nullptr && driver[0] != '\0' ? driver : nullptr);
    if (device == nullptr) {
        logf("gfx-gpu: no GPU device: %s", SDL_GetError());
    }
    return device;
}

extern "C" int mt_gfx_gpu_install(SDL_GPUDevice *device) noexcept {
    if (device == nullptr) {
        return -1;
    }
    mt_gfx_gpu_uninstall();
    auto b = std::unique_ptr<GpuBackend>(new (std::nothrow) GpuBackend(device));
    if (b == nullptr || !b->start()) {
        logf("gfx-gpu: the GPU backend cannot start on %s: the software rasterizer stays",
             SDL_GetGPUDeviceDriver(device));
        return -1;
    }
    mt_gfx::set_backend(b.get());
    installed() = std::move(b);
    logf("gfx-gpu: mt_gfx draws on the GPU: %s", installed()->name());
    return 0;
}

extern "C" void mt_gfx_gpu_uninstall(void) noexcept {
    if (installed() == nullptr) {
        return;
    }
    mt_gfx::set_backend(nullptr); // reads every target back and forgets it
    installed().reset();
}

extern "C" int mt_gfx_gpu_installed(void) noexcept { return installed() != nullptr ? 1 : 0; }

extern "C" int mt_gfx_gpu_flush(void) noexcept {
    return installed() != nullptr && !installed()->flush() ? -1 : 0;
}

extern "C" void mt_gfx_gpu_set_timing(int on) noexcept {
    if (installed() != nullptr) {
        installed()->set_timing(on != 0);
    }
}

extern "C" mt_gfx_gpu_stats mt_gfx_gpu_get_stats(void) noexcept {
    return installed() != nullptr ? installed()->stats() : mt_gfx_gpu_stats{};
}

extern "C" SDL_GPUTexture *mt_gfx_gpu_target_texture(const mt_gfx_target *target) noexcept {
    // Like mt_gfx_target_cpixels: syncing is not a change of the pixels.
    return installed() != nullptr && target != nullptr
               ? installed()->target_texture(*const_cast<mt_gfx_target *>(target))
               : nullptr;
}

extern "C" SDL_GPUShader *mt_gfx_gpu_create_shader(SDL_GPUDevice *device,
                                                   const char *name) noexcept {
    return device != nullptr && name != nullptr ? create_shader(device, name) : nullptr;
}
