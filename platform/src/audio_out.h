// The audio output (#61, ADR 0045): the runtime's mixer (maplethief/audio.h) pulled by one SDL3
// device stream, or rendered into a WAV capture.
//
//   AudioDevice   SDL_INIT_AUDIO and SDL_OpenAudioDeviceStream(default playback, F32 / 2 ch /
//                 44,100 Hz) with a callback that renders exactly the frames SDL asks for with
//                 mt_audio_render; SDL converts to the device's own format. Opened paused,
//                 resumed once the game runs. No device (none there, SDL_AUDIO_DRIVER=dummy
//                 fails, --no-audio) is not an error: the game runs silent. Late callbacks
//                 (LateDetector) go to mt_audio_note_late.
//   AudioCapture  --audio-capture FILE.wav: 16-bit stereo 44,100 Hz PCM. render_to(t) renders
//                 the frames up to t nanoseconds of audio (floor(t * 44,100 / 10^9) in all, so
//                 the remainder of each step is carried), so under the virtual clock
//                 (--fixed-step) a scripted run writes the same bytes every time and on every
//                 OS. A capture replaces the device: the mixer has one reader.
//   the settings  (#64) apply_audio_settings sets the mixer's buses from settings.ini's
//                 volumes (percent: gain = volume / 100); FocusMute mutes the whole output
//                 while the window is unfocused (mute_unfocused).
#ifndef MAPLETHIEF_PLATFORM_AUDIO_OUT_H
#define MAPLETHIEF_PLATFORM_AUDIO_OUT_H

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "settings.h"

struct SDL_AudioStream;

namespace maplethief::frontend {

// The frames of MT_AUDIO_RATE audio in the first `elapsed_ns` nanoseconds:
// floor(elapsed_ns * 44,100 / 10^9).
std::uint64_t audio_frames_at(std::uint64_t elapsed_ns);

// Decides whether a device callback came late (ADR 0045): when the time since the previous
// callback exceeds 1.5 x the duration of the audio delivered then. Callbacks less than 1 ms
// apart count as one delivery (SDL's converter can ask twice in a row).
class LateDetector {
  public:
    // A callback at `now_ns` delivering `frames` frames; returns true if it came late.
    bool on_callback(std::uint64_t now_ns, std::uint32_t frames);
    std::uint64_t late() const { return late_; }

  private:
    std::uint64_t last_ns_ = 0;
    std::uint64_t delivered_ = 0; // frames of the last delivery
    bool started_ = false;
    std::uint64_t late_ = 0;
};

class AudioDevice {
  public:
    AudioDevice() = default;
    AudioDevice(const AudioDevice &) = delete;
    AudioDevice &operator=(const AudioDevice &) = delete;
    ~AudioDevice() { close(); }

    // Initialises SDL's audio and opens the default playback device's stream, paused. Returns
    // false (logged: "audio: no output device ...") if there is none.
    bool open();
    // Starts pulling the mixer.
    void resume();
    // Stops the callback and closes the device (and SDL's audio subsystem).
    void close();
    bool is_open() const { return stream_ != nullptr; }

  private:
    static void callback(void *user, SDL_AudioStream *stream, int additional, int total);

    SDL_AudioStream *stream_ = nullptr;
    bool subsystem_ = false;
    // The callback's (one thread at a time).
    std::vector<float> scratch_;
    LateDetector late_;
};

class AudioCapture {
  public:
    AudioCapture() = default;
    AudioCapture(const AudioCapture &) = delete;
    AudioCapture &operator=(const AudioCapture &) = delete;
    ~AudioCapture() { close(); }

    // Creates `path` with a WAV header (sizes filled in by close()). False (logged) on failure.
    bool open(const std::string &path);
    // Renders and writes the frames up to audio_frames_at(elapsed_ns) (none if it is behind).
    // False once a write failed.
    bool render_to(std::uint64_t elapsed_ns);
    // Fills in the header's sizes and closes the file. False if a write failed.
    bool close();
    bool is_open() const { return out_.is_open(); }
    std::uint64_t frames() const { return frames_; }
    const std::string &path() const { return path_; }

  private:
    std::ofstream out_;
    std::string path_;
    std::uint64_t frames_ = 0;
    bool failed_ = false;
    std::vector<float> scratch_;
    std::vector<std::uint8_t> bytes_;
};

// The gain of a bus at `volume` percent (0 to kMaxVolume, more is clamped): volume / 100.
float volume_gain(std::uint32_t volume);

// Sets the mixer's master, music and effects buses (mt_audio_set_gain) from the settings.
void apply_audio_settings(const Settings &settings);

// Mutes the output while the window is unfocused (mute_unfocused, #64): SDL's
// SDL_EVENT_WINDOW_FOCUS_LOST mutes it (mt_audio_set_muted), SDL_EVENT_WINDOW_FOCUS_GAINED
// unmutes it; other events change nothing. Disabled, it never mutes. The output starts unmuted
// (a window that never had focus is not muted until SDL reports a loss).
class FocusMute {
  public:
    explicit FocusMute(bool enabled = false) : enabled_(enabled) {}
    // One SDL event's type (SDL_Event::type). Returns true if it changed the mute.
    bool on_event(std::uint32_t event_type);
    bool enabled() const { return enabled_; }
    bool muted() const { return muted_; }

  private:
    bool enabled_;
    bool muted_ = false;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_AUDIO_OUT_H
