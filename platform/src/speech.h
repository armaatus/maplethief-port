// Text-to-speech (#390, ADR 0174, docs/re/speech.md): the game's text read aloud through the
// system voice, as an accessibility option. This is the pipe; what feeds it (dialogue #391,
// notices #392, menus #393) calls Speech::say with a kind and UTF-8 text.
//
//   speech = off | on        (settings.h; Accessibility, off, in no preset, live)
//   speech_rate = 50..200    percent of the voice's normal rate
//   speech_volume = 0..100   percent
//   MAPLETHIEF_SPEECH = system | log | none   which backend (default system; log writes each
//                            utterance to the log instead of speaking, for tests and runs)
//
// The queue rules are here, not in the backends, so that every platform keeps them alike:
//   dialogue, menu   interrupt: what is being spoken stops and this is spoken at once
//                    (a new dialogue page, the focus on a new row)
//   notice           queue: spoken after what is being spoken; past kQueueLimit waiting, the
//                    oldest waiting one is dropped (a run of pick-ups is not read for a minute)
//   speech = off     nothing reaches a backend; turning it off stops the voice and empties the
//                    queue
// A backend says when it has finished, or Speech estimates it from the text and the rate
// (speech_seconds): the log backend and Speech Dispatcher cannot tell.
//
// Backends (open_system_speech_backend): AVSpeechSynthesizer on macOS and iOS (speech_apple.mm;
// on iOS it mixes with the game's audio session), SAPI 5 on Windows (speech_sapi.cpp), Speech
// Dispatcher's libspeechd on Linux, loaded at run time (no build dependency; without it the
// settings row says the voice is not available), none on the Switch and Android (kCapSpeech,
// platform_caps.h).
//
// Single-threaded: the run loop's thread calls everything.
#ifndef MAPLETHIEF_PLATFORM_SPEECH_H
#define MAPLETHIEF_PLATFORM_SPEECH_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "settings.h"

namespace maplethief::frontend {

// What an utterance is, which decides how it queues (speech_priority).
enum class SpeechKind { kDialogue, kNotice, kMenu };
// "dialogue", "notice", "menu".
const char *speech_kind_name(SpeechKind kind);

enum class SpeechPriority { kQueue, kInterrupt };
constexpr SpeechPriority speech_priority(SpeechKind kind) {
    return kind == SpeechKind::kNotice ? SpeechPriority::kQueue : SpeechPriority::kInterrupt;
}

struct Utterance {
    SpeechKind kind = SpeechKind::kDialogue;
    std::string text; // UTF-8
};

// How the voice speaks.
struct SpeechVoice {
    std::string language = "en-US";          // a BCP 47 tag (speech_language)
    std::uint32_t rate = kSpeechRateDefault; // percent of the voice's normal rate, 50 to 200
    std::uint32_t volume = kSpeechVolumeMax; // percent, 0 to 100
};

// The voice's language: the open text pack's manifest `language` (a tag such as "fr" or
// "pt-BR"; null or not a tag: none), else the game's ("ko": Korean, else English).
std::string speech_language(std::string_view game_language, const char *pack_language);
// The voice for `settings`' rate and volume, in `language`.
SpeechVoice speech_voice(const Settings &settings, std::string language);

// The time Speech runs on in the game: the host's clock in seconds (SDL_GetTicksNS), since a voice
// speaks in real time whatever the game's speed or pauses.
double speech_clock();

// About how long `text` takes to say at `rate` percent, in seconds: for backends that cannot
// tell when they are done. Hangul and CJK at about 6.5 a second, anything else 14 characters a
// second (the system voices' normal rates), plus a quarter second.
double speech_seconds(std::string_view text, std::uint32_t rate);

// One system voice (or the log). Everything is called from the run loop's thread.
class SpeechBackend {
  public:
    virtual ~SpeechBackend() = default;
    // "avspeech", "sapi", "speechd", "log".
    virtual const char *name() const = 0;
    // Starts speaking `u` (its text is never empty), cutting off whatever it says now. False if
    // it could not; nothing is spoken then.
    virtual bool speak(const Utterance &u, const SpeechVoice &voice) = 0;
    // Stops at once.
    virtual void stop() = 0;
    // Whether the last speak is still being spoken; nullopt if this backend cannot tell (Speech
    // then estimates it with speech_seconds).
    virtual std::optional<bool> speaking() const = 0;
};

// The log backend (MAPLETHIEF_SPEECH=log): each utterance goes to `sink` (SDL_Log's "speech:"
// line by default) and is kept, and nothing is heard.
class LogSpeechBackend final : public SpeechBackend {
  public:
    using Sink = std::function<void(const std::string &line)>;
    LogSpeechBackend();
    explicit LogSpeechBackend(Sink sink);
    const char *name() const override { return "log"; }
    bool speak(const Utterance &u, const SpeechVoice &voice) override;
    void stop() override;
    std::optional<bool> speaking() const override { return std::nullopt; }
    // What it was asked to say, in order, and how many stops it was asked for.
    const std::vector<Utterance> &said() const { return said_; }
    std::size_t stops() const { return stops_; }

