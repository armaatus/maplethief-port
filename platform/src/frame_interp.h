// Frame interpolation (#87, ADR 0091): frame_rate = smooth shows a picture at every display
// refresh while the game's logic stays at its 50 ms ticks.
//
// The game's logic and drawing are locked 1:1 inside CStateGame::Paint (docs/re/timing-
// threading.md): there is no point in the engine between two ticks to draw from. So the port
// interpolates the game's draws instead (the idea of Ship of Harkinian's frame interpolation):
//
//   record   mt_gfx's submit observer copies every draw of a tick (the vertices, the state)
//   match    at the tick's GL present, each draw of the tick is matched with one of the tick
//            before (match_draws):
//              exact   the same texture, primitive, vertex count, viewport, blending and texture
//                      coordinates, the nearest one after the camera's scroll (below); blended
//                      vertex by vertex, and its colour too (fades)
//              loose   the same primitive, vertex count and viewport, any texture coordinates (a
//                      sprite's next animation frame), within kLooseRadius of where it was;
//                      moved by the blend of its centre
//              camera  the rest, when the camera scrolled: moved with the camera's scroll
//              none    the rest, drawn as the tick drew it
//            The camera's scroll is the commonest move of the draws that are unique in both
//            ticks (their key appears once in each), voted to a quarter pixel.
//   snap     no interpolation for a tick (its own frame is shown, as with frame_rate =
//            original) when the ticks are of different targets, more than kMaxGap intervals
//            apart (a load, a pause, the settings menu), or too few draws match (a map change,
//            a teleport, a scene cut), or the widescreen view changed (below)
//   render   the tick at fraction alpha from the tick before (0) to it (1): its draws replayed
//            into a target of its own with the blended vertices (render), on the software
//            rasterizer or the GPU backend alike
//
// The window then shows, at a present whose picture is seen at guest time t, the tick at
// alpha = (t - the tick's guest time) / the tick interval (frame_alpha): with display-locked
// pacing at 60 Hz the three refreshes of a tick show 1/3, 2/3 and 1. The tick's own frame is
// fully shown n - 1 refreshes later than with original (33 ms at 60 Hz): the latency the
// smoothness costs.
//
// Trace: MAPLETHIEF_TRACE=interp logs each tick's matches and why it snapped.
//
// Widescreen (#177, ADR 0091's addendum): the wide GL frame is replayed like a narrow one, since
// the vertices a draw submits are clip space with the backdrop's and the centred screens' host x
// transforms already in them. Two things must not be blended over: a tick drawn at another view
// width (SkyCanvas +0x88, ADR 0090) and a change in the host x transforms the tick's draws used
// (mt_gles_set_host_x_transform: a popup menu's paint begins or ends). Both snap. A draw is also
// never matched with one under another transform (it is in the keys).
//
// Only the GL frame is interpolated: UIKit's views (the menus, the HUD's views) stay at the
// ticks' rate. Pure: no SDL; mt_gfx is the runtime's.
#ifndef MAPLETHIEF_PLATFORM_FRAME_INTERP_H
#define MAPLETHIEF_PLATFORM_FRAME_INTERP_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "maplethief/gfx.h"

namespace maplethief::frontend {

// One recorded draw: the draw's state (vertices = nullptr) and where its vertices are.
struct RecordedDraw {
    mt_gfx_draw state{};
    const mt_gfx_target *target = nullptr;
    std::uint32_t first = 0; // its first vertex in DrawList::vertices
    float cx = 0, cy = 0;    // the centre of its vertices in window coordinates (logical pixels)
    std::uint64_t key = 0;   // exact: the shape, texture, blending, texture coordinates
    std::uint64_t shape = 0; // loose: primitive, count, viewport, host x transform
    float host_scale = 1, host_offset = 0; // mt_gles_set_host_x_transform when it was drawn
};

// A tick's draws.
struct DrawList {
    std::vector<RecordedDraw> draws;
    std::vector<mt_gfx_vertex> vertices;

