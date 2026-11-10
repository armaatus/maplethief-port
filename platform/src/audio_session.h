// The host's audio session on iOS (#197, ADR 0045's addendum): the category the game's sound
// plays in, and what happens when a call, Siri or an alarm interrupts it.
//
//   ios_audio = original | own   (settings.h; iOS only, default original, next start)
//     original   SDL's "ambient": AVAudioSessionCategoryAmbient, as the original asks for. It
//                mixes with other apps' music and the Silent switch quiets it.
//     own        SDL's "playback": AVAudioSessionCategoryPlayback. It plays with the switch on
//                silent. SDL 3.4.18 adds DuckOthers, so other apps' music is turned down rather
//                than stopped.
//   Without the hint SDL picks Playback with MixWithOthers and DuckOthers, whatever its header
//   says, so the switch would not quiet the game.
//
// SDL owns the session: it reads the hint when a device opens, sets the category and activates
// the session, and sets it again at each open if it differs. So the category only ever goes
// through the hint, never through setCategory: here.
//
// Interruptions. SDL pauses its audio queue when one begins and starts it again when it ends or
// when the app becomes active (an end is not guaranteed). It never activates the session again,
// though the system deactivated it. AudioInterruption adds that, and pauses the stream, so the
// mixer is not pulled and the music keeps its place:
//   begin                 the stream pauses (once), logged
//   end, or the app       the session is activated, then the stream resumes, logged. If the
//   becoming active       session refuses (the call goes on) the stream stays paused and the
//                         next end or activation tries again
// The observer is registered before SDL opens its device, so it runs before SDL's own (the
// notification centre calls observers in the order they registered, in practice), and SDL's
// AudioQueueStart finds the session active.
//
// Thread-safe: iOS does not promise which thread posts these notifications.
#ifndef MAPLETHIEF_PLATFORM_AUDIO_SESSION_H
#define MAPLETHIEF_PLATFORM_AUDIO_SESSION_H

#include <cstdint>
#include <memory>
#include <mutex>

#include "settings.h"

namespace maplethief::frontend {

// The value of SDL_HINT_AUDIO_CATEGORY for `mode`: "ambient" or "playback".
const char *ios_audio_category(IosAudio mode);

// Where the category goes: SDL's hint in the game (set_sdl_audio_category), a recorder in tests.
using AudioCategorySetter = void (*)(void *user, const char *category);
void set_sdl_audio_category(void *user, const char *category);

// Asks for `mode`'s category through `set` and returns it. Not `for_ios`: asks for nothing and
// returns nullptr (a desktop has no audio session). Call it before the device opens.
const char *request_ios_audio_category(IosAudio mode, bool for_ios, AudioCategorySetter set,
                                       void *user = nullptr);

// What an interruption acts on: AudioDevice in the game, a recorder in tests.
class AudioInterruptionHost {
  public:
    virtual ~AudioInterruptionHost() = default;
    // False if the session cannot be active yet.
    virtual bool activate_session() = 0;
    virtual void pause_output() = 0;
    virtual void resume_output() = 0;
};

struct AudioInterruptionStats {
    std::uint64_t began = 0;   // interruptions that paused the output
    std::uint64_t ended = 0;   // ... and that resumed it
    std::uint64_t refused = 0; // ends the session refused
};

// The decision: see the table above. Not copyable (it holds a mutex).
class AudioInterruption {
  public:
    explicit AudioInterruption(AudioInterruptionHost *host) : host_(host) {}

    // An interruption began. Another while one is under way changes nothing.
    void began();
    // It ended, or the app became active (`why`, for the log). Nothing if none is under way.
    // True if the output resumed.
    bool ended(const char *why);
    bool interrupted() const;
    AudioInterruptionStats stats() const;
    // The device closed: forgets an interruption under way, without touching the host.
    void reset();

  private:
    AudioInterruptionHost *const host_;
    mutable std::mutex mutex_;
    bool interrupted_ = false;
    AudioInterruptionStats stats_;
};

// The platform's session: on iOS it observes the interruptions and the app becoming active for
// `interruption` (not owned) until it is destroyed, and activates AVAudioSession.
class AudioSessionWatch {
  public:
    virtual ~AudioSessionWatch() = default;
    virtual bool activate() = 0;
};

// iOS's watch; nullptr elsewhere. Make it before the device opens (see above).
std::unique_ptr<AudioSessionWatch> make_platform_audio_session(AudioInterruption *interruption);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_AUDIO_SESSION_H
