// OpenAL through the mixer (#61, ADR 0045) on a real guest address space with synthetic PCM
// only: generated tones go through alBufferData -> alSourcePlay -> mt_audio_render and come out
// sample for sample as the mixer's rules say (44.1 kHz as is; 22,050 and 8,000 Hz resampled by
// the 32.32 step with linear interpolation; stereo and 8-bit formats), restarts, the mix of two
// sources and its clamp, the listener gain, the host's buses and mute, the context's suspend
// and process, the game's start-up sequence (no AL error, so no exit(1)) and its
// alSourcePlay(-1), alcGetProcAddress, a render thread beside the guest's calls, a new process,
// and the music voices that #63 plays through (a synthetic MusicStream).
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "audio/audio_internal.h"
#include "audio/mixer.h"
#include "gles/gles_internal.h" // the fixture's GL calls
#include "maplethief/audio.h"

#include "gles_fixture.h"

namespace {

namespace mix = mt_audio::mix;
using i16 = std::int16_t;

enum : u32 {
    AL_FORMAT_MONO8 = 0x1100,
    AL_FORMAT_MONO16 = 0x1101,
    AL_FORMAT_STEREO8 = 0x1102,
    AL_FORMAT_STEREO16 = 0x1103,
    AL_BUFFER = 0x1009,
    AL_GAIN = 0x100A,
    AL_INVALID_NAME = 0xA001,
    AL_INVALID_VALUE = 0xA003,
    AL_INVALID_OPERATION = 0xA004,
};

// A triangle wave: period 100 frames, amplitude 20,000, shifted by `phase` frames.
std::vector<i16> tone(u32 frames, u32 phase = 0) {
    std::vector<i16> s(frames);
    for (u32 i = 0; i < frames; i++) {
        const int t = static_cast<int>((i + phase) % 100);
        s[i] = static_cast<i16>((t < 50 ? t : 100 - t) * 800 - 20000);
    }
    return s;
}

// What the mixer must give for frame `k` of a source, by ADR 0045's rule written out again
// here: position k * step (32.32), s0 + ((s1 - s0) * frac) >> 32, silence past the end.
std::int32_t expected(const std::vector<i16> &s, u32 channels, u32 ch, std::uint64_t step,
                      std::uint64_t k) {
    const std::uint64_t pos = k * step;
    const std::uint64_t i = pos >> 32;
    const std::int64_t frac = static_cast<std::int64_t>(pos & 0xFFFFFFFFu);
    const std::uint64_t frames = s.size() / channels;
    const std::int64_t s0 = i < frames ? s[i * channels + ch] : 0;
    const std::int64_t s1 = i + 1 < frames ? s[(i + 1) * channels + ch] : 0;
    return static_cast<std::int32_t>(s0 + (((s1 - s0) * frac) >> 32));
}

float f(std::int32_t sample) { return static_cast<float>(sample) / 32768.0f; }

std::vector<float> render(u32 frames) {
    std::vector<float> out(std::size_t{frames} * 2, 123.0f); // overwritten, not added to
    mt_audio_render(out.data(), frames);
    return out;
}

bool silent(const std::vector<float> &out) {
    for (const float v : out) {
        if (v != 0.0f) {
            return false;
        }
    }
    return true;
}

// The host's gains back to 1 and unmuted when a test ends.
struct Buses {
    Buses() { restore(); }
    ~Buses() { restore(); }
    static void restore() {
        mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 1.0f);
        mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 1.0f);
        mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, 1.0f);
        mt_audio_set_muted(false);
        mt_audio_set_mono(false);
    }
};

