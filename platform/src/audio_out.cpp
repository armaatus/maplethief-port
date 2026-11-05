// The audio output (#61, ADR 0045); see audio_out.h.
#include "audio_out.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

#include "maplethief/audio.h"

namespace maplethief::frontend {

namespace {

constexpr std::uint32_t kBlock = 4096;      // frames rendered per mt_audio_render call
constexpr std::uint64_t kBurstNs = 1000000; // callbacks closer than this are one delivery
constexpr std::uint32_t kWavHeaderBytes = 44;

std::filesystem::path utf8_path(const std::string &s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

void put16(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t *p, std::uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

// The 44-byte header of a 16-bit PCM WAV at MT_AUDIO_RATE / MT_AUDIO_CHANNELS holding
// `frames` frames (sizes clamped to RIFF's 32 bits).
void wav_header(std::uint8_t *h, std::uint64_t frames) {
    const std::uint32_t align = MT_AUDIO_CHANNELS * 2;
    const std::uint64_t data = std::min<std::uint64_t>(frames * align, 0xFFFFFFFFu - 36);
    std::copy_n("RIFF", 4, h);
    put32(h + 4, static_cast<std::uint32_t>(36 + data));
    std::copy_n("WAVEfmt ", 8, h + 8);
    put32(h + 16, 16);
    put16(h + 20, 1); // PCM
    put16(h + 22, MT_AUDIO_CHANNELS);
    put32(h + 24, MT_AUDIO_RATE);
    put32(h + 28, MT_AUDIO_RATE * align);
    put16(h + 32, align);
    put16(h + 34, 16);
    std::copy_n("data", 4, h + 36);
    put32(h + 40, static_cast<std::uint32_t>(data));
}

bool same_ascii_nocase(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

// SDL's playback devices now but `gone` (one being removed, which SDL may still list); the
// settings menu's list follows them (#195).
std::vector<AudioDeviceEntry> playback_devices(SDL_AudioDeviceID gone = 0) {
    std::vector<AudioDeviceEntry> out;
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; ids != nullptr && i < n; i++) {
        if (gone != 0 && ids[i] == gone) {
            continue;
        }
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        out.push_back({ids[i], name != nullptr ? name : ""});
    }
    SDL_free(ids);
    std::vector<std::string> names;
    for (const AudioDeviceEntry &d : out) {
        names.push_back(d.name);
    }
    set_audio_device_choices(std::move(names));
    return out;
}

} // namespace

std::uint64_t audio_frames_at(std::uint64_t elapsed_ns) {
    // elapsed_ns * 44,100 overflows 64 bits after about 4.8 days; split it.
    constexpr std::uint64_t kNs = 1000000000ull;
    return elapsed_ns / kNs * MT_AUDIO_RATE + elapsed_ns % kNs * MT_AUDIO_RATE / kNs;
}

// --- LateDetector ------------------------------------------------------------------------------

bool LateDetector::on_callback(std::uint64_t now_ns, std::uint32_t frames) {
    if (frames == 0) {
        return false;
    }
    if (started_ && now_ns - last_ns_ < kBurstNs) {
        delivered_ += frames; // the same delivery
        last_ns_ = now_ns;
        return false;
    }
    bool late = false;
    if (started_) {
        // 1.5 x the duration of the previous delivery, in nanoseconds.
        const std::uint64_t allowed = delivered_ * 1500000000ull / MT_AUDIO_RATE;
        late = now_ns - last_ns_ > allowed;
    }
    started_ = true;
    last_ns_ = now_ns;
    delivered_ = frames;
    late_ += late ? 1u : 0u;
    return late;
}

// --- AudioDevice -------------------------------------------------------------------------------

int pick_audio_device(std::string_view wanted, std::span<const AudioDeviceEntry> devices) {
    if (wanted.empty() || same_ascii_nocase(wanted, kAudioDeviceDefault)) {
        return -1;
    }
    for (const bool exact : {true, false}) {
        for (std::size_t i = 0; i < devices.size(); i++) {
            if (exact ? devices[i].name == wanted : same_ascii_nocase(devices[i].name, wanted)) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

bool AudioDevice::open(const std::string &wanted) {
    if (stream_ != nullptr) {
        return true;
    }
    wanted_ = wanted;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio: no output device (SDL audio: %s): silent", SDL_GetError());
        return false;
    }
    subsystem_ = true;
    if (!open_stream(playback_devices())) {
        close();
        return false;
    }
    return true;
}

bool AudioDevice::open_stream(const std::vector<AudioDeviceEntry> &devices) {
    const int i = pick_audio_device(wanted_, devices);
    fell_back_ = !wanted_.empty() && i < 0 && !same_ascii_nocase(wanted_, kAudioDeviceDefault);
    if (fell_back_) {
        SDL_Log("audio: the output device \"%s\" is not there: the default device (audio_device "
                "kept for when it is back)",
                wanted_.c_str());
    }
    device_ = i < 0 ? 0 : devices[static_cast<std::size_t>(i)].id;
    late_ = LateDetector{}; // a new stream's first callback is not late
    const SDL_AudioSpec spec = {SDL_AUDIO_F32, static_cast<int>(MT_AUDIO_CHANNELS),
                                static_cast<int>(MT_AUDIO_RATE)};
    stream_ = SDL_OpenAudioDeviceStream(device_ != 0 ? device_ : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec, callback, this);
    if (stream_ == nullptr && device_ != 0) {
        SDL_Log("audio: cannot open \"%s\" (%s): the default device", wanted_.c_str(),
                SDL_GetError());
        device_ = 0;
        fell_back_ = true;
        stream_ =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
    }
    if (stream_ == nullptr) {
        SDL_Log("audio: no output device (driver %s: %s): silent",
                SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "none",
                SDL_GetError());
        return false;
    }
    SDL_AudioSpec device{};
    int frames = 0;
    const SDL_AudioDeviceID id = SDL_GetAudioStreamDevice(stream_);
    SDL_GetAudioDeviceFormat(id, &device, &frames);
    const char *name = SDL_GetAudioDeviceName(id);
    SDL_Log("audio: driver %s, device \"%s\": %d Hz, %d channels, format 0x%X, %d-frame buffer; "
            "the mixer renders %u Hz stereo float",
            SDL_GetCurrentAudioDriver(), name != nullptr ? name : "?", device.freq, device.channels,
            static_cast<unsigned>(device.format), frames, MT_AUDIO_RATE);
    return true;
}

void AudioDevice::resume() {
    if (stream_ != nullptr && !SDL_ResumeAudioStreamDevice(stream_)) {
        SDL_Log("audio: cannot resume the device: %s", SDL_GetError());
    }
    resumed_ = stream_ != nullptr;
}

bool AudioDevice::reopen(const std::vector<AudioDeviceEntry> &devices) {
    const bool resumed = resumed_;
    close_stream();
    if (!open_stream(devices)) {
        return false;
    }
    if (resumed) {
        resume();
    }
    return true;
}

bool AudioDevice::set_device(const std::string &wanted) {
    wanted_ = wanted;
    return stream_ != nullptr && reopen(playback_devices());
}

bool AudioDevice::on_device_event(std::uint32_t event_type, std::uint32_t which) {
    if ((event_type != SDL_EVENT_AUDIO_DEVICE_ADDED &&
         event_type != SDL_EVENT_AUDIO_DEVICE_REMOVED) ||
        !subsystem_) {
        return false;
    }
    const bool removed = event_type == SDL_EVENT_AUDIO_DEVICE_REMOVED;
    const std::vector<AudioDeviceEntry> devices = playback_devices(removed ? which : 0);
    if (stream_ == nullptr) {
        return false;
    }
    if (!removed && fell_back_ && pick_audio_device(wanted_, devices) >= 0) {
        SDL_Log("audio: the output device \"%s\" is back", wanted_.c_str());
        return reopen(devices);
    }
    if (removed && device_ != 0 && which == device_) {
        SDL_Log("audio: the output device \"%s\" went away", wanted_.c_str());
        return reopen(devices); // the default, until it is back
    }
    return false;
}

void AudioDevice::close_stream() {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_); // also closes the device; the callback has returned
        stream_ = nullptr;
    }
    resumed_ = false;
}

void AudioDevice::close() {
    close_stream();
    if (subsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        subsystem_ = false;
    }
}

// SDL's audio thread: exactly the frames SDL asks for.
void AudioDevice::callback(void *user, SDL_AudioStream *stream, int additional, int) {
    auto &self = *static_cast<AudioDevice *>(user);
    const int frame_bytes = static_cast<int>(MT_AUDIO_CHANNELS * sizeof(float));
    if (additional <= 0) {
        return;
    }
    const auto frames = static_cast<std::uint32_t>((additional + frame_bytes - 1) / frame_bytes);
    if (self.late_.on_callback(SDL_GetTicksNS(), frames)) {
        mt_audio_note_late();
    }
    self.scratch_.resize(std::size_t{std::min(frames, kBlock)} * MT_AUDIO_CHANNELS);
    for (std::uint32_t at = 0; at < frames;) {
        const std::uint32_t n = std::min(frames - at, kBlock);
        mt_audio_render(self.scratch_.data(), n);
        SDL_PutAudioStreamData(stream, self.scratch_.data(), static_cast<int>(n) * frame_bytes);
        at += n;
    }
}

// --- AudioCapture ------------------------------------------------------------------------------

bool AudioCapture::open(const std::string &path) {
    close();
    path_ = path;
    frames_ = 0;
    failed_ = false;
    out_.open(utf8_path(path), std::ios::binary | std::ios::trunc);
    std::uint8_t header[kWavHeaderBytes];
    wav_header(header, 0);
    if (!out_ || !out_.write(reinterpret_cast<const char *>(header), sizeof header)) {
        SDL_Log("audio: cannot write the capture %s", path.c_str());
        out_.close();
        return false;
    }
    SDL_Log("audio: capturing the mix to %s (16-bit stereo, %u Hz)", path.c_str(), MT_AUDIO_RATE);
    return true;
}

bool AudioCapture::render_to(std::uint64_t elapsed_ns) {
    if (!out_.is_open() || failed_) {
        return false;
    }
    const std::uint64_t target = audio_frames_at(elapsed_ns);
    while (frames_ < target) {
        const auto n =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(target - frames_, kBlock));
        scratch_.resize(std::size_t{n} * MT_AUDIO_CHANNELS);
        bytes_.resize(scratch_.size() * 2);
        mt_audio_render(scratch_.data(), n);
        for (std::size_t i = 0; i < scratch_.size(); i++) {
            // [-1, 1] -> [-32767, 32767], rounded half away from zero (the same on every host)
            const long v = std::lround(std::clamp(scratch_[i], -1.0f, 1.0f) * 32767.0f);
            put16(&bytes_[2 * i], static_cast<std::uint32_t>(v) & 0xFFFFu);
        }
        if (!out_.write(reinterpret_cast<const char *>(bytes_.data()),
                        static_cast<std::streamsize>(bytes_.size()))) {
            SDL_Log("audio: cannot write the capture %s", path_.c_str());
            failed_ = true;
            return false;
        }
        frames_ += n;
    }
    return true;
}

bool AudioCapture::close() {
    if (!out_.is_open()) {
        return !failed_;
    }
    std::uint8_t header[kWavHeaderBytes];
    wav_header(header, frames_);
    out_.seekp(0);
    if (!out_.write(reinterpret_cast<const char *>(header), sizeof header)) {
        failed_ = true;
    }
    out_.close();
    if (out_.fail()) {
        failed_ = true;
    }
    SDL_Log("audio: capture %s: %llu frames (%.3f s)%s", path_.c_str(),
            static_cast<unsigned long long>(frames_), static_cast<double>(frames_) / MT_AUDIO_RATE,
            failed_ ? ", WRITE FAILED" : "");
    return !failed_;
}

float volume_gain(std::uint32_t volume) {
    return static_cast<float>(std::min(volume, kMaxVolume)) / static_cast<float>(kMaxVolume);
}

void apply_audio_settings(const Settings &settings) {
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, volume_gain(settings.master_volume));
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, volume_gain(settings.music_volume));
    mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, volume_gain(settings.effects_volume));
    mt_audio_set_gapless_music(settings.gapless_music);
    mt_audio_set_mono(settings.audio_mono);
}

void FocusMute::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_ && muted_) {
        muted_ = false;
        mt_audio_set_muted(false);
        SDL_Log("audio: unmuted (mute_unfocused off)");
    }
}

bool FocusMute::on_event(std::uint32_t event_type) {
    bool muted = muted_;
    if (event_type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        muted = enabled_;
    } else if (event_type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        muted = false;
    }
    if (muted == muted_) {
        return false;
    }
    muted_ = muted;
    mt_audio_set_muted(muted);
    SDL_Log("audio: %s (the window %s focus)", muted ? "muted" : "unmuted",
            muted ? "lost" : "gained");
    return true;
}

} // namespace maplethief::frontend
