// Text-to-speech (#390, ADR 0174): the queue rules, the log backend and Speech Dispatcher. See
// speech.h. AVSpeechSynthesizer is speech_apple.mm, SAPI speech_sapi.cpp.
#include "speech.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace maplethief::frontend {

#if defined(__APPLE__)
std::unique_ptr<SpeechBackend> open_apple_speech_backend(); // speech_apple.mm
#elif defined(_WIN32)
std::unique_ptr<SpeechBackend> open_sapi_speech_backend(); // speech_sapi.cpp
#endif

const char *speech_kind_name(SpeechKind kind) {
    switch (kind) {
    case SpeechKind::kDialogue:
        return "dialogue";
    case SpeechKind::kNotice:
        return "notice";
    case SpeechKind::kMenu:
        return "menu";
    }
    return "?";
}

namespace {

// A language tag as a text pack's manifest may give it ("fr", "pt-BR", "zh_Hant"): letters,
// digits, '-' and '_', starting with a letter, at most 35 characters (BCP 47's longest common
// form). '_' becomes '-'.
std::optional<std::string> language_tag(const char *text) {
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string out(text);
    if (out.empty() || out.size() > 35 ||
        !((out[0] >= 'a' && out[0] <= 'z') || (out[0] >= 'A' && out[0] <= 'Z'))) {
        return std::nullopt;
    }
    for (char &c : out) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return std::nullopt;
        }
        c = c == '_' ? '-' : c;
    }
    return out;
}

// Decodes one UTF-8 code point at text[i], moving i past it; a bad byte is one code point.
std::uint32_t next_code_point(std::string_view text, std::size_t &i) {
    const auto b = static_cast<unsigned char>(text[i++]);
    int more = 0;
    std::uint32_t cp = b;
    if (b >= 0xF0 && b < 0xF8) {
        more = 3;
        cp = b & 0x07u;
    } else if (b >= 0xE0) {
        more = b < 0xF0 ? 2 : 0;
        cp = more != 0 ? b & 0x0Fu : b;
    } else if (b >= 0xC0) {
        more = 1;
        cp = b & 0x1Fu;
    }
    for (; more > 0 && i < text.size(); more--) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c & 0xC0u) != 0x80u) {
            break;
        }
        cp = (cp << 6) | (c & 0x3Fu);
        i++;
    }
    return cp;
}

// Hangul, the CJK ideographs and kana: a voice says about one syllable per character.
bool syllabic(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3);
}

void log_line(const std::string &line) { SDL_Log("%s", line.c_str()); }

} // namespace

std::string speech_language(std::string_view game_language, const char *pack_language) {
    if (std::optional<std::string> tag = language_tag(pack_language)) {
        return *tag;
    }
    return game_language == "ko" ? "ko-KR" : "en-US";
}

SpeechVoice speech_voice(const Settings &settings, std::string language) {
    SpeechVoice v;
    v.language = std::move(language);
    v.rate = std::clamp(settings.speech_rate, kSpeechRateMin, kSpeechRateMax);
    v.volume = std::min(settings.speech_volume, kSpeechVolumeMax);
    return v;
}

double speech_clock() { return static_cast<double>(SDL_GetTicksNS()) / 1e9; }

double speech_seconds(std::string_view text, std::uint32_t rate) {
    double seconds = 0.25;
    for (std::size_t i = 0; i < text.size();) {
        seconds += syllabic(next_code_point(text, i)) ? 1.0 / 6.5 : 1.0 / 14.0;
    }
    const double r = std::clamp(rate, kSpeechRateMin, kSpeechRateMax) / 100.0;
    return seconds / r;
}

// The log backend.

LogSpeechBackend::LogSpeechBackend() : sink_(log_line) {}

LogSpeechBackend::LogSpeechBackend(Sink sink) : sink_(std::move(sink)) {}

bool LogSpeechBackend::speak(const Utterance &u, const SpeechVoice &voice) {
    said_.push_back(u);
    sink_("speech: " + std::string(speech_kind_name(u.kind)) + " [" + voice.language + " " +
          std::to_string(voice.rate) + "% " + std::to_string(voice.volume) + "%] " + u.text);
    return true;
}