// A device, a current context and helpers, as CMSoundPlayer sets them up.
struct Al {
    Env &e;
    u32 device = 0, context = 0;
    explicit Al(Env &env) : e(env) {
        device = e.call(mt_hle_alcOpenDevice, {0}); // the first audio call: a new process
        context = e.call(mt_hle_alcCreateContext, {device, 0});
        REQUIRE(e.call(mt_hle_alcMakeContextCurrent, {context}) == 1);
        REQUIRE(error() == 0);
    }
    u32 error() { return e.call(mt_hle_alGetError, {}); }
    u32 gen(mt_import_fn fn) {
        const u32 out = e.alloc(4);
        e.call(fn, {1, out});
        return e.get32(out);
    }
    u32 buffer() { return gen(mt_hle_alGenBuffers); }
    u32 source() { return gen(mt_hle_alGenSources); }
    // Fills `buffer` from a guest copy of `data`; returns the guest copy.
    u32 fill(u32 buffer, u32 format, const void *data, u32 bytes, u32 rate) {
        const u32 p = e.bytes(data, bytes);
        e.call(mt_hle_alBufferData, {buffer, format, p, bytes, rate});
        return p;
    }
    // A source playing `s` (mono 16-bit at `rate`).
    u32 play_mono(const std::vector<i16> &s, u32 rate, u32 *buffer_out = nullptr) {
        const u32 b = buffer(), src = source();
        fill(b, AL_FORMAT_MONO16, s.data(), static_cast<u32>(s.size() * 2), rate);
        e.call(mt_hle_alSourcei, {src, AL_BUFFER, b});
        e.call(mt_hle_alSourcePlay, {src});
        if (buffer_out != nullptr) {
            *buffer_out = b;
        }
        return src;
    }
};

// --- a tone, sample for sample -------------------------------------------------------------------

TEST_CASE("audio: a 44.1 kHz mono tone through alBufferData, alSourcePlay and mt_audio_render") {
    Env e;
    Buses buses;
    Al al(e);
    const std::vector<i16> s = tone(1000);
    CHECK(silent(render(64))); // nothing plays yet
    const u32 b = al.buffer(), src = al.source();
    const u32 guest = al.fill(b, AL_FORMAT_MONO16, s.data(), 2000, 44100);
    // alBufferData copied the samples: the game frees (here: scribbles over) its copy
    std::memset(e.cpu.mem + guest, 0x55, 2000);
    e.call(mt_hle_alSourcei, {src, AL_BUFFER, b});
    CHECK(mt_audio_get_output_stats().effects_playing == 0);
    e.call(mt_hle_alSourcePlay, {src});
    CHECK(al.error() == 0);
    CHECK(mt_audio_get_output_stats().effects_playing == 1);
    const std::vector<float> out = render(1200);
    for (u32 k = 0; k < 1200; k++) {
        const float want = k < 1000 ? f(s[k]) : 0.0f; // step 1: the samples as they are
        REQUIRE(out[2 * k] == want);
        REQUIRE(out[2 * k + 1] == want); // mono: both channels at unit gain
    }
    CHECK(mt_audio_get_output_stats().effects_playing == 0); // played out
    CHECK(silent(render(64)));
    CHECK(mt_audio_get_stats().source_plays == 1);
    CHECK(!logged("unimplemented"));
}

TEST_CASE("audio: 22,050 and 8,000 Hz are resampled by the 32.32 step, linearly") {
    Env e;
    Buses buses;
    Al al(e);
    SUBCASE("22,050 Hz: step 2^31, every other frame halfway") {
        CHECK(mix::step_for(22050) == 0x80000000ull);
        const std::vector<i16> s = tone(300, 7);
        al.play_mono(s, 22050);
        const std::vector<float> out = render(700);
        for (u32 k = 0; k < 300; k++) {
            const std::int32_t next = k + 1 < 300 ? s[k + 1] : 0; // past the end: silence
            REQUIRE(out[4 * k] == f(s[k]));
            // s + ((next - s) * 2^31) >> 32 = s + floor((next - s) / 2)
            const std::int32_t d = next - s[k];
            const std::int32_t half = d >= 0 ? d / 2 : -((-d + 1) / 2);
            REQUIRE(out[4 * k + 2] == f(s[k] + half));
            REQUIRE(out[4 * k + 3] == out[4 * k + 2]);
        }
        for (u32 k = 600; k < 700; k++) {
            REQUIRE(out[2 * k] == 0.0f);
        }
    }
    SUBCASE("8,000 Hz: floor(8000 * 2^32 / 44100) per frame") {
        const std::uint64_t step = (std::uint64_t{8000} << 32) / 44100;
        CHECK(mix::step_for(8000) == step);
        CHECK(step == 779132389ull);
        const std::vector<i16> s = tone(400, 3);
        al.play_mono(s, 8000);
        // the voice ends at the first output frame whose position is past the last source frame
        std::uint64_t frames = 0;
        while (((frames * step) >> 32) < 400) {
            frames++;
        }
        // 400 * 44100 / 8000 = 2205 exactly, but the step is rounded down: one frame more, the
        // fade from the last sample to silence
        CHECK(frames == 2206);
        const std::vector<float> out = render(2300);
        for (u32 k = 0; k < 2300; k++) {
            const float want = k < frames ? f(expected(s, 1, 0, step, k)) : 0.0f;
            REQUIRE(out[2 * k] == want);
            REQUIRE(out[2 * k + 1] == want);
        }
        // a spot check of the rule: frame 1 is 0.18 of the way from s[0] to s[1]
        const std::int64_t frac = static_cast<std::int64_t>(step);
        CHECK(out[2] == f(static_cast<std::int32_t>(s[0] + (((s[1] - s[0]) * frac) >> 32))));
    }
    CHECK(al.error() == 0);
}

