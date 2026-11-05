// The audio mixer and the pull API (#61, ADR 0045); see mixer.h and maplethief/audio.h.
// Compiled without floating-point contraction (maplethief_strict_fp): every float operation
// here rounds as written, so a mix is the same bytes on every host.
#include "audio/mixer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace mt_audio::mix {
namespace {

constexpr float k_sample_scale = 1.0f / 32768.0f; // 16-bit -> [-1, 1), a power of two: exact

struct EffectVoice {
    PcmPtr pcm;
    std::uint64_t pos = 0; // 32.32 frames into pcm
    std::uint64_t step = 0;
    bool playing = false;
};

struct MusicVoice {
    std::shared_ptr<MusicStream> stream;
    float volume = 1.0f;
    bool playing = false;
    // Frames pulled from the stream and not played yet: buf[0 .. frames) with the read
    // position pos (32.32) relative to buf[0]. The frame at pos >> 32 and the next one are
    // there whenever the stream has not ended.
    std::vector<std::int16_t> buf;
    u32 frames = 0;
    u32 channels = 0;
    std::uint64_t pos = 0;
    std::uint64_t step = 0;
    bool ended = false;

    void drop_buffer() {
        frames = 0;
        pos = 0;
        ended = false;
    }
};

struct Mixer {
    std::mutex lock;
    // The game's state (reset per process).
    std::map<u32, EffectVoice> effects; // by source name: the mix order
    std::map<u32, MusicVoice> music;    // by key
    float listener_gain = 1.0f;
    bool effects_paused = false;
    // The host's settings (kept across processes).
    float bus[MT_AUDIO_BUS_COUNT] = {1.0f, 1.0f, 1.0f};
    bool muted = false;
    bool mono = false; // mt_audio_set_mono (#195)
    // Output stats.
    std::uint64_t frames_rendered = 0, renders = 0;
    std::uint32_t longest_render_us = 0;
    std::atomic<std::uint64_t> late{0};
};

Mixer &mixer() {
    static Mixer m;
    return m;
}

bool is_bus(mt_audio_bus bus) { return static_cast<u32>(bus) < MT_AUDIO_BUS_COUNT; }

float sanitize_gain(float gain, float fallback) {
    return std::isnan(gain) ? fallback : std::max(gain, 0.0f);
}

// Adds `n` output frames of a source (`samples`, `frames` frames of `channels`) from 32.32
// position `pos` on at `step` to `out`, scaled by `g` (gain / 32768). Stops early, returning
// the frames done, when the next frame would need source frame `limit` or beyond: for a
// whole buffer limit is `frames` (one frame past the last one plays its fade to silence), for
// a stream's buffer limit is `frames - 1` while the stream goes on.
u32 add_source(const std::int16_t *samples, u32 frames, u32 channels, std::uint64_t &pos,
               std::uint64_t step, float g, u32 limit, float *out, u32 n) {
    u32 done = 0;
    for (; done < n; done++) {
        const std::uint64_t i = pos >> 32;
        if (i >= limit) {
            break;
        }
        const float l = static_cast<float>(interpolate(samples, frames, channels, 0, pos)) * g;
        const float r =
            channels == 1 ? l
                          : static_cast<float>(interpolate(samples, frames, channels, 1, pos)) * g;
        out[2 * done] += l;
        out[2 * done + 1] += r;
        pos += step;
    }
    return done;
}

void mix_effects(Mixer &m, float *out, u32 n) {
    if (m.effects_paused) {
        return;
    }
    const float g = m.listener_gain * m.bus[MT_AUDIO_BUS_EFFECTS] * k_sample_scale;
    for (auto &[key, v] : m.effects) {
        if (!v.playing) {
            continue;
        }
        const Pcm &p = *v.pcm;
        const u32 done =
            add_source(p.samples.data(), p.frames, p.channels, v.pos, v.step, g, p.frames, out, n);
        if (done < n) {
            v.playing = false; // past its last frame
        }
    }
}

// Pulls the next block of a music voice's stream behind what it still has to play.
void refill(MusicVoice &v) {
    const u32 ch = v.channels;
    const u32 keep_from = static_cast<u32>(std::min<std::uint64_t>(v.pos >> 32, v.frames));
    if (keep_from != 0) {
        std::memmove(v.buf.data(), v.buf.data() + std::size_t{keep_from} * ch,
                     std::size_t{v.frames - keep_from} * ch * sizeof(std::int16_t));
        v.frames -= keep_from;
        v.pos -= std::uint64_t{keep_from} << 32;
    }
    const u32 want = kMusicBlock;
    v.buf.resize(std::size_t{v.frames + want} * ch);
    const u32 got = std::min(want, v.stream->read(v.buf.data() + std::size_t{v.frames} * ch, want));
    v.frames += got;
    if (got < want) {
        v.ended = true;
    }
}

void mix_music(Mixer &m, float *out, u32 n) {
    for (auto &[key, v] : m.music) {
        if (!v.playing || !v.stream) {
            continue;
        }
        const float g = v.volume * m.bus[MT_AUDIO_BUS_MUSIC] * k_sample_scale;
        u32 done = 0;
        while (done < n) {
            const u32 limit = v.ended ? v.frames : (v.frames == 0 ? 0 : v.frames - 1);
            done += add_source(v.buf.data(), v.frames, v.channels, v.pos, v.step, g, limit,
                               out + 2 * done, n - done);
            if (done == n) {
                break;
            }
            if (v.ended) {
                v.playing = false; // the stream ended and its last frame has played
                v.drop_buffer();
                break;
            }
            refill(v);
        }
    }
}

} // namespace

