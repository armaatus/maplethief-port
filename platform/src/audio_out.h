// The audio output (#61, ADR 0045): the runtime's mixer (maplethief/audio.h) pulled by one SDL3
// device stream, or rendered into a WAV capture.
//
//   AudioDevice   SDL_INIT_AUDIO and SDL_OpenAudioDeviceStream(the playback device audio_device
//                 names, else the default; F32 / 2 ch / 44,100 Hz) with a callback that renders
//                 exactly the frames SDL asks for with mt_audio_render; SDL converts to the
//                 device's own format. Opened paused, resumed once the game runs. No device (none
//                 there, SDL_AUDIO_DRIVER=dummy fails, --no-audio) is not an error: the game runs
//                 silent. Late callbacks (LateDetector) go to mt_audio_note_late. audio_device
//                 (#195): a name that is not among SDL's playback devices plays on the default
//                 (logged), the setting unchanged; when SDL reports that device added, the stream
//                 moves to it, and when the chosen device is removed, back to the default. The
//                 menu's change reopens the stream at once (set_device). On iOS (#197,
//                 audio_session.h) it asks for ios_audio's category before the device opens, and
//                 an interruption pauses the stream until the session is active again.
//   AudioCapture  --audio-capture FILE.wav: 16-bit stereo 44,100 Hz PCM. render_to(t) renders
//                 the frames up to t nanoseconds of audio (floor(t * 44,100 / 10^9) in all, so
//                 the remainder of each step is carried), so under the virtual clock
//                 (--fixed-step) a scripted run writes the same bytes every time and on every
//                 OS. A capture replaces the device: the mixer has one reader.
//   the settings  (#64) apply_audio_settings sets the mixer's buses from settings.ini's
//                 volumes (percent: gain = volume / 100) and audio_mono (#195); FocusMute mutes the
//                 whole output while the window is unfocused (mute_unfocused).
#ifndef MAPLETHIEF_PLATFORM_AUDIO_OUT_H
#define MAPLETHIEF_PLATFORM_AUDIO_OUT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "audio_session.h"
#include "settings.h"
#include "thread_priority.h"

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

// A playback device as SDL lists it (SDL_GetAudioPlaybackDevices, SDL_GetAudioDeviceName).
struct AudioDeviceEntry {
    std::uint32_t id = 0; // SDL_AudioDeviceID
    std::string name;
};

// The index in `devices` of the device audio_device `wanted` names (Settings::audio_device:
// empty is the default), or -1 for the system's default: the setting is empty, or no device
// has that name (exactly; else ASCII case-insensitively), which the caller logs.
int pick_audio_device(std::string_view wanted, std::span<const AudioDeviceEntry> devices);

class AudioDevice final : private AudioInterruptionHost {
  public:
    AudioDevice() = default;
    AudioDevice(const AudioDevice &) = delete;
    AudioDevice &operator=(const AudioDevice &) = delete;
    ~AudioDevice() { close(); }

    // Initialises SDL's audio and opens the stream of the playback device `wanted` names
    // (pick_audio_device; else the default), paused. Returns false (logged: "audio: no output
    // device ...") if there is none. `session` is ios_audio's category (iOS only: #197).
    bool open(const std::string &wanted = {}, IosAudio session = IosAudio::kOriginal);
    // Starts pulling the mixer.
    void resume();
    // audio_device changed while the game runs: the stream reopens on `wanted` (or the default)
    // and plays on if it played. False if no stream is open (the next start applies it).
    bool set_device(const std::string &wanted);
    // SDL_EVENT_AUDIO_DEVICE_ADDED / _REMOVED (`which`: the device): refreshes the menu's list
    // and moves the stream to the wanted device that came back, or off the one that went.
    // Other events: nothing. True if the stream moved.
    bool on_device_event(std::uint32_t event_type, std::uint32_t which);
    // The device the stream plays on (0: the default), and whether `wanted` was missing.
    std::uint32_t device() const { return device_; }
    bool fell_back() const { return fell_back_; }
    // Stops the callback and closes the device (and SDL's audio subsystem).
    void close();
    bool is_open() const { return stream_ != nullptr; }
    // Interruptions (audio_session.h): iOS's watch drives it, tests call it directly.
    AudioInterruption &interruption() { return interruption_; }
    // The scheduling of the thread SDL first called the callback on (#206, ADR 0120: it must
    // stay above the main thread's raised priority), or unknown before the first callback.
    ThreadPriority callback_priority() const {
        return callback_seen_.load(std::memory_order_acquire) ? callback_priority_
                                                              : ThreadPriority{};
    }

  private:
    static void callback(void *user, SDL_AudioStream *stream, int additional, int total);
    // On wanted_ among `devices` (or the default), paused; false if SDL opens none.
    bool open_stream(const std::vector<AudioDeviceEntry> &devices);
    bool reopen(const std::vector<AudioDeviceEntry> &devices); // and resumed if it was
    void close_stream();
    // AudioInterruptionHost: on whichever thread iOS posts the notification.
    bool activate_session() override;
    void pause_output() override;
    void resume_output() override;

    // stream_mutex_ guards the stream against that thread: stream_, resumed_, output_paused_.
    // Never held while SDL opens or destroys a stream.
    std::mutex stream_mutex_;
    SDL_AudioStream *stream_ = nullptr;
    bool subsystem_ = false;
    bool resumed_ = false;
    bool output_paused_ = false; // an interruption is under way
    std::unique_ptr<AudioSessionWatch> session_;
    AudioInterruption interruption_{this};
    std::string wanted_;       // the setting
    std::uint32_t device_ = 0; // what the stream opened (0: the default)
    bool fell_back_ = false;   // wanted_ was named but not there
    // The callback's (one thread at a time).
    std::vector<float> scratch_;
    LateDetector late_;
    ThreadPriority callback_priority_; // written once, before callback_seen_
    std::atomic<bool> callback_seen_{false};
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

// The mixer's output hashed instead of written (#91, ADR 0104: the regression run's
// checkpoints): render_to(t) renders exactly what AudioCapture would write up to t (the same
// frames, the same 16-bit bytes) and folds the bytes into a 64-bit FNV-1a, so the hash of a run
// is the hash of its capture's data chunk. Like a capture it replaces the device (the mixer has
// one reader).
class AudioHash {
  public:
    static constexpr std::uint64_t kStart = 14695981039346656037ull; // FNV-1a's offset basis
    // FNV-1a over `n` bytes, from `h`.
    static std::uint64_t fnv(std::uint64_t h, const std::uint8_t *p, std::size_t n);

    void render_to(std::uint64_t elapsed_ns);
    std::uint64_t hash() const { return hash_; }
    std::uint64_t frames() const { return frames_; }

  private:
    std::uint64_t hash_ = kStart;
    std::uint64_t frames_ = 0;
    std::vector<float> scratch_;
    std::vector<std::uint8_t> bytes_;
};

// The gain of a bus at `volume` percent (0 to kMaxVolume, more is clamped): volume / 100.
float volume_gain(std::uint32_t volume);

// Sets the mixer's master, music and effects buses (mt_audio_set_gain) and gapless music
// (mt_audio_set_gapless_music, ADR 0062: before the game opens its players) from the settings.
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
    // mute_unfocused changed while the game runs (the settings menu, #89): off unmutes now.
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }
    bool muted() const { return muted_; }

  private:
    bool enabled_;
    bool muted_ = false;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_AUDIO_OUT_H