TEST_CASE("audio: stereo and 8-bit buffers") {
    Env e;
    Buses buses;
    Al al(e);
    SUBCASE("AL_FORMAT_STEREO16: left and right apart") {
        const std::vector<i16> l = tone(200), r = tone(200, 50);
        std::vector<i16> s(400);
        for (u32 i = 0; i < 200; i++) {
            s[2 * i] = l[i];
            s[2 * i + 1] = r[i];
        }
        const u32 b = al.buffer(), src = al.source();
        al.fill(b, AL_FORMAT_STEREO16, s.data(), 800, 44100);
        e.call(mt_hle_alSourcei, {src, AL_BUFFER, b});
        e.call(mt_hle_alSourcePlay, {src});
        const std::vector<float> out = render(210);
        for (u32 k = 0; k < 200; k++) {
            REQUIRE(out[2 * k] == f(l[k]));
            REQUIRE(out[2 * k + 1] == f(r[k]));
        }
        CHECK(out[2 * 205] == 0.0f);
    }
    SUBCASE("AL_FORMAT_MONO8 and AL_FORMAT_STEREO8: unsigned, 128 is silence") {
        const u8 mono[4] = {128, 255, 0, 64};
        const u8 stereo[4] = {0, 255, 128, 192};
        const u32 b1 = al.buffer(), b2 = al.buffer(), s1 = al.source(), s2 = al.source();
        al.fill(b1, AL_FORMAT_MONO8, mono, 4, 44100);
        al.fill(b2, AL_FORMAT_STEREO8, stereo, 4, 44100);
        e.call(mt_hle_alSourcei, {s1, AL_BUFFER, b1});
        e.call(mt_hle_alSourcePlay, {s1});
        std::vector<float> out = render(5);
        const float m[4] = {0.0f, f(127 * 256), f(-128 * 256), f(-64 * 256)};
        for (u32 k = 0; k < 4; k++) {
            CHECK(out[2 * k] == m[k]);
            CHECK(out[2 * k + 1] == m[k]);
        }
        e.call(mt_hle_alSourcei, {s2, AL_BUFFER, b2});
        e.call(mt_hle_alSourcePlay, {s2});
        out = render(3);
        CHECK(out[0] == f(-128 * 256));
        CHECK(out[1] == f(127 * 256));
        CHECK(out[2] == 0.0f);
        CHECK(out[3] == f(64 * 256));
        CHECK(out[4] == 0.0f);
    }
    CHECK(al.error() == 0);
}