  private:
    Sink sink_;
    std::vector<Utterance> said_;
    std::size_t stops_ = 0;
};

enum class SpeechBackendChoice { kSystem, kLog, kNone };
// MAPLETHIEF_SPEECH's value (null or empty: system); nullopt if it is none of the three.
std::optional<SpeechBackendChoice> parse_speech_backend(const char *value);
// The platform's voice, or null where it has none (the Switch, Android) or it does not load
// (Linux without libspeechd).
std::unique_ptr<SpeechBackend> open_system_speech_backend();
// Speech Dispatcher from the shared library `soname`, or null if it does not load or lacks a
// function. The connection to the daemon opens at the first speak. Linux's system backend; the
// tests load a name that does not exist.
std::unique_ptr<SpeechBackend> open_speechd_backend(const char *soname = "libspeechd.so.2");
std::unique_ptr<SpeechBackend> open_speech_backend(SpeechBackendChoice choice);

struct SpeechStats {
    std::uint64_t said = 0;        // say() calls while on
    std::uint64_t spoken = 0;      // utterances a backend started
    std::uint64_t interrupted = 0; // cut off by an interrupting one
    std::uint64_t dropped = 0;     // queued notices dropped past the limit
    std::uint64_t ignored = 0;     // say() while off, without a backend, or with no text
    std::uint64_t failed = 0;      // the backend refused to start one
};

// The front the game talks to: the queue rules above over one backend.
class Speech {
  public:
    // Notices waiting behind the one being spoken; past it the oldest is dropped.
    static constexpr std::size_t kQueueLimit = 4;

    explicit Speech(std::unique_ptr<SpeechBackend> backend = nullptr);
    void set_backend(std::unique_ptr<SpeechBackend> backend);
    // Whether there is a voice to speak with (the settings row says "not available" if not).
    bool available() const { return backend_ != nullptr; }
    const char *backend_name() const { return backend_ ? backend_->name() : "none"; }

    // speech = on | off. Off stops the voice and empties the queue.
    void set_enabled(bool on);
    bool enabled() const { return enabled_; }
    // The voice of the next utterance (rate, volume, language); what is speaking now goes on.
    void set_voice(SpeechVoice voice) { voice_ = std::move(voice); }
    const SpeechVoice &voice() const { return voice_; }

    // Says `text` (UTF-8) by the rules above, at run-loop time `now` (seconds).
    void say(SpeechKind kind, std::string text, double now);
    // Once per run-loop turn: the next queued utterance starts when the last one is done.
    void turn(double now);
    // Stops the voice and empties the queue.
    void stop();

    // Whether an utterance is being spoken (as far as Speech knows at `now`).
    bool speaking(double now) const;
    std::size_t queued() const { return queue_.size(); }
    const SpeechStats &stats() const { return stats_; }

  private:
    void start(Utterance u, double now);

    std::unique_ptr<SpeechBackend> backend_;
    bool enabled_ = false;
    SpeechVoice voice_;
    std::deque<Utterance> queue_;
    bool active_ = false; // an utterance was started and may still be speaking
    double estimate_ = 0; // when it ends, for a backend that cannot tell
    SpeechStats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SPEECH_H