PcmPtr make_pcm(const std::uint8_t *data, u32 bytes, u32 channels, u32 bits, u32 rate) {
    auto p = std::make_shared<Pcm>();
    p->channels = channels;
    p->rate = rate;
    const u32 frame_bytes = channels * (bits / 8);
    p->frames = frame_bytes == 0 ? 0 : bytes / frame_bytes;
    const std::size_t n = std::size_t{p->frames} * channels;
    p->samples.assign(n, 0);
    if (data != nullptr) {
        if (bits == 8) {
            for (std::size_t i = 0; i < n; i++) {
                p->samples[i] = static_cast<std::int16_t>((static_cast<int>(data[i]) - 128) * 256);
            }
        } else {
            std::memcpy(p->samples.data(), data, n * sizeof(std::int16_t));
        }
    }
    return p;
}

std::uint64_t step_for(u32 rate) { return (std::uint64_t{rate} << 32) / MT_AUDIO_RATE; }

std::int32_t interpolate(const std::int16_t *samples, u32 frames, u32 channels, u32 ch,
                         std::uint64_t pos) {
    const std::uint64_t i = pos >> 32;
    const std::int64_t frac = static_cast<std::int64_t>(pos & 0xFFFFFFFFu);
    const std::int64_t s0 = i < frames ? samples[i * channels + ch] : 0;
    const std::int64_t s1 = i + 1 < frames ? samples[(i + 1) * channels + ch] : 0;
    return static_cast<std::int32_t>(s0 + (((s1 - s0) * frac) >> 32));
}

// --- effects -----------------------------------------------------------------------------------

void effect_play(u32 key, PcmPtr pcm) {
    PcmPtr old;
    {
        Mixer &m = mixer();
        std::lock_guard<std::mutex> hold(m.lock);
        EffectVoice &v = m.effects[key];
        old = std::move(v.pcm);
        v.playing = pcm != nullptr && pcm->frames != 0 && pcm->rate != 0;
        v.pcm = std::move(pcm);
        v.pos = 0;
        v.step = v.playing ? step_for(v.pcm->rate) : 0;
    }
    // `old` is freed here, outside the lock.
}