TEST_CASE("audio: mono mixes (L + R) / 2 into both channels, before the master bus and clamp") {
    Env e;
    Buses buses;
    Al al(e);
    // A synthetic stereo buffer, left and right apart (#195).
    const std::vector<i16> l = tone(200), r = tone(200, 50);
    std::vector<i16> s(400);
    for (u32 i = 0; i < 200; i++) {
        s[2 * i] = l[i];
        s[2 * i + 1] = r[i];
    }
    const u32 b = al.buffer(), src = al.source();
    al.fill(b, AL_FORMAT_STEREO16, s.data(), 800, 44100);
    e.call(mt_hle_alSourcei, {src, AL_BUFFER, b});
    CHECK_FALSE(mt_audio_get_mono()); // off at start
    // Off: exactly the stereo mix.
    e.call(mt_hle_alSourcePlay, {src});
    std::vector<float> out = render(200);
    for (u32 k = 0; k < 200; k++) {
        REQUIRE(out[2 * k] == f(l[k]));
        REQUIRE(out[2 * k + 1] == f(r[k]));
    }
    // On: both channels (L + R) / 2, then the master bus.
    mt_audio_set_mono(true);
    CHECK(mt_audio_get_mono());
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 0.5f);
    e.call(mt_hle_alSourcePlay, {src});
    out = render(200);
    for (u32 k = 0; k < 200; k++) {
        const float m = (f(l[k]) + f(r[k])) * 0.5f * 0.5f;
        REQUIRE(out[2 * k] == m);
        REQUIRE(out[2 * k + 1] == m);
    }
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 1.0f);
    // The clamp is where it was, after the downmix: opposite full-scale channels cancel to 0,
    // and two loud sources that clip in stereo clip the same in mono.
    out = render(2); // the first source's end
    const i16 apart[2] = {32767, -32767};
    const i16 loud[2] = {30000, 30000};
    const u32 b2 = al.buffer(), b3 = al.buffer();
    const u32 s2 = al.source(), s3 = al.source(), s4 = al.source();
    al.fill(b2, AL_FORMAT_STEREO16, apart, 4, 44100);
    al.fill(b3, AL_FORMAT_STEREO16, loud, 4, 44100);
    e.call(mt_hle_alSourcei, {s2, AL_BUFFER, b2});
    e.call(mt_hle_alSourcePlay, {s2});
    out = render(1);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == 0.0f);
    e.call(mt_hle_alSourcei, {s3, AL_BUFFER, b3});
    e.call(mt_hle_alSourcei, {s4, AL_BUFFER, b3});
    e.call(mt_hle_alSourcePlay, {s3});
    e.call(mt_hle_alSourcePlay, {s4});
    out = render(1);
    CHECK(out[0] == 1.0f);
    CHECK(out[1] == 1.0f);
    CHECK(al.error() == 0);
}

// --- sources -------------------------------------------------------------------------------------

TEST_CASE("audio: alSourcePlay restarts, sources mix and clamp, empty sources stay silent") {
    Env e;
    Buses buses;
    Al al(e);
    const std::vector<i16> s = tone(1000);
    u32 buffer = 0;
    const u32 a = al.play_mono(s, 44100, &buffer);
    std::vector<float> out = render(300);
    CHECK(out[2 * 299] == f(s[299]));
    // replaying a playing source restarts it at frame 0 (an effect never overlaps itself)
    e.call(mt_hle_alSourcePlay, {a});
    out = render(10);
    for (u32 k = 0; k < 10; k++) {
        CHECK(out[2 * k] == f(s[k]));
    }
    // a second source adds to it
    const std::vector<i16> c(500, 1000);
    const u32 b = al.play_mono(c, 44100);
    out = render(10);
    for (u32 k = 0; k < 10; k++) {
        CHECK(out[2 * k] == f(s[10 + k]) + f(1000));
    }
    // the buffer of a playing source cannot change, nor be deleted while attached
    e.call(mt_hle_alSourcei, {a, AL_BUFFER, 0});
    CHECK(al.error() == AL_INVALID_OPERATION);
    const u32 names = e.alloc(4);
    e.put32(names, buffer);
    e.call(mt_hle_alDeleteBuffers, {1, names});
    CHECK(al.error() == AL_INVALID_OPERATION);
    CHECK(mt_audio_get_stats().buffers == 2);
    // a deleted source stops
    e.put32(names, b);
    e.call(mt_hle_alDeleteSources, {1, names});
    CHECK(al.error() == 0);
    out = render(10);
    CHECK(out[0] == f(s[20]));
    render(1000);                                // a plays out
    e.call(mt_hle_alSourcei, {a, AL_BUFFER, 0}); // a stopped source can change its buffer
    CHECK(al.error() == 0);
    e.put32(names, buffer);
    e.call(mt_hle_alDeleteBuffers, {1, names});
    CHECK(al.error() == 0);

    // two loud sources clamp at full scale
    const std::vector<i16> loud(100, 30000);
    al.play_mono(loud, 44100);
    al.play_mono(loud, 44100);
    out = render(4);
    CHECK(out[0] == 1.0f);
    CHECK(out[1] == 1.0f);

    // no buffer, an empty buffer, a null buffer of silence: no sound and no error
    render(200);
    const u32 none = al.source();
    e.call(mt_hle_alSourcePlay, {none});
    const u32 empty = al.buffer(), with_empty = al.source();
    e.call(mt_hle_alBufferData, {empty, AL_FORMAT_MONO16, 0, 0, 22050});
    e.call(mt_hle_alSourcei, {with_empty, AL_BUFFER, empty});
    e.call(mt_hle_alSourcePlay, {with_empty});
    CHECK(mt_audio_get_output_stats().effects_playing == 0);
    const u32 nul = al.buffer(), with_nul = al.source();
    e.call(mt_hle_alBufferData, {nul, AL_FORMAT_MONO16, 0, 200, 22050});
    e.call(mt_hle_alSourcei, {with_nul, AL_BUFFER, nul});
    e.call(mt_hle_alSourcePlay, {with_nul});
    CHECK(mt_audio_get_output_stats().effects_playing == 1);
    CHECK(silent(render(300)));
    CHECK(al.error() == 0);

    // alBufferData: a partial frame, a too-large size
    const u32 bad = al.buffer();
    al.fill(bad, AL_FORMAT_STEREO16, s.data(), 6, 44100);
    CHECK(al.error() == AL_INVALID_VALUE);
    e.call(mt_hle_alBufferData, {bad, AL_FORMAT_MONO16, 0, 0x7FFFFFFEu, 44100});
    CHECK(al.error() == 0xA005); // AL_OUT_OF_MEMORY
}

