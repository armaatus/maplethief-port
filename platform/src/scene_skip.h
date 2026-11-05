// The scene skip (#174, ADR 0096, docs/re/qol.md#cutscene-skip): skip_intros.
//
// A scene (the intro, a chapter card, a boss's entrance or end, an ending) is a CAniManager
// command list that the scene runner CStateGame::PaintExitToMenu 0x83C71 steps through, one step
// per CStateGame::Paint 0x87501 in game mode 9 (0x11AA00). Between steps Paint moves the hero, the
// NPCs and the scene's sprites by their velocities every frame, and the commands set positions,
// create NPCs, run the NPC scripts, start boss fights and change maps. So jumping steps, or
// running several in one frame, would change what the scene leaves behind. The skip runs every
// frame instead, faster:
//
//   - while a scene plays, rate() is kSceneSkipRate: the guest clock's rate (game.cpp folds it
//     into fast-forward's, ADR 0087), so every tick, step and command runs in the game's order
//     and each tick still sees the game's own interval. Only the host's wait shrinks;
//   - each speech box (CStateGame +0x1E9 set: the runner waits for a key) gets a tap of OK,
//     pressed and released kSceneTapTicks ticks later through mt_input_press / release, as the
//     cut-scene panel's button (key pad panel 3, -5) would send it. CStateGame::OnKeyPressed
//     shows the next two lines or closes the box, as for a player's tap.
//
// The game can't tell a skipped scene from one watched by a player who taps each box at once.
// The fixed-step clock (tests) has no rate (ADR 0087), so there the skip is the taps only.
//
// "A scene" is the engine's CStateGame (vtable 0x119B78) in game mode 9 with no popup
// (0x108C64 == -1). Every read goes through a GuestRead32 (autosave.h), so a synthetic guest
// tests it. Time is game ticks (mt_input_get_stats().ticks). Single thread: the turn hook's.
#ifndef MAPLETHIEF_PLATFORM_SCENE_SKIP_H
#define MAPLETHIEF_PLATFORM_SCENE_SKIP_H

#include <cstdint>
#include <functional>

#include "autosave.h"
#include "settings.h"

namespace maplethief::frontend {

// Guest facts (docs/re/qol.md#cutscene-skip).
inline constexpr std::int32_t kSceneGameMode = 9;     // 0x11AA00 while a scene plays
inline constexpr std::uint32_t kSceneBoxWord = 0x1E8; // CStateGame: the word holding +0x1E9
inline constexpr std::uint32_t kSceneBoxShift = 8;    // +0x1E9: a speech box waits for a key
inline constexpr std::uint32_t kSceneList = 0x38;     // CStateGame: the scene's command list
inline constexpr std::uint32_t kSceneListStep = 0x14; // the list: the next step to run
inline constexpr std::int32_t kScenePanel = 3;        // the key pad's cut-scene panel

// The guest clock's rate while a scene is skipped. The game's 50 ms tick becomes 6.25 ms of host
// time: the intro's 444 steps take about 3 s instead of 22 s. A host that can't keep up runs the
// ticks back to back.
inline constexpr std::uint32_t kSceneSkipRate = 8;
inline constexpr std::uint64_t kSceneTapTicks = 2; // OK held for 2 ticks: 0.1 s at 50 ms

// What the skip reads of the game in one turn.
struct SceneView {
    bool scene = false;     // CStateGame in game mode 9, no popup
    bool box = false;       // a speech box waits for a key (+0x1E9)
    std::int32_t step = -1; // the scene's next step (the list's +0x14), -1 if unknown
};
SceneView read_scene_view(const GuestRead32 &read);

class SceneSkip {
  public:
    // Where presses go: mt_input_press / mt_input_release by default.
    using Press = std::function<void(std::uint32_t action, bool down)>;
    explicit SceneSkip(Press press = nullptr);

    // One run-loop turn. `on`: skip_intros; `panel` the key pad's panel (mt_input_panel),
    // `ticks` the game ticks so far, `idle`: no action waits to be delivered
    // (mt_input_get_stats().queued == 0), `t` the run's seconds (for the log).
    void turn(const GuestRead32 &read, bool on, std::int32_t panel, std::uint64_t ticks, bool idle,
              double t);
    // The same on the game: guest memory, mt_input_panel and mt_input_get_stats.
    void turn(const mt_cpu *cpu, const Settings &settings, double t);
    // A save state is restored (#203, save_state_input.h): a tap still down is released now
    // (before the restore replaces the runtime's input) and the skip starts over.
    void restart();

    // The guest clock's rate the skip asks for now: kSceneSkipRate while it skips a scene, else 1.
    std::uint32_t rate() const { return skipping_ ? kSceneSkipRate : 1; }
    bool skipping() const { return skipping_; }

    struct Stats {
        std::uint64_t scenes = 0;      // scenes skipped (entered with the skip on)
        std::uint64_t taps = 0;        // speech box taps
        std::uint64_t scene_ticks = 0; // game ticks inside skipped scenes
        std::int32_t last_step = -1;   // the last scene's last step seen
    };
    const Stats &stats() const { return stats_; }

  private:
    Press press_;
    Stats stats_;
    bool skipping_ = false;
    std::uint64_t since_ticks_ = 0; // the tick the scene's skip began
    double since_t_ = 0;
    std::uint64_t scene_taps_ = 0;
    bool tap_down_ = false; // OK pressed by the skip, not yet released
    std::uint64_t tap_at_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SCENE_SKIP_H
