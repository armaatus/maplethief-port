// The scene skip (#174, ADR 0096): skip_intros; see scene_skip.h.
#include "scene_skip.h"

#include <SDL3/SDL.h>

#include "maplethief/input.h"

namespace maplethief::frontend {

namespace {

using u32 = std::uint32_t;

constexpr u32 kCanvasState = 0x0C; // SkyCanvas: the engine's current IState

void runtime_press(u32 action, bool down) {
    if (down) {
        mt_input_press(action);
    } else {
        mt_input_release(action);
    }
}

} // namespace

SceneView read_scene_view(const GuestRead32 &read) {
    SceneView v;
    u32 canvas = 0, state = 0, vptr = 0, mode = 0, popup = 0, word = 0;
    if (!read(kAutosaveCanvas, &canvas) || canvas == 0 || !read(canvas + kCanvasState, &state) ||
        state == 0 || !read(state, &vptr) || vptr != kAutosaveGameVptr ||
        !read(kAutosaveGameMode, &mode) || static_cast<std::int32_t>(mode) != kSceneGameMode ||
        !read(kAutosavePopup, &popup) || static_cast<std::int32_t>(popup) != -1 ||
        !read(state + kSceneBoxWord, &word)) {
        return v;
    }
    v.scene = true;
    v.box = ((word >> kSceneBoxShift) & 0xFF) != 0;
    u32 list = 0, step = 0;
    if (read(state + kSceneList, &list) && list != 0 && read(list + kSceneListStep, &step)) {
        v.step = static_cast<std::int32_t>(step);
    }
    return v;
}

SceneSkip::SceneSkip(Press press) : press_(press ? std::move(press) : Press(runtime_press)) {}

void SceneSkip::turn(const GuestRead32 &read, bool on, std::int32_t panel, std::uint64_t ticks,
                     bool idle, double t) {
    if (!on && !skipping_ && !tap_down_) {
        return;
    }
    const SceneView v = read_scene_view(read);
    // The tap ends after its ticks, whatever happened meanwhile (the scene may have ended).
    bool released = false;
    if (tap_down_ && ticks >= tap_at_ + kSceneTapTicks) {
        press_(MT_ACTION_OK, false);
        tap_down_ = false;
        released = true; // the next tap waits until the release has been delivered
    }
    const bool active = on && v.scene;
    if (active && !skipping_) {
        skipping_ = true;
        since_ticks_ = ticks;
        since_t_ = t;
        scene_taps_ = 0;
        stats_.scenes++;
        SDL_Log("scene_skip: a scene (game mode 9) at step %d: x%u, each speech box tapped, at "
                "%.3f s",
                v.step, kSceneSkipRate, t);
    } else if (!active && skipping_) {
        skipping_ = false;
        stats_.scene_ticks += ticks - since_ticks_;
        SDL_Log("scene_skip: %s after %llu ticks (last step %d), %llu speech box taps, %.3f s, at "
                "%.3f s",
                on ? "the scene ended" : "skip_intros off", //
                static_cast<unsigned long long>(ticks - since_ticks_), stats_.last_step,
                static_cast<unsigned long long>(scene_taps_), t - since_t_, t);
    }
    if (!active) {
        return;
    }
    stats_.last_step = v.step;
    if (v.box && !tap_down_ && !released && idle && panel == kScenePanel) {
        press_(MT_ACTION_OK, true);
        tap_down_ = true;
        tap_at_ = ticks;
        scene_taps_++;
        stats_.taps++;
    }
}

void SceneSkip::turn(const mt_cpu *cpu, const Settings &settings, double t) {
    if (!settings.skip_intros && !skipping_ && !tap_down_) {
        return;
    }
    const mt_input_stats s = mt_input_get_stats();
    turn(guest_reader(cpu), settings.skip_intros, mt_input_panel(), s.ticks, s.queued == 0, t);
}

} // namespace maplethief::frontend