TEST_CASE("audio: the listener gain, the host's buses and mute") {
    Env e;
    Buses buses;
    Al al(e);
    const std::vector<i16> s = tone(2000);
    const u32 src = al.play_mono(s, 44100);
    u32 at = 0;
    auto next = [&](u32 n) {
        std::vector<float> out = render(n);
        at += n;
        return out;
    };
    e.call(mt_hle_alListenerf, {AL_GAIN, fb(0.5f)});
    std::vector<float> out = next(10);
    for (u32 k = 0; k < 10; k++) {
        CHECK(out[2 * k] == f(s[k]) * 0.5f);
    }
    e.call(mt_hle_alListenerf, {AL_GAIN, fb(0.0f)}); // the game's effects switch, off
    CHECK(silent(next(10)));
    e.call(mt_hle_alListenerf, {AL_GAIN, fb(-1.0f)});
    CHECK(al.error() == AL_INVALID_VALUE);
    e.call(mt_hle_alListenerf, {AL_GAIN, fb(1.0f)});
    // the effects bus and the master bus multiply it
    mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, 0.5f);
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 0.25f);
    out = next(10);
    for (u32 k = 0; k < 10; k++) {
        CHECK(out[2 * k] == f(s[at - 10 + k]) * 0.5f * 0.25f);
    }
    // the music bus does not touch effects
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 0.0f);
    mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, 1.0f);
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 1.0f);
    out = next(10);
    CHECK(out[0] == f(s[at - 10]));
    // mute silences everything; the voice goes on meanwhile
    mt_audio_set_muted(true);
    CHECK(mt_audio_get_muted());
    CHECK(silent(next(10)));
    mt_audio_set_muted(false);
    out = next(10);
    CHECK(out[0] == f(s[at - 10]));
    // gains are clamped to [0, 1]; NaN and unknown buses are ignored
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 2.0f);
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MASTER) == 1.0f);
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, -1.0f);
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MUSIC) == 0.0f);
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 0.7f);
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, std::nanf(""));
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MUSIC) == 0.7f);
    mt_audio_set_gain(static_cast<mt_audio_bus>(7), 0.0f);
    CHECK(mt_audio_get_gain(static_cast<mt_audio_bus>(7)) == 0.0f);
    // none of it is an AL error, and the game's state reads the same
    CHECK(al.error() == 0);
    CHECK(mt_audio_get_stats().source_plays == 1);
    (void)src;
}