    void clear() {
        draws.clear();
        vertices.clear();
    }
    // Copies `draw` (to `target`, under the host's x transform of the time) in.
    void add(const mt_gfx_target *target, const mt_gfx_draw &draw, float host_scale = 1,
             float host_offset = 0);
    // Keeps only the draws to `target`, in order.
    void keep(const mt_gfx_target *target);
    const mt_gfx_vertex *vertices_of(const RecordedDraw &d) const { return &vertices[d.first]; }
};

enum class MatchKind : std::uint8_t { kExact, kLoose, kCamera, kNone };

// What a draw of the current tick is matched with.
struct DrawMatch {
    MatchKind kind = MatchKind::kNone;
    std::int32_t prev = -1; // kExact, kLoose: the previous tick's draw
    // kLoose, kCamera: where the draw was a tick ago, from where it is (window pixels).
    float dx = 0, dy = 0;
};

struct MatchLimits {
    float max_move = 96;       // the farthest a matched draw moves in a tick (logical pixels)
    float loose_radius = 24;   // a loose match's farthest distance from where it was predicted
    double min_exact = 0.3;    // fewer exact matches than this share of the draws: snap
    std::size_t min_draws = 8; // the share is judged from this many draws on
};

struct MatchStats {
    std::size_t exact = 0, loose = 0, camera = 0, none = 0;
    float camera_dx = 0, camera_dy = 0; // the camera's scroll, current - previous (window pixels)
    bool snapped = false;
    const char *why = ""; // why it snapped
};

// Matches the draws of `cur` with those of `prev` (out: one DrawMatch per draw of cur).
MatchStats match_draws(const DrawList &prev, const DrawList &cur, const MatchLimits &limits,
                       std::vector<DrawMatch> *out);

// The vertices and colour of cur's draw `i` at `alpha` (0: where the previous tick drew it, 1:
// where cur drew it) into `verts` and `color`.
void blend_draw(const DrawList &prev, const DrawList &cur, std::size_t i, const DrawMatch &match,
                float alpha, std::vector<mt_gfx_vertex> *verts, std::uint8_t color[4]);

// The fraction of the tick at guest time `shown` (the time the present's picture is seen) for a
// tick at guest time `tick`, `interval` seconds long: 0 to 1, and 1 from 1 - kWholeTick on.
inline constexpr double kWholeTick = 1e-3;
double frame_alpha(double shown, double tick, double interval);

class FrameInterpolator {
  public:
    // A tick more than this many intervals after the one before snaps (a load, a pause).
    static constexpr double kMaxGap = 2.5;

    FrameInterpolator() = default;
    FrameInterpolator(const FrameInterpolator &) = delete;
    FrameInterpolator &operator=(const FrameInterpolator &) = delete;
    ~FrameInterpolator();

    // Records mt_gfx's draws from now on (mt_gfx_set_submit_observer), or no more (forgets the
    // ticks and frees its target: before mt_gfx's backend goes). Only one interpolator records
    // at a time.
    void start();
    void stop();
    // Forgets the ticks (the next one snaps) and keeps recording: before the targets change.
    void forget();
    bool running() const { return running_; }

    // The game presented `target` (its GL present) at guest time `t` (seconds), its tick
    // interval `interval` seconds, drawn at view width `view` (480 unless widescreen is on): the
    // draws to it since the last end_tick are this tick's. Returns whether it interpolates from
    // the tick before (false: it snapped).
    bool end_tick(const mt_gfx_target *target, double t, double interval, std::uint32_t view = 480);
    // Whether the last tick interpolates (a pair of ticks matched).
    bool active() const { return active_; }
    // The last tick's guest time and interval.
    double tick_time() const { return tick_t_; }
    double interval() const { return interval_; }
    // The view width the last tick was drawn at.
    std::uint32_t view() const { return view_; }

    // Draws the last tick at `alpha` into the interpolator's own target (the size and scale of
    // the tick's) and returns it; nullptr if it is not active or the target cannot be made.
    // alpha >= 1 draws the tick's draws unchanged. Textures freed since the tick are skipped.
    const mt_gfx_target *render(float alpha);

    const MatchStats &last() const { return last_; }
    struct Totals {
        std::uint64_t ticks = 0, snaps = 0, renders = 0;
        std::uint64_t exact = 0, loose = 0, camera = 0, none = 0; // over the interpolated ticks
        std::uint64_t skipped = 0;    // draws not replayed (a texture freed since)
        std::uint64_t scrolled = 0;   // interpolated ticks with a camera scroll
        std::uint64_t snap_gap = 0;   // snaps: the ticks too far apart
        std::uint64_t snap_match = 0; // snaps: too few draws match (the rest: no tick before)
        std::uint64_t snap_view = 0;  // snaps: the view width or the host's x transforms changed
    };
    const Totals &totals() const { return totals_; }

  private:
    static void observe(void *user, const mt_gfx_target *target, const mt_gfx_draw *draw);

    bool running_ = false;
    bool replaying_ = false;
    DrawList rec_, prev_, cur_;
    std::vector<DrawMatch> matches_;
    std::vector<mt_gfx_vertex> scratch_;
    bool active_ = false;
    bool have_prev_ = false;
    const mt_gfx_target *prev_target_ = nullptr;
    double prev_t_ = 0, tick_t_ = 0, interval_ = 0.05;
    std::uint32_t w_ = 0, h_ = 0, scale_ = 0;                 // the tick's target's
    std::uint32_t view_ = 480;                                // the tick's view width
    std::vector<std::pair<float, float>> hosts_, prev_hosts_; // its host x transforms, sorted
    mt_gfx_target *out_ = nullptr;
    MatchLimits limits_;
    MatchStats last_;
    Totals totals_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_FRAME_INTERP_H
