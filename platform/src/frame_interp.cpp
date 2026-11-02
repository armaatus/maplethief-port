// Frame interpolation (#87, ADR 0091): see frame_interp.h.
#include "frame_interp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

#include "maplethief/diag.h"

namespace maplethief::frontend {
namespace {

// Why a tick snaps.
constexpr const char *kWhyFirst = "no tick before of the same target";
constexpr const char *kWhyGap = "the ticks are too far apart";
constexpr const char *kWhyFew = "few draws match";

// 64-bit FNV-1a.
struct Hash {
    std::uint64_t h = 0xCBF29CE484222325ull;
    void bytes(const void *p, std::size_t n) {
        const auto *b = static_cast<const std::uint8_t *>(p);
        for (std::size_t i = 0; i < n; i++) {
            h = (h ^ b[i]) * 0x00000100000001B3ull;
        }
    }
    template <typename T> void add(const T &v) { bytes(&v, sizeof v); }
};

// A vertex's window coordinates (logical pixels), false if it is behind the eye (w <= 0).
bool window_xy(const mt_gfx_vertex &v, const std::int32_t vp[4], float *x, float *y) {
    if (!(v.w > 0)) {
        return false;
    }
    *x = static_cast<float>(vp[0]) + (v.x / v.w * 0.5f + 0.5f) * static_cast<float>(vp[2]);
    *y = static_cast<float>(vp[1]) + (v.y / v.w * 0.5f + 0.5f) * static_cast<float>(vp[3]);
    return true;
}

float dist(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

// The previous draw's distance from where cur's draw `c` predicts it: where it is now, or where
// it is less the camera's scroll; the nearer of the two.
float predicted(const RecordedDraw &p, const RecordedDraw &c, float cam_x, float cam_y) {
    return std::min(dist(p.cx, p.cy, c.cx, c.cy), dist(p.cx, p.cy, c.cx - cam_x, c.cy - cam_y));
}

} // namespace

void DrawList::add(const mt_gfx_target *target, const mt_gfx_draw &draw) {
    RecordedDraw d;
    d.state = draw;
    d.state.vertices = nullptr;
    d.target = target;
    d.first = static_cast<std::uint32_t>(vertices.size());
    const std::uint32_t n = draw.vertices != nullptr ? draw.count : 0;
    d.state.count = n;
    vertices.insert(vertices.end(), draw.vertices, draw.vertices + n);
    // The centre and the keys.
    double sx = 0, sy = 0;
    std::uint32_t seen = 0;
    Hash shape;
    shape.add(draw.primitive);
    shape.add(n);
    shape.bytes(draw.viewport, sizeof draw.viewport);
    Hash key = shape;
    key.add(draw.texture);
    key.add(draw.blend);
    key.add(draw.src_factor);
    key.add(draw.dst_factor);
    for (std::uint32_t i = 0; i < n; i++) {
        const mt_gfx_vertex &v = draw.vertices[i];
        key.add(v.s);
        key.add(v.t);
        float x = 0, y = 0;
        if (window_xy(v, draw.viewport, &x, &y)) {
            sx += x;
            sy += y;
            seen++;
        }
    }
    if (seen != 0) {
        d.cx = static_cast<float>(sx / seen);
        d.cy = static_cast<float>(sy / seen);
    }
    d.key = key.h;
    d.shape = shape.h;
    draws.push_back(d);
}

void DrawList::keep(const mt_gfx_target *target) {
    DrawList kept;
    kept.draws.reserve(draws.size());
    kept.vertices.reserve(vertices.size());
    for (const RecordedDraw &d : draws) {
        if (d.target != target) {
            continue;
        }
        RecordedDraw k = d;
        k.first = static_cast<std::uint32_t>(kept.vertices.size());
        kept.vertices.insert(kept.vertices.end(), vertices.begin() + d.first,
                             vertices.begin() + d.first + d.state.count);
        kept.draws.push_back(k);
    }
    *this = std::move(kept);
}

MatchStats match_draws(const DrawList &prev, const DrawList &cur, const MatchLimits &limits,
                       std::vector<DrawMatch> *out) {
    MatchStats st;
    out->assign(cur.draws.size(), DrawMatch{});
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_key;
    for (std::uint32_t i = 0; i < prev.draws.size(); i++) {
        by_key[prev.draws[i].key].push_back(i);
    }
    // The camera's scroll: the commonest move of the draws unique in both ticks, in quarter
    // pixels. Standing still (0, 0) is a move too (the HUD); the scroll is the commonest other
    // move when it is common enough.
    std::unordered_map<std::uint64_t, std::uint32_t> cur_count;
    for (const RecordedDraw &d : cur.draws) {
        cur_count[d.key]++;
    }
    std::map<std::pair<int, int>, std::uint32_t> votes;
    for (const RecordedDraw &d : cur.draws) {
        const auto it = by_key.find(d.key);
        if (it == by_key.end() || it->second.size() != 1 || cur_count[d.key] != 1) {
            continue;
        }
        const RecordedDraw &p = prev.draws[it->second[0]];
        const float mx = d.cx - p.cx;
        const float my = d.cy - p.cy;
        if (std::hypot(mx, my) > limits.max_move) {
            continue;
        }
        votes[{static_cast<int>(std::lround(mx * 4)), static_cast<int>(std::lround(my * 4))}]++;
    }
    std::uint32_t still = 0, best = 0;
    std::pair<int, int> scroll{0, 0};
    for (const auto &[move, n] : votes) {
        if (move == std::pair<int, int>{0, 0}) {
            still = n;
        } else if (n > best) {
            best = n;
            scroll = move;
        }
    }
    if (best >= 3 && best * 4 >= still) {
        st.camera_dx = static_cast<float>(scroll.first) / 4;
        st.camera_dy = static_cast<float>(scroll.second) / 4;
    }
    const float cam_x = st.camera_dx;
    const float cam_y = st.camera_dy;

    // Exact: the same key, the nearest to where it is predicted.
    std::vector<char> used(prev.draws.size(), 0);
    for (std::size_t i = 0; i < cur.draws.size(); i++) {
        const RecordedDraw &c = cur.draws[i];
        const auto it = by_key.find(c.key);
        if (it == by_key.end()) {
            continue;
        }
        std::int32_t pick = -1;
        float pick_score = 0;
        for (const std::uint32_t j : it->second) {
            const RecordedDraw &p = prev.draws[j];
            if (used[j] != 0 || dist(p.cx, p.cy, c.cx, c.cy) > limits.max_move) {
                continue;
            }
            const float score = predicted(p, c, cam_x, cam_y);
            if (pick < 0 || score < pick_score) {
                pick = static_cast<std::int32_t>(j);
                pick_score = score;
            }
        }
        if (pick >= 0) {
            used[static_cast<std::size_t>(pick)] = 1;
            (*out)[i] = DrawMatch{MatchKind::kExact, pick, 0, 0};
            st.exact++;
        }
    }
    // Loose: the same shape, within the radius of where it is predicted, the same texture first.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_shape;
    for (std::uint32_t j = 0; j < prev.draws.size(); j++) {
        if (used[j] == 0) {
            by_shape[prev.draws[j].shape].push_back(j);
        }
    }
    constexpr float kOtherTexture = 8; // the score's penalty for another texture
    for (std::size_t i = 0; i < cur.draws.size(); i++) {
        if ((*out)[i].kind == MatchKind::kExact) {
            continue;
        }
        const RecordedDraw &c = cur.draws[i];
        const auto it = by_shape.find(c.shape);
        std::int32_t pick = -1;
        float pick_score = 0;
        if (it != by_shape.end()) {
            for (const std::uint32_t j : it->second) {
                const RecordedDraw &p = prev.draws[j];
                const float d = predicted(p, c, cam_x, cam_y);
                if (used[j] != 0 || d > limits.loose_radius) {
                    continue;
                }
                const float score = d + (p.state.texture != c.state.texture ? kOtherTexture : 0);
                if (pick < 0 || score < pick_score) {
                    pick = static_cast<std::int32_t>(j);
                    pick_score = score;
                }
            }
        }
        if (pick >= 0) {
            const RecordedDraw &p = prev.draws[static_cast<std::size_t>(pick)];
            used[static_cast<std::size_t>(pick)] = 1;
            (*out)[i] = DrawMatch{MatchKind::kLoose, pick, p.cx - c.cx, p.cy - c.cy};
            st.loose++;
        } else if (cam_x != 0 || cam_y != 0) {
            (*out)[i] = DrawMatch{MatchKind::kCamera, -1, -cam_x, -cam_y};
            st.camera++;
        } else {
            st.none++;
        }
    }
    if (cur.draws.size() >= limits.min_draws &&
        static_cast<double>(st.exact) < limits.min_exact * static_cast<double>(cur.draws.size())) {
        st.snapped = true;
        st.why = kWhyFew;
    }
    return st;
}

void blend_draw(const DrawList &prev, const DrawList &cur, std::size_t i, const DrawMatch &match,
                float alpha, std::vector<mt_gfx_vertex> *verts, std::uint8_t color[4]) {
    const RecordedDraw &c = cur.draws[i];
    const mt_gfx_vertex *cv = cur.vertices_of(c);
    verts->assign(cv, cv + c.state.count);
    std::memcpy(color, c.state.color, 4);
    if (alpha >= 1) {
        return;
    }
    const float a = std::max(alpha, 0.0f);
    if (match.kind == MatchKind::kExact) {
        const RecordedDraw &p = prev.draws[static_cast<std::size_t>(match.prev)];
        const mt_gfx_vertex *pv = prev.vertices_of(p);
        for (std::uint32_t k = 0; k < c.state.count; k++) {
            mt_gfx_vertex &v = (*verts)[k];
            v.x = pv[k].x + (cv[k].x - pv[k].x) * a;
            v.y = pv[k].y + (cv[k].y - pv[k].y) * a;
            v.z = pv[k].z + (cv[k].z - pv[k].z) * a;
            v.w = pv[k].w + (cv[k].w - pv[k].w) * a;
        }
        for (int k = 0; k < 4; k++) {
            const float b = static_cast<float>(p.state.color[k]) +
                            (static_cast<float>(c.state.color[k]) -
                             static_cast<float>(p.state.color[k])) *
                                a;
            color[k] = static_cast<std::uint8_t>(std::clamp(std::lround(b), 0L, 255L));
        }
        return;
    }
    if (match.kind == MatchKind::kNone || c.state.viewport[2] == 0 || c.state.viewport[3] == 0) {
        return;
    }
    // Moved by the rest of the way from where it was: (1 - alpha) of the offset, in clip space.
    const float ox = match.dx * (1 - a) * 2 / static_cast<float>(c.state.viewport[2]);
    const float oy = match.dy * (1 - a) * 2 / static_cast<float>(c.state.viewport[3]);
    for (mt_gfx_vertex &v : *verts) {
        v.x += ox * v.w;
        v.y += oy * v.w;
    }
}

double frame_alpha(double shown, double tick, double interval) {
    if (!(interval > 0)) {
        return 1;
    }
    // The guest's CF seconds are about 8e8: a double holds them to about 0.1 us, so the last
    // refresh of a tick can read a hair under 1. Within a thousandth of the tick is the tick.
    const double a = (shown - tick) / interval;
    return a >= 1 - kWholeTick ? 1.0 : std::clamp(a, 0.0, 1.0);
}

FrameInterpolator::~FrameInterpolator() { stop(); }

void FrameInterpolator::start() {
    if (running_) {
        return;
    }
    mt_gfx_set_submit_observer(&FrameInterpolator::observe, this);
    running_ = true;
}

void FrameInterpolator::stop() {
    if (out_ != nullptr) {
        mt_gfx_target_destroy(out_);
        out_ = nullptr;
    }
    if (!running_) {
        return;
    }
    mt_gfx_set_submit_observer(nullptr, nullptr);
    running_ = false;
    rec_.clear();
    prev_.clear();
    cur_.clear();
    active_ = false;
    have_prev_ = false;
}

void FrameInterpolator::forget() {
    rec_.clear();
    prev_.clear();
    cur_.clear();
    active_ = false;
    have_prev_ = false;
    prev_target_ = nullptr;
}

void FrameInterpolator::observe(void *user, const mt_gfx_target *target,
                                const mt_gfx_draw *draw) {
    auto *self = static_cast<FrameInterpolator *>(user);
    if (!self->replaying_) {
        self->rec_.add(target, *draw);
    }
}

bool FrameInterpolator::end_tick(const mt_gfx_target *target, double t, double interval) {
    rec_.keep(target);
    std::swap(prev_, cur_);
    std::swap(cur_, rec_);
    rec_.clear();
    const std::uint32_t w = target != nullptr ? mt_gfx_target_width(target) : 0;
    const std::uint32_t h = target != nullptr ? mt_gfx_target_height(target) : 0;
    const std::uint32_t scale = target != nullptr ? mt_gfx_target_scale(target) : 0;
    const bool same = have_prev_ && target == prev_target_ && w == w_ && h == h_ && scale == scale_;
    prev_t_ = tick_t_;
    tick_t_ = t;
    interval_ = interval > 0 ? interval : 0.05;
    w_ = w;
    h_ = h;
    scale_ = scale;
    prev_target_ = target;
    have_prev_ = target != nullptr;
    totals_.ticks++;
    last_ = MatchStats{};
    if (!same) {
        last_.snapped = true;
        last_.why = kWhyFirst;
    } else if (!(t > prev_t_) || t - prev_t_ > kMaxGap * interval_) {
        last_.snapped = true;
        last_.why = kWhyGap;
    } else {
        last_ = match_draws(prev_, cur_, limits_, &matches_);
    }
    active_ = !last_.snapped;
    if (active_) {
        totals_.exact += last_.exact;
        totals_.loose += last_.loose;
        totals_.camera += last_.camera;
        totals_.none += last_.none;
        totals_.scrolled += last_.camera_dx != 0 || last_.camera_dy != 0 ? 1 : 0;
    } else {
        totals_.snaps++;
        totals_.snap_gap += last_.why == kWhyGap ? 1 : 0;
        totals_.snap_match += last_.why == kWhyFew ? 1 : 0;
    }
    if (mt_trace_enabled("interp") != 0) {
        mt_tracef("interp",
                  "tick %llu at %.3f: %zu draws (%zu before): %s%s; exact %zu, loose %zu, camera "
                  "%zu, none %zu; scroll (%.2f, %.2f)",
                  static_cast<unsigned long long>(totals_.ticks), t, cur_.draws.size(),
                  prev_.draws.size(), active_ ? "interpolated" : "snapped: ", last_.why,
                  last_.exact, last_.loose, last_.camera, last_.none,
                  static_cast<double>(last_.camera_dx), static_cast<double>(last_.camera_dy));
    }
    return active_;
}

const mt_gfx_target *FrameInterpolator::render(float alpha) {
    if (!active_ || w_ == 0 || h_ == 0) {
        return nullptr;
    }
    if (out_ != nullptr && (mt_gfx_target_width(out_) != w_ || mt_gfx_target_height(out_) != h_ ||
                            mt_gfx_target_scale(out_) != scale_)) {
        mt_gfx_target_destroy(out_);
        out_ = nullptr;
    }
    if (out_ == nullptr && (out_ = mt_gfx_target_create(w_, h_, scale_)) == nullptr) {
        return nullptr;
    }
    // The game redraws the whole screen every tick (mt_gles_frame.retained_backing): what is
    // under its first draw does not show. Opaque black, as a fresh renderbuffer shows.
    const std::uint8_t black[4] = {0, 0, 0, 255};
    mt_gfx_target_fill(out_, black);
    replaying_ = true;
    for (std::size_t i = 0; i < cur_.draws.size(); i++) {
        const RecordedDraw &c = cur_.draws[i];
        if (c.state.texture != 0 && mt_gfx_texture_size(c.state.texture, nullptr, nullptr) != 0) {
            totals_.skipped++;
            continue;
        }
        mt_gfx_draw d = c.state;
        blend_draw(prev_, cur_, i, matches_[i], alpha, &scratch_, d.color);
        d.vertices = scratch_.data();
        d.count = static_cast<std::uint32_t>(scratch_.size());
        mt_gfx_submit(out_, &d);
    }
    replaying_ = false;
    totals_.renders++;
    return out_;
}

} // namespace maplethief::frontend