void effect_stop(u32 key) {
    PcmPtr old;
    {
        Mixer &m = mixer();
        std::lock_guard<std::mutex> hold(m.lock);
        const auto v = m.effects.find(key);
        if (v == m.effects.end()) {
            return;
        }
        old = std::move(v->second.pcm);
        m.effects.erase(v);
    }
}

void effect_stop_all() {
    std::map<u32, EffectVoice> old;
    {
        Mixer &m = mixer();
        std::lock_guard<std::mutex> hold(m.lock);
        old.swap(m.effects);
    }
}

bool effect_playing(u32 key) {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    const auto v = m.effects.find(key);
    return v != m.effects.end() && v->second.playing;
}

void set_listener_gain(float gain) {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    m.listener_gain = sanitize_gain(gain, m.listener_gain);
}

void set_effects_paused(bool paused) {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    m.effects_paused = paused;
}

// --- music -------------------------------------------------------------------------------------

void music_play(u32 key, std::shared_ptr<MusicStream> stream, float volume) {
    std::shared_ptr<MusicStream> old;
    {
        Mixer &m = mixer();
        std::lock_guard<std::mutex> hold(m.lock);
        MusicVoice &v = m.music[key];
        if (v.stream != stream) {
            // Only 1 or 2 channels at a positive rate can play.
            if (stream &&
                (stream->rate() == 0 || stream->channels() < 1 || stream->channels() > 2)) {
                stream.reset();
            }
            old = std::move(v.stream);
            v.stream = std::move(stream);
            v.drop_buffer();
            v.channels = v.stream ? v.stream->channels() : 0;
            v.step = v.stream ? step_for(v.stream->rate()) : 0;
            v.buf.reserve(std::size_t{kMusicBlock + 1} * MT_AUDIO_CHANNELS);
        }
        v.volume = sanitize_gain(volume, v.volume);
        v.playing = v.stream != nullptr && v.step != 0;
    }
}

void music_stop(u32 key) {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    const auto v = m.music.find(key);
    if (v != m.music.end()) {
        v->second.playing = false;
    }
}

void music_remove(u32 key) {
    std::shared_ptr<MusicStream> old;
    {
        Mixer &m = mixer();
        std::lock_guard<std::mutex> hold(m.lock);
        const auto v = m.music.find(key);
        if (v == m.music.end()) {
            return;
        }
        old = std::move(v->second.stream);
        m.music.erase(v);
    }
}

void music_set_volume(u32 key, float volume) {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    const auto v = m.music.find(key);
    if (v != m.music.end()) {
        v->second.volume = sanitize_gain(volume, v->second.volume);
    }
}

bool music_playing(u32 key) {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    const auto v = m.music.find(key);
    return v != m.music.end() && v->second.playing;
}

bool music_edit(u32 key, const std::function<void(MusicStream &)> &fn) {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    const auto v = m.music.find(key);
    if (v == m.music.end() || !v->second.stream) {
        return false;
    }
    v->second.drop_buffer();
    fn(*v->second.stream);
    return true;
}

// --- the whole mixer ---------------------------------------------------------------------------

void reset() {
    std::map<u32, EffectVoice> old_effects;
    std::map<u32, MusicVoice> old_music;
    {
        Mixer &m = mixer();
        std::lock_guard<std::mutex> hold(m.lock);
        old_effects.swap(m.effects);
        old_music.swap(m.music);
        m.listener_gain = 1.0f;
        m.effects_paused = false;
    }
}

void render(float *out, u32 frames) {
    std::fill(out, out + std::size_t{frames} * MT_AUDIO_CHANNELS, 0.0f);
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    mix_effects(m, out, frames);
    mix_music(m, out, frames);
    if (m.mono) { // (L + R) / 2 to both channels, before the master bus and the clamp (#195)
        for (std::size_t i = 0; i < std::size_t{frames} * MT_AUDIO_CHANNELS; i += 2) {
            out[i] = out[i + 1] = (out[i] + out[i + 1]) * 0.5f;
        }
    }
    const float master = m.muted ? 0.0f : m.bus[MT_AUDIO_BUS_MASTER];
    for (std::size_t i = 0; i < std::size_t{frames} * MT_AUDIO_CHANNELS; i++) {
        out[i] = std::clamp(out[i] * master, -1.0f, 1.0f);
    }
}