TEST_CASE("audio: a suspended or released context pauses the effects; processing resumes them") {
    Env e;
    Buses buses;
    Al al(e);
    const std::vector<i16> s = tone(1000);
    al.play_mono(s, 44100);
    std::vector<float> out = render(100);
    CHECK(out[2 * 99] == f(s[99]));
    // SoundPlayer_BeginInterruption: alcMakeContextCurrent(NULL), alcSuspendContext
    CHECK(e.call(mt_hle_alcMakeContextCurrent, {0}) == 1);
    CHECK(silent(render(50)));
    CHECK(al.error() == AL_INVALID_OPERATION); // no current context (ADR 0038)
    e.call(mt_hle_alcSuspendContext, {al.context});
    CHECK(silent(render(50)));
    CHECK(mt_audio_get_output_stats().effects_playing == 0);
    // SoundPlayer_EndInterruption: alcMakeContextCurrent(context), alcProcessContext
    CHECK(e.call(mt_hle_alcMakeContextCurrent, {al.context}) == 1);
    CHECK(silent(render(50))); // still suspended
    e.call(mt_hle_alcProcessContext, {al.context});
    out = render(10);
    for (u32 k = 0; k < 10; k++) {
        CHECK(out[2 * k] == f(s[100 + k])); // where it paused
    }
    CHECK(al.error() == 0);
    // destroying the last context stops the voices
    CHECK(e.call(mt_hle_alcMakeContextCurrent, {0}) == 1);
    e.call(mt_hle_alcDestroyContext, {al.context});
    e.call(mt_hle_alcMakeContextCurrent, {0});
    CHECK(mt_audio_get_output_stats().effects_playing == 0);
    CHECK(e.call(mt_hle_alcCloseDevice, {al.device}) == 1);
}

// --- the game's calls ----------------------------------------------------------------------------

TEST_CASE("audio: the game's start-up sequence raises no AL error; its quirks stay") {
    Env e;
    Buses buses;
    Al al(e);
    // SoundPlayer_InitOpenAL: 44 buffers and 44 sources; any error would be exit(1)
    const u32 buffers = e.alloc(44 * 4), sources = e.alloc(44 * 4);
    e.call(mt_hle_alGenBuffers, {44, buffers});
    CHECK(al.error() == 0);
    e.call(mt_hle_alGenSources, {44, sources});
    CHECK(al.error() == 0);
    // SoundPlayer_LoadEffects: the 26 effect ids, 16-bit at their file's rate and channels
    const auto is_bgm = [](u32 id) {
        return id <= 6 || id == 30 || id == 31 || (id >= 34 && id <= 42);
    };
    for (u32 id = 0; id < 44; id++) {
        if (is_bgm(id)) {
            continue;
        }
        const bool stereo = id == 10 || id == 20;
        const u32 rate = stereo ? 44100 : (id % 2 == 0 ? 22050 : 8000);
        const std::vector<i16> s = tone(stereo ? 400 : 200, id);
        al.fill(e.get32(buffers + 4 * id), stereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, s.data(),
                static_cast<u32>(s.size() * 2), rate);
        REQUIRE(al.error() == 0);
    }
    // SoundPlayer_AttachBuffers: every source gets its buffer, music ids' empty ones too
    for (u32 id = 0; id < 44; id++) {
        e.call(mt_hle_alSourcei, {e.get32(sources + 4 * id), AL_BUFFER, e.get32(buffers + 4 * id)});
        REQUIRE(al.error() == 0);
    }
    CHECK(mt_audio_get_stats().buffers_filled == 26);
    // Play(7) and Play(9): effects; a music id's source has no samples
    e.call(mt_hle_alSourcePlay, {e.get32(sources + 4 * 7)});
    e.call(mt_hle_alSourcePlay, {e.get32(sources + 4 * 9)});
    e.call(mt_hle_alSourcePlay, {e.get32(sources + 4 * 31)});
    CHECK(mt_audio_get_output_stats().effects_playing == 2);
    CHECK(al.error() == 0);
    // ResumeBGM before any music: alSourcePlay with the word before g_soundSources
    e.call(mt_hle_alSourcePlay, {0xFFFFFFFFu});
    CHECK(al.error() == AL_INVALID_NAME);
    CHECK(al.error() == 0);
    // EffectsOff / EffectsOn
    e.call(mt_hle_alListenerf, {AL_GAIN, fb(0.0f)});
    CHECK(silent(render(100)));
    e.call(mt_hle_alListenerf, {AL_GAIN, fb(1.0f)});
    CHECK(!silent(render(100)));
    // alcGetProcAddress: the game's one name (alBufferDataStaticProc) and any other: NULL
    CHECK(e.call(mt_hle_alcGetProcAddress, {0, e.bytes("alBufferDataStatic", 19)}) == 0);
    CHECK(e.call(mt_hle_alcGetProcAddress, {al.device, e.bytes("alcMacOSXMixerOutputRate", 25)}) ==
          0);
    CHECK(al.error() == 0);
}