void LogSpeechBackend::stop() {
    stops_++;
    sink_("speech: stop");
}

// Speech Dispatcher (libspeechd, C), loaded at run time: its ABI written out so that the build
// needs neither its headers nor its library (as update_fetch.cpp does with libcurl).

namespace {

constexpr int kSpdModeSingle = 0; // SPDConnectionMode
constexpr int kSpdText = 3;       // SPDPriority: SPD_TEXT

using SpdOpen = void *(*)(const char *client, const char *connection, const char *user, int mode);
using SpdClose = void (*)(void *);
using SpdSay = int (*)(void *, int priority, const char *text);
using SpdCancel = int (*)(void *);
using SpdSetInt = int (*)(void *, int value);
using SpdSetLanguage = int (*)(void *, const char *language);

// speechd's rate and volume run from -100 to 100, 0 the voice's normal: 50% -> -100, 200% ->
// 100; volume 0% -> -100, 100% -> 100.
int speechd_rate(std::uint32_t rate) {
    const int r = static_cast<int>(std::clamp(rate, kSpeechRateMin, kSpeechRateMax));
    return r >= 100 ? r - 100 : (r - 100) * 2;
}
int speechd_volume(std::uint32_t volume) {
    return static_cast<int>(std::min(volume, kSpeechVolumeMax)) * 2 - 100;
}

class SpeechdBackend final : public SpeechBackend {
  public:
    static std::unique_ptr<SpeechdBackend> load(const char *soname) {
        SDL_SharedObject *lib = SDL_LoadObject(soname);
        if (lib == nullptr) {
            SDL_Log("speech: %s does not load: no system voice (install speech-dispatcher)",
                    soname);
            return nullptr;
        }
        auto out = std::unique_ptr<SpeechdBackend>(new SpeechdBackend());
        out->open_ = reinterpret_cast<SpdOpen>(SDL_LoadFunction(lib, "spd_open"));
        out->close_ = reinterpret_cast<SpdClose>(SDL_LoadFunction(lib, "spd_close"));
        out->say_ = reinterpret_cast<SpdSay>(SDL_LoadFunction(lib, "spd_say"));
        out->cancel_ = reinterpret_cast<SpdCancel>(SDL_LoadFunction(lib, "spd_cancel"));
        out->rate_ = reinterpret_cast<SpdSetInt>(SDL_LoadFunction(lib, "spd_set_voice_rate"));
        out->volume_ = reinterpret_cast<SpdSetInt>(SDL_LoadFunction(lib, "spd_set_volume"));
        out->language_ =
            reinterpret_cast<SpdSetLanguage>(SDL_LoadFunction(lib, "spd_set_language"));
        if (out->open_ == nullptr || out->close_ == nullptr || out->say_ == nullptr ||
            out->cancel_ == nullptr || out->rate_ == nullptr || out->volume_ == nullptr ||
            out->language_ == nullptr) {
            SDL_Log("speech: %s lacks a function: no system voice", soname);
            SDL_UnloadObject(lib);
            return nullptr;
        }
        return out; // the library stays loaded for the process's life
    }

    ~SpeechdBackend() override {
        if (conn_ != nullptr) {
            close_(conn_);
        }
    }

    const char *name() const override { return "speechd"; }

    bool speak(const Utterance &u, const SpeechVoice &voice) override {
        if (!connect()) {
            return false;
        }
        cancel_(conn_);
        if (voice.language != language_set_) {
            language_(conn_, voice.language.c_str());
            language_set_ = voice.language;
        }
        rate_(conn_, speechd_rate(voice.rate));
        volume_(conn_, speechd_volume(voice.volume));
        return say_(conn_, kSpdText, u.text.c_str()) >= 0;
    }

    void stop() override {
        if (conn_ != nullptr) {
            cancel_(conn_);
        }
    }

    // speechd reports the end through a callback on its socket thread; the estimate does.
    std::optional<bool> speaking() const override { return std::nullopt; }

  private:
    SpeechdBackend() = default;

