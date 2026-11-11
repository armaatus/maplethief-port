// The Nintendo Switch's applet side (#240, ADR 0141): the decisions behind HOME, sleep, applet
// mode's memory and the CPU boost of loads, as plain code that builds and is tested on every host.
// platform/switch/switch_app.{h,cpp} asks libnx for the facts; game.cpp calls AppletWatch once
// per run-loop turn.
//
//   SdlSwitchModel   what nx-mod/sdl3-nx (ADR 0138's pin, src/video/switch/SDL_switchvideo.c)
//                    sends for libnx's applet messages: Background focus (the HOME menu or sleep)
//                    the background pair, InFocus after it the foreground pair, OutOfFocus (a
//                    library applet over the game, the software keyboard) nothing, and an exit
//                    request (closed from the HOME menu) TERMINATING then QUIT. The tests feed
//                    its events to ADR 0063's Lifecycle, so the mapping is pinned without a
//                    console.
//   SwitchRunMode    title takeover (the Homebrew Menu started through a game: the console's
//                    memory) or applet mode (from the Album: much less), from appletGetAppletType.
//   judge_memory     whether the game fits: the newlib heap (which the loader hands all of the
//                    process's memory, and from which #236's commit map takes the guest's pages)
//                    must keep kAppletMarginBytes free at the peak. Applet mode that does not fit
//                    gets applet_mode_message() on the screen; one that fits logs its headroom.
//   boost_wanted     the CPU boost (appletSetCpuBoostMode FastLoad) only during long loads: the
//                    start, until the first run-loop turn, and the game's loading modes 6 to 8
//                    (a map load); never in play.
//   AppletWatch      per turn: the boost, and the memory on each map load and at each new peak,
//                    logged as "memory: ..." lines; the message once.
#ifndef MAPLETHIEF_PLATFORM_SWITCH_APPLET_H
#define MAPLETHIEF_PLATFORM_SWITCH_APPLET_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "maplethief/recomp/cpu.h"

namespace maplethief::frontend {

// libnx's AppletFocusState values.
enum class SwitchFocus : int { kInFocus = 1, kOutOfFocus = 2, kBackground = 3 };
// "InFocus", "OutOfFocus", "Background", or "focus N".
std::string switch_focus_name(int focus);

// The SDL events nx-mod/sdl3-nx sends for each applet message, in order.
class SdlSwitchModel {
  public:
    // SWITCH_AppletHook on AppletHookType_OnFocusState with the new focus.
    std::vector<std::uint32_t> focus(SwitchFocus f);
    // SWITCH_PumpEvents when appletMainLoop returns false: SDL_EVENT_TERMINATING, SDL_EVENT_QUIT.
    static std::vector<std::uint32_t> exit_request();
    bool background() const { return background_; }

  private:
    bool background_ = false;
};

enum class SwitchRunMode { kNotSwitch, kTitleTakeover, kApplet };
// From libnx's AppletType: Application (0) and SystemApplication (4) are title takeover, any
// other applet type (LibraryApplet: the Homebrew Menu from the Album) applet mode.
SwitchRunMode switch_run_mode(int applet_type);
// "title takeover", "applet mode", "not a Switch".
const char *switch_run_mode_name(SwitchRunMode mode);

// One measurement, in bytes.
struct MemorySample {
    std::uint64_t guest_committed = 0; // the guest's pages, from the commit map
    std::uint64_t heap_size = 0;       // newlib's heap, as the loader handed it over
    std::uint64_t heap_used = 0;       // malloc's bytes in use (the guest's pages among them)
    std::uint64_t process_total = 0;   // svcGetInfo TotalMemorySize
    std::uint64_t process_used = 0;    // svcGetInfo UsedMemorySize (the whole heap, once claimed)
};

// What must stay free in the heap at the peak: a map load allocates in bursts, and a commit
// that finds no heap block ends the run. Provisional until #240's applet-mode run measures it.
inline constexpr std::uint64_t kAppletMarginBytes = std::uint64_t{64} << 20;

struct MemoryVerdict {
    bool fits = true;
    std::uint64_t headroom = 0; // heap_size - heap_used (0 when over)
};
MemoryVerdict judge_memory(const MemorySample &s, std::uint64_t margin = kAppletMarginBytes);

// The log line: "memory: <when> (<mode>): guest committed 123.4 MiB, heap 300.0 MiB of 400.0
// MiB in use, process 410.0 MiB of 420.0 MiB; headroom 100.0 MiB" plus ", below the 64 MiB margin"
// when it does not fit.
std::string memory_line(const std::string &when, SwitchRunMode mode, const MemorySample &s);
// The screen's message when applet mode does not fit.
std::string applet_mode_message(const MemorySample &s);

// Whether the CPU boost is wanted: before the first turn (`started` false), else in the game's
// loading modes 6 to 8.
bool boost_wanted(bool started, std::int32_t game_mode);

class AppletWatch {
  public:
    using Sampler = std::function<std::optional<MemorySample>()>;
    using Boost = std::function<void(bool on)>;

    // Starts the watch (once, before the guest's start): the run mode, the measurement and the
    // boost's switch (either may be empty). The boost goes on at once (boost_wanted(false, ...)).
    void configure(SwitchRunMode mode, Sampler sampler, Boost boost);
    bool enabled() const { return mode_ != SwitchRunMode::kNotSwitch; }

    // One run-loop turn at `t` seconds: the game's mode, area and map (autosave.h's check; -1
    // when not in the game). Measures at a map load (a new area and map outside the loading
    // modes) and once a second, logs a load and each peak 8 MiB above the last one logged.
    // Returns the message to show, once, when applet mode does not fit.
    std::optional<std::string> on_turn(std::int32_t game_mode, std::int32_t area, std::int32_t map,
                                       double t);
    // The same with the game's memory read through autosave.h's check_autosave.
    std::optional<std::string> on_turn(const mt_cpu *cpu, double t);
    // The run ends: the boost off, and the peak's line (with the verdict in applet mode).
    void finish();

    const MemorySample &peak() const { return peak_; }
    bool boosted() const { return boosted_; }
    std::uint64_t samples() const { return samples_; }

  private:
    void set_boost(bool on);
    std::optional<std::string> judge(const MemorySample &s);

    SwitchRunMode mode_ = SwitchRunMode::kNotSwitch;
    Sampler sampler_;
    Boost boost_;
    bool boosted_ = false;
    bool warned_ = false;
    bool started_ = false;
    double last_sample_ = 0;
    std::int32_t area_ = -1, map_ = -1; // the last map logged
    MemorySample peak_;                 // the largest heap_used seen, with its sample
    std::uint64_t peak_logged_ = 0;     // heap_used of the last peak line
    std::uint64_t samples_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SWITCH_APPLET_H
