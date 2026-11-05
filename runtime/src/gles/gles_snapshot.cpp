// Save states (#203, ADR 0105): the GL state and mt_gfx's textures as state file sections.
//
//   gfx    every mt_gfx texture by handle: its logical and texel sizes, whether a replacement
//          hook replaced it, its RGBA8 texels, filters and wrap modes, and the next handle. The
//          game's upload buffers are freed after glTexImage2D, so the texels exist nowhere else.
//          A replaced texture is saved with the replacement's texels (a restore in the process
//          that saved it gives the same picture; the hook is not asked again).
//   gles   the tracker's state: the GL names (textures with their mt_gfx handles, renderbuffers,
//          framebuffers), the next names, the bindings, the matrix stacks, the viewport, the
//          error flag, the fixed-function state and the current EAGL context.
//
// A restore replaces every texture (the GPU backend is told each live one is gone, so it
// uploads the restored texels when it next draws with them). The renderbuffers are the
// screen's: made once at start-up, with an mt_gfx target the presenter knows, so they are kept
// and must match the state's (names, sizes, formats). Their pixels are saved and put back: the
// game draws every frame over the last one without a glClear, and some of its draws blend with
// what is there, so the frame after a restore depends on the one before. A target of another
// pixel size (the render scale changed since) keeps its pixels.
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gfx/gfx_internal.h"
#include "gles/gles_internal.h"
#include "snapshot/snapshot_internal.h"