    // The daemon's connection, at the first speak (spd_open starts the daemon if it is not
    // running, which the game should not do unless speech is on). Tried once.
    bool connect() {
        if (conn_ == nullptr && !tried_) {
            tried_ = true;
            conn_ = open_("maplethief", "main", nullptr, kSpdModeSingle);
            if (conn_ == nullptr) {
                SDL_Log("speech: speech-dispatcher does not answer: nothing is spoken");
            }
        }
        return conn_ != nullptr;
    }

    SpdOpen open_ = nullptr;
    SpdClose close_ = nullptr;
    SpdSay say_ = nullptr;
    SpdCancel cancel_ = nullptr;
    SpdSetInt rate_ = nullptr;
    SpdSetInt volume_ = nullptr;
    SpdSetLanguage language_ = nullptr;
    void *conn_ = nullptr;
    bool tried_ = false;
    std::string language_set_;
};

} // namespace

std::unique_ptr<SpeechBackend> open_speechd_backend(const char *soname) {
    return SpeechdBackend::load(soname);
}

std::unique_ptr<SpeechBackend> open_system_speech_backend() {
#if defined(__APPLE__)
    return open_apple_speech_backend();
#elif defined(_WIN32)
    return open_sapi_speech_backend();
#elif defined(__linux__) && !defined(__ANDROID__)
    return open_speechd_backend();
#else
    return nullptr; // the Switch, Android: no voice a program can call (yet)
#endif
}

std::optional<SpeechBackendChoice> parse_speech_backend(const char *value) {
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, "system") == 0) {
        return SpeechBackendChoice::kSystem;
    }
    if (std::strcmp(value, "log") == 0) {
        return SpeechBackendChoice::kLog;
    }
    if (std::strcmp(value, "none") == 0) {
        return SpeechBackendChoice::kNone;
    }
    return std::nullopt;
}

std::unique_ptr<SpeechBackend> open_speech_backend(SpeechBackendChoice choice) {
    switch (choice) {
    case SpeechBackendChoice::kSystem:
        return open_system_speech_backend();
    case SpeechBackendChoice::kLog:
        return std::make_unique<LogSpeechBackend>();
    case SpeechBackendChoice::kNone:
        return nullptr;
    }
    return nullptr;
}

// The queue.

Speech::Speech(std::unique_ptr<SpeechBackend> backend) : backend_(std::move(backend)) {}

void Speech::set_backend(std::unique_ptr<SpeechBackend> backend) {
    stop();
    backend_ = std::move(backend);
}

void Speech::set_enabled(bool on) {
    if (enabled_ && !on) {
        stop();
    }
    enabled_ = on;
}

bool Speech::speaking(double now) const {
    if (!active_ || backend_ == nullptr) {
        return false;
    }
    const std::optional<bool> s = backend_->speaking();
    return s ? *s : now < estimate_;
}

void Speech::start(Utterance u, double now) {
    if (backend_->speak(u, voice_)) {
        stats_.spoken++;
        active_ = true;
        estimate_ = now + speech_seconds(u.text, voice_.rate);
    } else {
        stats_.failed++;
        active_ = false;
    }
}

void Speech::say(SpeechKind kind, std::string text, double now) {
    if (!enabled_ || backend_ == nullptr || text.empty()) {
        stats_.ignored++;
        return;
    }
    stats_.said++;
    Utterance u{kind, std::move(text)};
    if (speech_priority(kind) == SpeechPriority::kInterrupt) {
        if (speaking(now)) {
            stats_.interrupted++;
            backend_->stop();
        }
        start(std::move(u), now); // queued notices follow it
        return;
    }
    if (!speaking(now) && queue_.empty()) {
        start(std::move(u), now);
        return;
    }
    queue_.push_back(std::move(u));
    while (queue_.size() > kQueueLimit) {
        queue_.pop_front();
        stats_.dropped++;
    }
}

void Speech::turn(double now) {
    if (!enabled_ || backend_ == nullptr || queue_.empty() || speaking(now)) {
        return;
    }
    Utterance next = std::move(queue_.front());
    queue_.pop_front();
    start(std::move(next), now);
}

void Speech::stop() {
    queue_.clear();
    if (active_ && backend_ != nullptr) {
        backend_->stop();
    }
    active_ = false;
}

} // namespace maplethief::frontend