TEST_CASE("audio: a new process stops the previous one's voices") {
    Buses buses;
    {
        Env e;
        Al al(e);
        al.play_mono(tone(5000), 44100);
        CHECK(!silent(render(10)));
    }
    Env e;
    e.call(mt_hle_alcOpenDevice, {0}); // the new process's first audio call
    CHECK(silent(render(10)));
    CHECK(mt_audio_get_stats().source_plays == 0);
}

TEST_CASE("audio: a render thread beside the guest's calls (thread-safety smoke)") {
    Env e;
    Buses buses;
    Al al(e);
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};
    std::atomic<std::uint64_t> blocks{0};
    std::thread renderer([&] {
        std::vector<float> out(512);
        while (!stop.load()) {
            mt_audio_render(out.data(), 256);
            for (const float v : out) {
                if (!(v >= -1.0f && v <= 1.0f)) {
                    bad = true;
                }
            }
            blocks++;
        }
    });
    const u32 names = e.alloc(4);
    for (u32 i = 0; i < 400; i++) {
        const std::vector<i16> s = tone(64 + i % 200, i);
        const u32 b = al.buffer(), src = al.source();
        al.fill(b, i % 2 == 0 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, s.data(),
                static_cast<u32>(s.size() * 2) & ~3u, i % 3 == 0 ? 8000 : 22050);
        e.call(mt_hle_alSourcei, {src, AL_BUFFER, b});
        e.call(mt_hle_alSourcePlay, {src});
        e.call(mt_hle_alSourcePlay, {src});
        e.call(mt_hle_alListenerf, {AL_GAIN, fb(i % 5 == 0 ? 0.0f : 1.0f)});
        if (i % 7 == 0) {
            e.call(mt_hle_alcSuspendContext, {al.context});
            e.call(mt_hle_alcProcessContext, {al.context});
        }
        mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, static_cast<float>(i % 10) / 10.0f);
        if (i % 3 == 0) {
            e.put32(names, src);
            e.call(mt_hle_alDeleteSources, {1, names});
            e.put32(names, b);
            e.call(mt_hle_alDeleteBuffers, {1, names});
        }
        (void)mt_audio_get_output_stats();
    }
    while (blocks.load() < 10) {
        std::this_thread::yield();
    }
    stop = true;
    renderer.join();
    CHECK(!bad.load());
    CHECK(al.error() == 0);
    CHECK(mt_audio_get_output_stats().renders >= 10);
}

// --- music voices (#63's interface) --------------------------------------------------------------

// Stereo 16-bit music made up on the spot: frame i is (tone(i), -tone(i)) at `rate`, for
// `length` frames (0: for ever). Counts its reads.
class TestStream : public mix::MusicStream {
  public:
    TestStream(u32 rate, u32 length) : rate_(rate), length_(length) {}
    u32 rate() const override { return rate_; }
    u32 channels() const override { return 2; }
    u32 read(i16 *out, u32 frames) override {
        largest_read = std::max(largest_read, frames);
        u32 n = 0;
        for (; n < frames && (length_ == 0 || position < length_); n++, position++) {
            const i16 v = sample(position);
            out[2 * n] = v;
            out[2 * n + 1] = static_cast<i16>(-v);
        }
        return n;
    }
    static i16 sample(std::uint64_t i) { return tone(1, static_cast<u32>(i % 100))[0]; }
    std::uint64_t position = 0;
    u32 largest_read = 0;

  private:
    u32 rate_, length_;
};

// The music a voice must give from source frame `first` on (a stream of `length` frames), by
// the same rule as the effects.
std::vector<i16> stream_frames(std::uint64_t first, u32 count) {
    std::vector<i16> s(std::size_t{count} * 2);
    for (u32 i = 0; i < count; i++) {
        s[2 * i] = TestStream::sample(first + i);
        s[2 * i + 1] = static_cast<i16>(-s[2 * i]);
    }
    return s;
}