u32 effects_playing() {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    u32 n = 0;
    for (const auto &e : m.effects) {
        n += e.second.playing && !m.effects_paused ? 1u : 0u;
    }
    return n;
}

u32 music_voices_playing() {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    u32 n = 0;
    for (const auto &e : m.music) {
        n += e.second.playing ? 1u : 0u;
    }
    return n;
}

} // namespace mt_audio::mix

using namespace mt_audio::mix;

extern "C" {

void mt_audio_render(float *out, std::uint32_t frames) noexcept {
    if (out == nullptr || frames == 0) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    // In blocks, so that a long render does not hold the lock for long.
    constexpr u32 k_block = 4096;
    for (u32 at = 0; at < frames; at += k_block) {
        render(out + std::size_t{at} * MT_AUDIO_CHANNELS, std::min(k_block, frames - at));
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    m.frames_rendered += frames;
    m.renders++;
    m.longest_render_us = std::max(m.longest_render_us,
                                   static_cast<std::uint32_t>(std::min<long long>(us, 0xFFFFFFFF)));
}

void mt_audio_set_gain(mt_audio_bus bus, float gain) noexcept {
    if (!is_bus(bus) || std::isnan(gain)) {
        return;
    }
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    m.bus[bus] = std::clamp(gain, 0.0f, 1.0f);
}

float mt_audio_get_gain(mt_audio_bus bus) noexcept {
    if (!is_bus(bus)) {
        return 0.0f;
    }
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    return m.bus[bus];
}

void mt_audio_set_muted(bool muted) noexcept {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    m.muted = muted;
}

bool mt_audio_get_muted(void) noexcept {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    return m.muted;
}

void mt_audio_set_mono(bool mono) noexcept {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    m.mono = mono;
}

bool mt_audio_get_mono(void) noexcept {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    return m.mono;
}

mt_audio_output_stats mt_audio_get_output_stats(void) noexcept {
    mt_audio_output_stats st{};
    const u32 effects = effects_playing();
    const u32 music = music_voices_playing();
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    st.frames = m.frames_rendered;
    st.renders = m.renders;
    st.late = m.late.load(std::memory_order_relaxed);
    st.longest_render_us = m.longest_render_us;
    st.effects_playing = effects;
    st.music_playing = music;
    return st;
}

void mt_audio_reset_output_stats(void) noexcept {
    Mixer &m = mixer();
    std::lock_guard<std::mutex> hold(m.lock);
    m.frames_rendered = 0;
    m.renders = 0;
    m.longest_render_us = 0;
    m.late.store(0, std::memory_order_relaxed);
}

void mt_audio_note_late(void) noexcept { mixer().late.fetch_add(1, std::memory_order_relaxed); }

void mt_audio_test_tone(float hz, std::uint32_t ms) noexcept {
    constexpr u32 k_rate = 22050;
    const u32 frames =
        static_cast<u32>(std::uint64_t{k_rate} * std::min<std::uint32_t>(ms, 60000) / 1000);
    auto p = std::make_shared<Pcm>();
    p->channels = 1;
    p->rate = k_rate;
    p->frames = frames;
    p->samples.resize(frames);
    const double w = 2.0 * 3.14159265358979323846 * static_cast<double>(hz) / k_rate;
    for (u32 i = 0; i < frames; i++) {
        p->samples[i] = static_cast<std::int16_t>(std::lround(16383.0 * std::sin(w * i)));
    }
    // Key 0: no OpenAL source has that name.
    effect_play(0, std::move(p));
}

} // extern "C"
