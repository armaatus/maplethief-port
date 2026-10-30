// Rumble (#72, ADR 0053): the active gamepad (Gamepads::active(), ADR 0048) rumbles on
//
//   vibrate     the game's own vibration: AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
//               (mt_audio_set_vibrate_hook, #64), i.e. CMSoundPlayer::Vibrate while the game's
//               vibration option is on: every hit the hero takes, the options menu's preview,
//               22 scene steps. The phone's vibration is one fixed buzz (the game's
//               milliseconds are ignored there too), so the rumble is one fixed effect.
//   hero damage the runtime's MT_GAME_EVENT_HERO_DAMAGE (maplethief/events.h), whatever the
//               game's option says
//   level-up    MT_GAME_EVENT_LEVEL_UP
//
// The requests of one frontend turn are merged (the strongest amplitude per motor, the longest
// duration) and sent as one SDL_RumbleGamepad at the next turn() (a hit with the game's
// vibration on is both a vibrate and a damage request: one rumble). Amplitudes are scaled by
// rumble_strength / 100. Nothing is sent while rumble is off, at strength 0, or without an
// active pad; a pad without rumble just answers false (counted).
//
// settings.ini: `rumble = on | off` (default on), `rumble_strength = 0..100` (default 100);
// the command line: --rumble on|off, --rumble-strength N.
//
// Single thread: the hooks run on the guest thread, which is the frontend's.
#ifndef MAPLETHIEF_PLATFORM_RUMBLE_H
#define MAPLETHIEF_PLATFORM_RUMBLE_H

#include <SDL3/SDL.h>

#include <cstdint>

#include "maplethief/events.h"

namespace maplethief::frontend {

class Gamepads;

// One rumble: each motor's amplitude 0..1 (SDL's low frequency, the big motor; high
// frequency, the small one) for `ms` milliseconds.
struct RumbleEffect {
    float low = 0;
    float high = 0;
    std::uint32_t ms = 0;
};

inline constexpr RumbleEffect kRumbleVibrate{0.70f, 0.70f, 400}; // the phone's buzz
inline constexpr RumbleEffect kRumbleDamage{0.60f, 0.30f, 180};  // a hit
inline constexpr RumbleEffect kRumbleLevelUp{0.35f, 0.85f, 700}; // a level-up

struct RumbleStats {
    std::uint64_t requests = 0; // vibrate, damage and level-up requests while on
    std::uint64_t ignored = 0;  // requests while off or at strength 0
    std::uint64_t sent = 0;     // SDL_RumbleGamepad calls that started a rumble
    std::uint64_t no_pad = 0;   // turns with requests and no active pad
    std::uint64_t failed = 0;   // SDL_RumbleGamepad answered false (no rumble on that pad)
};

class Rumble {
  public:
    explicit Rumble(const Gamepads *pads) : pads_(pads) {}
    ~Rumble();
    Rumble(const Rumble &) = delete;
    Rumble &operator=(const Rumble &) = delete;

    // rumble and rumble_strength (clamped to 0..100). Off drops what is pending.
    void configure(bool on, std::uint32_t strength);
    bool on() const { return on_; }
    std::uint32_t strength() const { return strength_; }

    // Installs the runtime's vibrate and game-event hooks (to this object), or removes them.
    void install();
    void uninstall();

    // A request (the hooks call these): merged until the next turn().
    void request(const RumbleEffect &e);
    void on_vibrate() { request(kRumbleVibrate); }
    void on_event(const mt_game_event &e);

    // Sends what was requested since the last turn to the active pad.
    void turn();
    // Stops the pad's rumble now (focus lost, shut down).
    void stop();

    const RumbleStats &stats() const { return stats_; }

  private:
    const Gamepads *pads_;
    bool on_ = true;
    std::uint32_t strength_ = 100;
    bool pending_ = false;
    RumbleEffect merged_;
    SDL_JoystickID rumbling_ = 0; // the pad the last rumble went to (it may be gone by now)
    bool installed_ = false;
    RumbleStats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RUMBLE_H
