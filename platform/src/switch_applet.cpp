// The Nintendo Switch's applet side (#240, ADR 0141); see switch_applet.h.
#include "switch_applet.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <utility>

#include "autosave.h"

namespace maplethief::frontend {

namespace {

constexpr std::uint64_t kPeakStep = std::uint64_t{8} << 20; // a new peak line every 8 MiB
constexpr double kSampleSeconds = 1.0;

bool loading(std::int32_t game_mode) { return game_mode >= 6 && game_mode <= 8; }

std::string mib(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

} // namespace

std::string switch_focus_name(int focus) {
    switch (focus) {
    case static_cast<int>(SwitchFocus::kInFocus):
        return "InFocus";
    case static_cast<int>(SwitchFocus::kOutOfFocus):
        return "OutOfFocus";
    case static_cast<int>(SwitchFocus::kBackground):
        return "Background";
    default:
        return "focus " + std::to_string(focus);
    }
}

std::vector<std::uint32_t> SdlSwitchModel::focus(SwitchFocus f) {
    // SWITCH_AppletHook: only InFocus and Background change anything, and only once each way
    // (SWITCH_SetBackground).
    const bool background = f == SwitchFocus::kBackground;
    if (f == SwitchFocus::kOutOfFocus || background == background_) {
        return {};
    }
    background_ = background;
    if (background) {
        return {SDL_EVENT_WILL_ENTER_BACKGROUND, SDL_EVENT_DID_ENTER_BACKGROUND};
    }
    return {SDL_EVENT_WILL_ENTER_FOREGROUND, SDL_EVENT_DID_ENTER_FOREGROUND};
}

std::vector<std::uint32_t> SdlSwitchModel::exit_request() {
    return {SDL_EVENT_TERMINATING, SDL_EVENT_QUIT};
}

SwitchRunMode switch_run_mode(int applet_type) {
    return applet_type == 0 || applet_type == 4 ? SwitchRunMode::kTitleTakeover
                                                : SwitchRunMode::kApplet;
}

const char *switch_run_mode_name(SwitchRunMode mode) {
    switch (mode) {
    case SwitchRunMode::kTitleTakeover:
        return "title takeover";
    case SwitchRunMode::kApplet:
        return "applet mode";
    case SwitchRunMode::kNotSwitch:
        break;
    }
    return "not a Switch";
}

MemoryVerdict judge_memory(const MemorySample &s, std::uint64_t margin) {
    MemoryVerdict v;
    v.headroom = s.heap_size > s.heap_used ? s.heap_size - s.heap_used : 0;
    v.fits = v.headroom >= margin;
    return v;
}

std::string memory_line(const std::string &when, SwitchRunMode mode, const MemorySample &s) {
    const MemoryVerdict v = judge_memory(s);
    std::string line = "memory: " + when + " (" + switch_run_mode_name(mode) +
                       "): guest committed " + mib(s.guest_committed) + ", heap " +
                       mib(s.heap_used) + " of " + mib(s.heap_size) + " in use, process " +
                       mib(s.process_used) + " of " + mib(s.process_total) + "; headroom " +
                       mib(v.headroom);
    if (!v.fits) {
        line += ", below the " + mib(kAppletMarginBytes) + " margin";
    }
    return line;
}

std::string applet_mode_message(const MemorySample &s) {
    return "MapleThief is short of memory in applet mode (the Homebrew Menu started from the "
           "Album): " +
           mib(judge_memory(s).headroom) + " left of " + mib(s.heap_size) +
           ". The game may stop at the next map. Save, quit, and start the Homebrew Menu "
           "through a game title instead, which gives it the console's full memory.";
}

bool boost_wanted(bool started, std::int32_t game_mode) { return !started || loading(game_mode); }

void AppletWatch::configure(SwitchRunMode mode, Sampler sampler, Boost boost) {
    mode_ = mode;
    sampler_ = std::move(sampler);
    boost_ = std::move(boost);
    if (!enabled()) {
        return;
    }
    SDL_Log("switch: %s; the memory is logged at each map load and each new peak",
            switch_run_mode_name(mode_));
    set_boost(boost_wanted(false, -1));
}

void AppletWatch::set_boost(bool on) {
    if (on == boosted_) {
        return;
    }
    boosted_ = on;
    if (boost_) {
        boost_(on);
    }
}

std::optional<std::string> AppletWatch::judge(const MemorySample &s) {
    if (mode_ != SwitchRunMode::kApplet || warned_ || judge_memory(s).fits) {
        return std::nullopt;
    }
    warned_ = true;
    std::string message = applet_mode_message(s);
    SDL_Log("switch: %s", message.c_str());
    return message;
}

std::optional<std::string> AppletWatch::on_turn(std::int32_t game_mode, std::int32_t area,
                                                std::int32_t map, double t) {
    if (!enabled()) {
        return std::nullopt;
    }
    started_ = true;
    set_boost(boost_wanted(true, game_mode));
    const bool new_map =
        !loading(game_mode) && area >= 0 && map >= 0 && (area != area_ || map != map_);
    if (!sampler_ || (!new_map && t - last_sample_ < kSampleSeconds && samples_ != 0)) {
        return std::nullopt;
    }
    const std::optional<MemorySample> s = sampler_();
    last_sample_ = t;
    if (!s) {
        return std::nullopt;
    }
    samples_++;
    if (new_map) {
        area_ = area;
        map_ = map;
        SDL_Log("%s", memory_line("map " + std::to_string(area) + "." + std::to_string(map) +
                                      " loaded at " + std::to_string(static_cast<int>(t)) + " s",
                                  mode_, *s)
                          .c_str());
    }
    if (s->heap_used > peak_.heap_used) {
        peak_ = *s;
        if (peak_.heap_used >= peak_logged_ + kPeakStep) {
            peak_logged_ = peak_.heap_used;
            SDL_Log("%s", memory_line("a new peak at " + std::to_string(static_cast<int>(t)) + " s",
                                      mode_, peak_)
                              .c_str());
        }
    }
    return judge(*s);
}

std::optional<std::string> AppletWatch::on_turn(const mt_cpu *cpu, double t) {
    if (!enabled()) {
        return std::nullopt;
    }
    const AutosaveCheck c = check_autosave(cpu);
    return on_turn(c.mode, c.area, c.map, t);
}

void AppletWatch::finish() {
    if (!enabled()) {
        return;
    }
    set_boost(false);
    if (samples_ == 0) {
        return;
    }
    const MemoryVerdict v = judge_memory(peak_);
    std::string line = memory_line("the run's peak", mode_, peak_);
    if (mode_ == SwitchRunMode::kApplet) {
        line += v.fits ? ": applet mode fits" : ": applet mode does not fit";
    }
    SDL_Log("%s", line.c_str());
}

} // namespace maplethief::frontend