namespace mt_gles {
namespace {

// --- gfx ---------------------------------------------------------------------------------------

constexpr u32 k_max_dim = MT_GFX_MAX_SIZE * MT_GFX_MAX_SCALE;

bool save_gfx(mt_cpu *, mt_snap::Writer &w, std::string *) {
    std::vector<std::pair<u32, const mt_gfx::Texture *>> all;
    mt_gfx::texture_each(
        [](void *user, u32 handle, const mt_gfx::Texture &t) {
            static_cast<std::vector<std::pair<u32, const mt_gfx::Texture *>> *>(user)->emplace_back(
                handle, &t);
        },
        &all);
    std::sort(all.begin(), all.end());
    w.u32(mt_gfx::texture_next());
    w.u32(static_cast<u32>(all.size()));
    for (const auto &[handle, t] : all) {
        w.u32(handle);
        w.u32(t->logical_width);
        w.u32(t->logical_height);
        w.u32(t->width);
        w.u32(t->height);
        w.boolean(t->replaced);
        w.blob(t->texels);
        w.u32(t->min_filter);
        w.u32(t->mag_filter);
        w.u32(t->wrap_s);
        w.u32(t->wrap_t);
        w.u64(t->generation);
    }
    return true;
}

bool load_gfx(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    u32 next = 0, n = 0;
    if (!r.u32(&next) || !r.u32(&n) || !r.count_fits(n, 49)) {
        return false;
    }
    auto map = std::make_shared<std::unordered_map<u32, mt_gfx::Texture>>();
    for (u32 i = 0; i < n; i++) {
        u32 handle = 0;
        mt_gfx::Texture t;
        if (!r.u32(&handle) || !r.u32(&t.logical_width) || !r.u32(&t.logical_height) ||
            !r.u32(&t.width) || !r.u32(&t.height) || !r.boolean(&t.replaced) ||
            !r.blob(&t.texels) || !r.u32(&t.min_filter) || !r.u32(&t.mag_filter) ||
            !r.u32(&t.wrap_s) || !r.u32(&t.wrap_t) || !r.u64(&t.generation)) {
            return false;
        }
        if (handle == 0 || map->count(handle) != 0 || t.width > k_max_dim || t.height > k_max_dim ||
            t.texels.size() != static_cast<std::size_t>(t.width) * t.height * 4u) {
            return r.fail("a texture with a bad handle or size");
        }
        map->emplace(handle, std::move(t));
    }
    *apply = [map, next](mt_cpu *) { mt_gfx::texture_replace_all(std::move(*map), next); };
    return true;
}

// --- gles --------------------------------------------------------------------------------------

void put_matrix(mt_snap::Writer &w, const Matrix &m) {
    for (const float f : m.m) {
        w.f32(f);
    }
}
bool get_matrix(mt_snap::Reader &r, Matrix *m) {
    for (float &f : m->m) {
        if (!r.f32(&f)) {
            return false;
        }
    }
    return true;
}
void put_array(mt_snap::Writer &w, const ClientArray &a) {
    w.boolean(a.enabled);
    w.u32(a.size);
    w.u32(a.type);
    w.u32(a.stride);
    w.u32(a.pointer);
}
bool get_array(mt_snap::Reader &r, ClientArray *a) {
    return r.boolean(&a->enabled) && r.u32(&a->size) && r.u32(&a->type) && r.u32(&a->stride) &&
           r.u32(&a->pointer);
}

template <typename Map> std::vector<u32> sorted_names(const Map &m) {
    std::vector<u32> names;
    for (const auto &e : m) {
        names.push_back(e.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool save_gles(mt_cpu *, mt_snap::Writer &w, std::string *) {
    const State &s = state();
    w.u32(static_cast<u32>(s.textures.size()));
    for (const u32 name : sorted_names(s.textures)) {
        const Texture &t = s.textures.at(name);
        w.u32(name);
        w.boolean(t.bound);
        for (const u32 v : {t.width, t.height, t.internal_format, t.format, t.type, t.bytes, t.gfx,
                            t.min_filter}) {
            w.u32(v);
        }
        w.boolean(t.generate_mipmap);
    }
    w.u32(static_cast<u32>(s.renderbuffers.size()));
    for (const u32 name : sorted_names(s.renderbuffers)) {
        const Renderbuffer &rb = s.renderbuffers.at(name);
        w.u32(name);
        w.u32(rb.width);
        w.u32(rb.height);
        w.u32(rb.format);
        w.boolean(rb.retained);
        const mt_gfx_target *t = rb.target;
        const u32 pw = t != nullptr ? mt_gfx_target_pixel_width(t) : 0;
        const u32 ph = t != nullptr ? mt_gfx_target_pixel_height(t) : 0;
        w.u32(pw);
        w.u32(ph);
        const u8 *px = t != nullptr ? mt_gfx_target_cpixels(t) : nullptr;
        w.blob(px != nullptr ? std::vector<u8>(px, px + static_cast<std::size_t>(pw) * ph * 4u)
                             : std::vector<u8>());
    }
    w.u32(static_cast<u32>(s.framebuffers.size()));
    for (const u32 name : sorted_names(s.framebuffers)) {
        const Framebuffer &fb = s.framebuffers.at(name);
        w.u32(name);
        w.u32(fb.color);
        w.u32(fb.depth);
        w.u32(fb.stencil);
    }
    for (const u32 v :
         {s.next_texture,    s.next_renderbuffer, s.next_framebuffer, s.framebuffer,
          s.renderbuffer,    s.texture,           s.matrix_mode,      s.depth[0],
          s.depth[1],        s.depth[2],          s.viewport[0],      s.viewport[1],
          s.viewport[2],     s.viewport[3],       s.texture_bytes,    s.error,
          s.current_context, s.frame_calls,       s.frame_draws,      s.frame_uploads}) {
        w.u32(v);
    }
    const Fixed &f = s.fixed;
    for (const Matrix &m : f.modelview) {
        put_matrix(w, m);
    }
    for (const Matrix &m : f.projection) {
        put_matrix(w, m);
    }
    for (const Matrix &m : f.texture) {
        put_matrix(w, m);
    }
    put_array(w, f.vertex);
    put_array(w, f.texcoord);
    for (const bool b :
         {f.normal_array, f.color_array, f.point_size_array, f.texture_2d, f.blend}) {
        w.boolean(b);
    }
    std::vector<std::pair<u32, bool>> caps(f.caps.begin(), f.caps.end());
    std::sort(caps.begin(), caps.end());
    w.u32(static_cast<u32>(caps.size()));
    for (const auto &[cap, on] : caps) {
        w.u32(cap);
        w.boolean(on);
    }
    w.raw(f.color, 4);
    w.u32(f.src_factor);
    w.u32(f.dst_factor);
    w.u32(eagl_current());
    return true;
}

struct Pixels {
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
};

struct Loaded {
    State s; // the saved values (rt, binding, stats, seen and the renderbuffers' targets unset)
    std::unordered_map<u32, Pixels> pixels; // each renderbuffer's target
    u32 eagl = 0;
};

bool load_gles(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    auto l = std::make_shared<Loaded>();
    State &s = l->s;
    u32 n = 0;
    if (!r.u32(&n) || !r.count_fits(n, 39)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u32 name = 0;
        Texture t;
        if (!r.u32(&name) || !r.boolean(&t.bound)) {
            return false;
        }
        for (u32 *v : {&t.width, &t.height, &t.internal_format, &t.format, &t.type, &t.bytes,
                       &t.gfx, &t.min_filter}) {
            if (!r.u32(v)) {
                return false;
            }
        }
        if (!r.boolean(&t.generate_mipmap)) {
            return false;
        }
        s.textures[name] = t;
    }
    if (!r.u32(&n) || !r.count_fits(n, 29)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u32 name = 0;
        Renderbuffer rb;
        Pixels px;
        if (!r.u32(&name) || !r.u32(&rb.width) || !r.u32(&rb.height) || !r.u32(&rb.format) ||
            !r.boolean(&rb.retained) || !r.u32(&px.width) || !r.u32(&px.height) ||
            !r.blob(&px.rgba)) {
            return false;
        }
        if (px.width > k_max_dim || px.height > k_max_dim ||
            px.rgba.size() != static_cast<std::size_t>(px.width) * px.height * 4u) {
            return r.fail("a renderbuffer's pixels do not match its size");
        }
        s.renderbuffers[name] = rb;
        l->pixels[name] = std::move(px);
    }
    if (!r.u32(&n) || !r.count_fits(n, 16)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u32 name = 0;
        Framebuffer fb;
        if (!r.u32(&name) || !r.u32(&fb.color) || !r.u32(&fb.depth) || !r.u32(&fb.stencil)) {
            return false;
        }
        s.framebuffers[name] = fb;
    }
    for (u32 *v :
         {&s.next_texture,    &s.next_renderbuffer, &s.next_framebuffer, &s.framebuffer,
          &s.renderbuffer,    &s.texture,           &s.matrix_mode,      &s.depth[0],
          &s.depth[1],        &s.depth[2],          &s.viewport[0],      &s.viewport[1],
          &s.viewport[2],     &s.viewport[3],       &s.texture_bytes,    &s.error,
          &s.current_context, &s.frame_calls,       &s.frame_draws,      &s.frame_uploads}) {
        if (!r.u32(v)) {
            return false;
        }
    }
    if (s.depth[0] < 1 || s.depth[0] > k_modelview_depth || s.depth[1] < 1 ||
        s.depth[1] > k_projection_depth || s.depth[2] < 1 || s.depth[2] > k_texture_depth) {
        return r.fail("a matrix stack depth out of range");
    }
    Fixed &f = s.fixed;
    for (Matrix &m : f.modelview) {
        if (!get_matrix(r, &m)) {
            return false;
        }
    }
    for (Matrix &m : f.projection) {
        if (!get_matrix(r, &m)) {
            return false;
        }
    }
    for (Matrix &m : f.texture) {
        if (!get_matrix(r, &m)) {
            return false;
        }
    }
    if (!get_array(r, &f.vertex) || !get_array(r, &f.texcoord)) {
        return false;
    }
    for (bool *b :
         {&f.normal_array, &f.color_array, &f.point_size_array, &f.texture_2d, &f.blend}) {
        if (!r.boolean(b)) {
            return false;
        }
    }
    if (!r.u32(&n) || !r.count_fits(n, 5)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u32 cap = 0;
        bool on = false;
        if (!r.u32(&cap) || !r.boolean(&on)) {
            return false;
        }
        f.caps[cap] = on;
    }
    if (!r.raw(f.color, 4) || !r.u32(&f.src_factor) || !r.u32(&f.dst_factor) || !r.u32(&l->eagl)) {
        return false;
    }
    // The screen's renderbuffers stay this process's: the state's must be the same ones.
    const State &live = state();
    bool same = live.renderbuffers.size() == s.renderbuffers.size();
    for (const auto &[name, rb] : s.renderbuffers) {
        const auto it = live.renderbuffers.find(name);
        same = same && it != live.renderbuffers.end() && it->second.width == rb.width &&
               it->second.height == rb.height && it->second.format == rb.format;
    }
    if (!same) {
        return r.fail("the screen's renderbuffers differ from the state's (another screen size)");
    }
    *apply = [l](mt_cpu *) {
        State &g = state();
        State &from = l->s;
        for (auto &[name, rb] : from.renderbuffers) {
            rb.target = g.renderbuffers[name].target;
            const Pixels &px = l->pixels[name];
            if (rb.target != nullptr && !px.rgba.empty() &&
                mt_gfx_target_pixel_width(rb.target) == px.width &&
                mt_gfx_target_pixel_height(rb.target) == px.height) {
                u8 *dst = mt_gfx_target_pixels(rb.target);
                if (dst != nullptr) {
                    std::copy(px.rgba.begin(), px.rgba.end(), dst);
                }
            }
        }
        g.textures = std::move(from.textures);
        g.renderbuffers = std::move(from.renderbuffers);
        g.framebuffers = std::move(from.framebuffers);
        g.next_texture = from.next_texture;
        g.next_renderbuffer = from.next_renderbuffer;
        g.next_framebuffer = from.next_framebuffer;
        g.framebuffer = from.framebuffer;
        g.renderbuffer = from.renderbuffer;
        g.texture = from.texture;
        g.matrix_mode = from.matrix_mode;
        std::copy(from.depth, from.depth + 3, g.depth);
        std::copy(from.viewport, from.viewport + 4, g.viewport);
        g.texture_bytes = from.texture_bytes;
        g.error = from.error;
        g.current_context = from.current_context;
        g.frame_calls = from.frame_calls;
        g.frame_draws = from.frame_draws;
        g.frame_uploads = from.frame_uploads;
        g.fixed = std::move(from.fixed);
        eagl_restore_current(l->eagl);
    };
    return true;
}

} // namespace

extern const mt_snap::Module k_snap_gfx, k_snap_gles;
const mt_snap::Module k_snap_gfx = {"gfx", save_gfx, load_gfx};
const mt_snap::Module k_snap_gles = {"gles", save_gles, load_gles};

} // namespace mt_gles