TEST_CASE("audio: music voices pull a MusicStream in blocks, resampled, stopped and resumed") {
    Env e;
    Buses buses;
    Al al(e); // a new process: no voices
    auto stream = std::make_shared<TestStream>(22050, 0);
    const std::uint64_t step = mix::step_for(22050);
    mix::music_play(1, stream, 1.0f);
    CHECK(mix::music_playing(1));
    CHECK(mt_audio_get_output_stats().music_playing == 1);
    // 10,000 output frames = 5,000 source frames: more than one block of 4,096
    const std::vector<i16> s = stream_frames(0, 6000);
    std::vector<float> out = render(10000);
    for (u32 k = 0; k < 10000; k++) {
        REQUIRE(out[2 * k] == f(expected(s, 2, 0, step, k)));
        REQUIRE(out[2 * k + 1] == f(expected(s, 2, 1, step, k)));
    }
    CHECK(stream->largest_read <= mix::kMusicBlock);
    // -stop keeps the exact position: -play goes on with output frame 10,000
    mix::music_stop(1);
    CHECK(!mix::music_playing(1));
    CHECK(silent(render(500)));
    mix::music_play(1, stream, 0.5f);
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 0.5f);
    out = render(100);
    for (u32 k = 0; k < 100; k++) {
        REQUIRE(out[2 * k] == f(expected(s, 2, 0, step, 10000 + k)) * 0.5f * 0.5f);
    }
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 1.0f);
    mix::music_set_volume(1, 1.0f);
    // setCurrentTime:0 through music_edit: from the first frame again
    CHECK(
        mix::music_edit(1, [](mix::MusicStream &m) { static_cast<TestStream &>(m).position = 0; }));
    out = render(50);
    for (u32 k = 0; k < 50; k++) {
        REQUIRE(out[2 * k] == f(expected(s, 2, 0, step, k)));
    }
    CHECK(!mix::music_edit(99, [](mix::MusicStream &) {}));
    // effects and music add up
    const std::vector<i16> c(100, 1000);
    al.play_mono(c, 44100);
    out = render(10);
    for (u32 k = 0; k < 10; k++) {
        CHECK(out[2 * k] == f(1000) + f(expected(s, 2, 0, step, 50 + k)));
    }
    mix::music_remove(1);
    CHECK(!mix::music_playing(1));

    // a stream that ends: the voice plays out its last frame and stops
    auto finite = std::make_shared<TestStream>(44100, 5000);
    mix::music_play(2, finite, 1.0f);
    render(200); // the effect ends
    out = render(5000);
    const std::vector<i16> fs = stream_frames(200, 4800);
    for (u32 k = 0; k < 4800; k++) {
        REQUIRE(out[2 * k] == f(fs[2 * k]));
    }
    CHECK(!mix::music_playing(2));
    CHECK(silent(render(10)));
    // a stream the mixer cannot play is no voice
    auto mono3 = std::make_shared<TestStream>(0, 10);
    mix::music_play(3, mono3, 1.0f);
    CHECK(!mix::music_playing(3));
    // a new process forgets the music voices
    mix::music_play(4, std::make_shared<TestStream>(44100, 0), 1.0f);
    mix::reset();
    CHECK(!mix::music_playing(4));
    CHECK(silent(render(10)));
}

TEST_CASE("audio: the output stats count renders and late callbacks") {
    mt_audio_reset_output_stats();
    std::vector<float> out(2 * 100);
    mt_audio_render(out.data(), 100);
    mt_audio_render(out.data(), 0); // nothing
    mt_audio_render(nullptr, 10);   // nothing
    mt_audio_note_late();
    const mt_audio_output_stats st = mt_audio_get_output_stats();
    CHECK(st.frames == 100);
    CHECK(st.renders == 1);
    CHECK(st.late == 1);
    mt_audio_reset_output_stats();
    CHECK(mt_audio_get_output_stats().frames == 0);
    CHECK(MT_AUDIO_RATE == 44100u);
    CHECK(MT_AUDIO_CHANNELS == 2u);
}

} // namespace
