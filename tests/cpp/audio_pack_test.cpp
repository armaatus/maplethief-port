// Audio packs (#188, ADR 0106) on synthetic files only: generated tones and the LAME- and
// FFmpeg-encoded tones of tests/cpp/data/audio/. A pack's file in place of an effect goes through
// the game's ExtAudioFile calls (Apple's MyGetOpenALAudioData), alBufferData and the mixer sample
// for sample; in place of a track, through AVAudioPlayer and the mixer on the file's own timeline
// (no decoder delay), looped whole, by a .loop file or by a WAV's smpl chunk. Names in any case,
// .wav for .mp3 and .mp3 for .wav, FLAC (our own triangles, losslessly). Corrupt, cut, oversize and
// out-of-limit files (and random mutations of a good one) fall back to the game's file, logged once
// each. Without a pack, or with a pack without the name, nothing changes.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "audio/audio_internal.h"
#include "audio/audio_pack.h"
#include "audio/mixer.h"
#include "audio/mp3_stream.h"
#include "audio/wav.h"
#include "gles/gles_internal.h" // the fixture's GL calls
#include "maplethief/audio.h"
#include "maplethief/diag.h"

#include <dr_mp3.h> // the reference decode: the runtime's own dr_mp3

#include "gles_fixture.h"

namespace {

using i16 = std::int16_t;
using u64 = std::uint64_t;
namespace mix = mt_audio::mix;

const fs::path k_data = MT_AUDIO_DATA_DIR;

std::size_t count_logged(const char *text) {
    return static_cast<std::size_t>(std::count_if(g_log.begin(), g_log.end(), [&](const auto &l) {
        return l.find(text) != std::string::npos;
    }));
}

std::vector<u8> file_bytes(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

void write_file(const fs::path &p, const std::vector<u8> &data) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
}
void write_text(const fs::path &p, const std::string &text) {
    write_file(p, std::vector<u8>(text.begin(), text.end()));
}

// A generated tone: a triangle wave per channel (periods 100 and 74 frames), amplitude 20,000,
// shifted by `phase`.
std::vector<i16> tone(u32 frames, u32 channels, u32 phase = 0) {
    std::vector<i16> s(std::size_t{frames} * channels);
    for (u32 i = 0; i < frames; i++) {
        for (u32 ch = 0; ch < channels; ch++) {
            const int period = ch == 0 ? 100 : 74;
            const int t = static_cast<int>((i + phase) % static_cast<u32>(period));
            const int half = period / 2;
            s[std::size_t{i} * channels + ch] =
                static_cast<i16>((t < half ? t : period - t) * 40000 / period - 10000);
        }
    }
    return s;
}

void le(std::vector<u8> &b, u32 v, u32 bytes) {
    for (u32 k = 0; k < bytes; k++) {
        b.push_back(static_cast<u8>(v >> (8 * k)));
    }
}
void chunk(std::vector<u8> &b, const char *id, const std::vector<u8> &body) {
    b.insert(b.end(), id, id + 4);
    le(b, static_cast<u32>(body.size()), 4);
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() % 2 != 0) {
        b.push_back(0);
    }
}

// A 16-bit PCM WAV of `s`; with a smpl chunk of one loop [loop_start, loop_end] (inclusive) if
// loop_end != 0.
std::vector<u8> make_wav(const std::vector<i16> &s, u32 rate, u32 channels, u32 loop_start = 0,
                         u32 loop_end = 0) {
    std::vector<u8> fmt;
    le(fmt, 1, 2);
    le(fmt, channels, 2);
    le(fmt, rate, 4);
    le(fmt, rate * 2 * channels, 4);
    le(fmt, 2 * channels, 2);
    le(fmt, 16, 2);
    std::vector<u8> data;
    for (const i16 v : s) {
        le(data, static_cast<std::uint16_t>(v), 2);
    }
    std::vector<u8> body = {'W', 'A', 'V', 'E'};
    chunk(body, "fmt ", fmt);
    if (loop_end != 0) {
        std::vector<u8> smpl;
        for (int k = 0; k < 7; k++) {
            le(smpl, 0, 4); // manufacturer .. SMPTE offset
        }
        le(smpl, 1, 4); // one loop
        le(smpl, 0, 4); // no sampler data
        le(smpl, 0, 4); // the loop's id, type, start, end, fraction, play count
        le(smpl, 0, 4);
        le(smpl, loop_start, 4);
        le(smpl, loop_end, 4);
        le(smpl, 0, 4);
        le(smpl, 0, 4);
        chunk(body, "smpl", smpl);
    }
    chunk(body, "data", data);
    std::vector<u8> b = {'R', 'I', 'F', 'F'};
    le(b, static_cast<u32>(body.size()), 4);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

// dr_mp3's whole output for a file (interleaved).
std::vector<i16> raw_decode(const std::vector<u8> &bytes, u32 *channels, u32 *rate) {
    drmp3_config cfg{};
    drmp3_uint64 frames = 0;
    drmp3_int16 *p = drmp3_open_memory_and_read_pcm_frames_s16(bytes.data(), bytes.size(), &cfg,
                                                               &frames, nullptr);
    REQUIRE(p != nullptr);
    std::vector<i16> s(p, p + frames * cfg.channels);
    drmp3_free(p, nullptr);
    *channels = cfg.channels;
    *rate = cfg.sampleRate;
    return s;
}

std::vector<float> render(u32 frames) {
    std::vector<float> out(std::size_t{frames} * 2, 123.0f);
    mt_audio_render(out.data(), frames);
    return out;
}

// What the mixer gives for output frame k of a source (ADR 0045's rule, written out again).
std::int32_t expected(const std::vector<i16> &s, u32 channels, u32 ch, u64 step, u64 k) {
    const u64 pos = k * step;
    const u64 i = pos >> 32;
    const std::int64_t frac = static_cast<std::int64_t>(pos & 0xFFFFFFFFu);
    const u64 frames = s.size() / channels;
    const std::int64_t s0 = i < frames ? s[i * channels + ch] : 0;
    const std::int64_t s1 = i + 1 < frames ? s[(i + 1) * channels + ch] : 0;
    return static_cast<std::int32_t>(s0 + (((s1 - s0) * frac) >> 32));
}

// The host's gains at 1, unmuted, gapless off; and no pack, before and after each test. The
// runtime's log lines and its audio trace go to the fixture's log.
struct Clean {
    Clean() {
        restore();
        mt_diag_config cfg = mt_diag_config_default();
        cfg.log = capture;
        cfg.trace = "audio";
        mt_diag_configure(&cfg);
    }
    ~Clean() {
        restore();
        mt_diag_reset();
    }
    Clean(const Clean &) = delete;
    Clean &operator=(const Clean &) = delete;
    static void restore() {
        mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 1.0f);
        mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 1.0f);
        mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, 1.0f);
        mt_audio_set_muted(false);
        mt_audio_set_mono(false);
        mt_audio_set_gapless_music(false);
        mt_audio_pack_close();
    }
};

// An empty pack directory, made fresh.
fs::path pack_dir() {
    const fs::path p = test_dir() / "pack";
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// The game's MyGetOpenALAudioData on Documents/<name>: the 16-bit samples, channels and rate
// (empty if a call fails).
struct Decoded {
    std::vector<i16> samples;
    u32 channels = 0, rate = 0;
};
Decoded game_decode(Env &e, const char *name) {
    Decoded d;
    const u32 out = e.alloc(4), asbd = e.alloc(40), size = e.alloc(4), frames64 = e.alloc(8),
              list = e.alloc(16), io = e.alloc(4);
    if (e.call(mt_hle_ExtAudioFileOpenURL,
               {e.home_url((std::string("Documents/") + name).c_str()), out}) != 0) {
        return d;
    }
    const u32 file = e.get32(out);
    e.put32(size, 40);
    REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty, {file, 0x66666D74u /* ffmt */, size, asbd}) ==
            0);
    double rate = 0;
    std::memcpy(&rate, e.cpu.mem + asbd, 8);
    const u32 channels = e.get32(asbd + 28);
    // the client format: lpcm, signed | packed, 16 bits, the file's channels and rate
    const u32 words[8] = {0x6C70636Du, 0xCu, 2 * channels, 1, 2 * channels, channels, 16, 0};
    std::memcpy(e.cpu.mem + asbd + 8, words, sizeof words);
    REQUIRE(e.call(mt_hle_ExtAudioFileSetProperty, {file, 0x63666D74u /* cfmt */, 40, asbd}) == 0);
    e.put32(size, 8);
    REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty,
                   {file, 0x2366726Du /* #frm */, size, frames64}) == 0);
    u64 n = 0;
    std::memcpy(&n, e.cpu.mem + frames64, 8);
    const u32 bytes = static_cast<u32>(n) * 2 * channels;
    const u32 pcm = e.alloc(bytes + 4);
    e.put32(list, 1);
    e.put32(list + 4, channels);
    e.put32(list + 8, bytes);
    e.put32(list + 12, pcm);
    e.put32(io, static_cast<u32>(n));
    REQUIRE(e.call(mt_hle_ExtAudioFileRead, {file, io, list}) == 0);
    CHECK(e.get32(io) == n);
    REQUIRE(e.call(mt_hle_ExtAudioFileDispose, {file}) == 0);
    d.samples.resize(static_cast<std::size_t>(n) * channels);
    std::memcpy(d.samples.data(), e.cpu.mem + pcm, bytes);
    d.channels = channels;
    d.rate = static_cast<u32>(rate);
    return d;
}

// CMSoundPlayer's start-up for one effect and its play: alBufferData from the decode, then
// alSourcePlay.
void play_effect(Env &e, const Decoded &d) {
    const u32 device = e.call(mt_hle_alcOpenDevice, {0});
    const u32 context = e.call(mt_hle_alcCreateContext, {device, 0});
    REQUIRE(e.call(mt_hle_alcMakeContextCurrent, {context}) == 1);
    const u32 names = e.alloc(8);
    e.call(mt_hle_alGenBuffers, {1, names});
    e.call(mt_hle_alGenSources, {1, names + 4});
    const u32 bytes = static_cast<u32>(d.samples.size() * 2);
    const u32 p = e.bytes(d.samples.data(), bytes);
    e.call(mt_hle_alBufferData,
           {e.get32(names), d.channels == 2 ? 0x1103u : 0x1101u, p, bytes, d.rate});
    e.call(mt_hle_alSourcei, {e.get32(names + 4), 0x1009u, e.get32(names)});
    REQUIRE(e.call(mt_hle_alGetError, {}) == 0);
    e.call(mt_hle_alSourcePlay, {e.get32(names + 4)});
}

// The mixer's output `out` is `s` (channels, rate) from source frame 0 at gain 1, through the
// effect or music voice's resampling; a mono source plays in both channels.
bool mixes_as(const std::vector<float> &out, const std::vector<i16> &s, u32 channels, u32 rate,
              u64 frames) {
    const u64 step = mix::step_for(rate);
    for (u64 k = 0; k < frames; k++) {
        for (u32 ch = 0; ch < 2; ch++) {
            const float want =
                static_cast<float>(expected(s, channels, channels == 2 ? ch : 0, step, k)) /
                32768.0f;
            if (out[2 * k + ch] != want) {
                FAIL_CHECK("frame " << k << " channel " << ch << ": " << out[2 * k + ch]
                                    << " != " << want);
                return false;
            }
        }
    }
    return true;
}

// A player for Documents/<name> as CMSoundPlayer::LoadBGM makes one (volume 1 here, so the
// output is the stream's samples), then Play.
u32 play_track(Env &e, const char *name) {
    const u32 p = e.send(e.send(e.klass("AVAudioPlayer"), "alloc"), "initWithContentsOfURL:error:",
                         {e.home_url((std::string("Documents/") + name).c_str()), 0});
    REQUIRE(p != 0);
    e.send(p, "setVolume:", {fb(1.0f)});
    e.send(p, "setNumberOfLoops:", {static_cast<u32>(-1)});
    e.send(p, "prepareToPlay");
    CHECK(e.send(p, "play") == 1);
    return p;
}

// `pass` (stereo 44.1 kHz) looped: frames 0..end, then [start, end) again and again.
std::vector<i16> looped(const std::vector<i16> &pass, u64 start, u64 end, u64 frames) {
    std::vector<i16> s;
    u64 at = 0;
    for (u64 k = 0; k < frames; k++) {
        s.push_back(pass[2 * at]);
        s.push_back(pass[2 * at + 1]);
        at = at + 1 < end ? at + 1 : start;
    }
    return s;
}

} // namespace

TEST_CASE("audio.pack: a pack that is no directory is no pack") {
    Clean clean;
    Env e;
    CHECK(mt_audio_pack_open((test_dir() / "no-such-pack").string().c_str()) == -1);
    CHECK(logged("is no directory: no pack"));
    CHECK(mt_audio_pack_open("") == -1);
    CHECK(mt_audio_pack_open(nullptr) == -1);
}

TEST_CASE("audio.pack: a generated tone in place of an effect plays sample for sample") {
    Clean clean;
    Env e;
    const std::vector<i16> game = tone(800, 1, 31);
    write_file(test_dir() / "Documents" / "12.wav", make_wav(game, 8000, 1));
    const std::vector<i16> mine = tone(2000, 2, 5);
    const fs::path dir = pack_dir();
    write_file(dir / "12.WAV", make_wav(mine, 22050, 2)); // any case; stereo for mono
    write_text(dir / "notes.txt", "not a sound");
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
    CHECK(mt_audio_pack_get_stats().files == 1);
    const Decoded d = game_decode(e, "12.wav");
    CHECK(d.channels == 2);
    CHECK(d.rate == 22050);
    CHECK(d.samples == mine);
    play_effect(e, d);
    const u64 frames = 2 * 2000 + 10; // 22,050 Hz: two output frames a source frame, then past
    CHECK(mixes_as(render(static_cast<u32>(frames)), mine, 2, 22050, frames));
    const mt_audio_pack_stats st = mt_audio_pack_get_stats();
    CHECK(st.effects == 1);
    CHECK(st.effects_replaced == 1);
    CHECK(st.rejected == 0);
    // a name the pack has no file for: the game's file
    write_file(test_dir() / "Documents" / "13.wav", make_wav(game, 8000, 1));
    CHECK(game_decode(e, "13.wav").samples == game);
    CHECK(mt_audio_pack_get_stats().effects == 2);
    CHECK(mt_audio_pack_get_stats().effects_replaced == 1);
    // closed: the game's file again
    mt_audio_pack_close();
    CHECK(game_decode(e, "12.wav").samples == game);
}

TEST_CASE("audio.pack: an MP3 in place of an effect is decoded whole, on its own timeline") {
    Clean clean;
    Env e;
    write_file(test_dir() / "Documents" / "20.wav", make_wav(tone(100, 1), 8000, 1));
    const std::vector<u8> mp3 = file_bytes(k_data / "tone22.mp3");
    const fs::path dir = pack_dir();
    write_file(dir / "20.mp3", mp3);
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
    u32 channels = 0, rate = 0;
    const std::vector<i16> want = raw_decode(mp3, &channels, &rate);
    const Decoded d = game_decode(e, "20.wav");
    CHECK(d.rate == rate);
    CHECK(d.channels == channels);
    CHECK(d.samples == want); // every decoded frame, none dropped (no Apple decoder delay)
    play_effect(e, d);
    const u64 frames = 1000;
    CHECK(mixes_as(render(static_cast<u32>(frames)), want, channels, rate, frames));
}

TEST_CASE("audio.pack: a generated tone in place of a track plays sample for sample, looped") {
    Clean clean;
    Env e;
    write_file(test_dir() / "Documents" / "0.mp3", file_bytes(k_data / "tone44.mp3"));
    const std::vector<i16> mine = tone(3000, 2, 11);
    const fs::path dir = pack_dir();
    write_file(dir / "0.wav", make_wav(mine, 44100, 2)); // .wav for the game's .mp3
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
    const u32 p = play_track(e, "0.mp3");
    CHECK(logged("from the audio pack: "));
    // 44.1 kHz: step 1; the whole file, then from its first frame again (numberOfLoops -1)
    const u64 frames = 3000 * 2 + 700;
    CHECK(mixes_as(render(static_cast<u32>(frames)), looped(mine, 0, 3000, frames), 2, 44100,
                   frames));
    CHECK(mt_audio_pack_get_stats().music == 1);
    CHECK(mt_audio_pack_get_stats().music_replaced == 1);
    // stop and play: on from the exact frame (as the game's tracks)
    e.send(p, "stop");
    e.send(p, "play");
    const std::vector<float> out = render(100);
    const std::vector<i16> again = looped(mine, 0, 3000, frames + 100);
    CHECK(mixes_as(out, std::vector<i16>(again.end() - 200, again.end()), 2, 44100, 100));
    mt_objc_release(&e.cpu, p);
}

TEST_CASE("audio.pack: an MP3 in place of a track plays its decoder's frames from the first") {
    Clean clean;
    Env e;
    write_file(test_dir() / "Documents" / "31.mp3", file_bytes(k_data / "tone44.mp3"));
    const std::vector<u8> mp3 = file_bytes(k_data / "gap44.mp3");
    const fs::path dir = pack_dir();
    write_file(dir / "31.mp3", mp3);
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
    u32 channels = 0, rate = 0;
    const std::vector<i16> pass = raw_decode(mp3, &channels, &rate);
    REQUIRE(channels == 2);
    REQUIRE(rate == 44100);
    const u64 n = pass.size() / 2;
    const u32 p = play_track(e, "31.mp3");
    const u64 frames = n + 3000;
    CHECK(mixes_as(render(static_cast<u32>(frames)), looped(pass, 0, n, frames), 2, 44100, frames));
    mt_objc_release(&e.cpu, p);
}

TEST_CASE("audio.pack: loop points from a .loop file or a WAV's smpl chunk") {
    Clean clean;
    Env e;
    write_file(test_dir() / "Documents" / "5.mp3", file_bytes(k_data / "tone44.mp3"));
    const std::vector<i16> mine = tone(4000, 2, 3);
    const fs::path dir = pack_dir();
    SUBCASE(".loop: frames 0 to 3000, then 1000 to 3000 again and again") {
        write_file(dir / "5.wav", make_wav(mine, 44100, 2));
        write_text(dir / "5.loop", "# start end\n1000 3000\n");
        REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
        CHECK(mt_audio_pack_get_stats().loops == 1);
        const u32 p = play_track(e, "5.mp3");
        CHECK(logged("loop 1000 to 3000"));
        const u64 frames = 3000 + 2000 * 3 + 50;
        CHECK(mixes_as(render(static_cast<u32>(frames)), looped(mine, 1000, 3000, frames), 2, 44100,
                       frames));
        mt_objc_release(&e.cpu, p);
    }
    SUBCASE("smpl: the first loop, its end sample played") {
        write_file(dir / "5.wav", make_wav(mine, 44100, 2, 500, 1499));
        REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
        const u32 p = play_track(e, "5.mp3");
        const u64 frames = 1500 + 1000 * 4;
        CHECK(mixes_as(render(static_cast<u32>(frames)), looped(mine, 500, 1500, frames), 2, 44100,
                       frames));
        mt_objc_release(&e.cpu, p);
    }
    SUBCASE("a .loop file wins over smpl; an MP3 loops by its .loop too") {
        write_file(dir / "5.wav", make_wav(mine, 44100, 2, 500, 1499));
        write_text(dir / "5.loop", "2000, 2500");
        REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
        const u32 p = play_track(e, "5.mp3");
        const u64 frames = 2500 + 500 * 3;
        CHECK(mixes_as(render(static_cast<u32>(frames)), looped(mine, 2000, 2500, frames), 2, 44100,
                       frames));
        mt_objc_release(&e.cpu, p);
        const std::vector<u8> mp3 = file_bytes(k_data / "gap44.mp3");
        write_file(dir / "6.mp3", mp3);
        write_text(dir / "6.loop", "100 9000");
        REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 2);
        write_file(test_dir() / "Documents" / "6.mp3", file_bytes(k_data / "tone44.mp3"));
        u32 channels = 0, rate = 0;
        const std::vector<i16> pass = raw_decode(mp3, &channels, &rate);
        const u32 q = play_track(e, "6.mp3");
        const u64 more = 9000 + 8900 * 2 + 10;
        CHECK(mixes_as(render(static_cast<u32>(more)), looped(pass, 100, 9000, more), 2, 44100,
                       more));
        mt_objc_release(&e.cpu, q);
    }
    SUBCASE("a bad .loop is logged and ignored; one past the file's end too") {
        write_file(dir / "5.wav", make_wav(mine, 44100, 2));
        write_text(dir / "5.loop", "3000 1000");
        write_file(dir / "7.wav", make_wav(mine, 44100, 2));
        write_text(dir / "7.loop", "5000 9000");
        write_file(test_dir() / "Documents" / "7.mp3", file_bytes(k_data / "tone44.mp3"));
        REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 2);
        const u32 p = play_track(e, "5.mp3");
        CHECK(logged("5.loop is not \"start end\""));
        const u64 frames = 4000 * 2 + 10;
        CHECK(mixes_as(render(static_cast<u32>(frames)), looped(mine, 0, 4000, frames), 2, 44100,
                       frames));
        e.send(p, "stop");
        const u32 q = play_track(e, "7.mp3");
        CHECK(logged("is outside its 4000 frames: no loop"));
        mt_objc_release(&e.cpu, q);
        mt_objc_release(&e.cpu, p);
    }
}

TEST_CASE("audio.pack: gapless_music trims a pack file without a loop as the game's") {
    Clean clean;
    Env e;
    write_file(test_dir() / "Documents" / "3.mp3", file_bytes(k_data / "tone44.mp3"));
    std::vector<i16> mine(2 * 1000, 0);
    const std::vector<i16> t = tone(600, 2, 1);
    std::copy(t.begin(), t.end(), mine.begin() + 2 * 200); // 200 silent frames, 600, 200
    mine[2 * 200] = mine[2 * 200 + 1] = 5;                 // the first sound is frame 200
    mine[2 * 799] = 0;
    mine[2 * 799 + 1] = 7; // the last frame with a sample other than 0 is 799
    const fs::path dir = pack_dir();
    write_file(dir / "3.wav", make_wav(mine, 44100, 2));
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 1);
    mt_audio_set_gapless_music(true);
    const u32 p = play_track(e, "3.mp3");
    const std::vector<i16> trimmed(mine.begin() + 2 * 200, mine.begin() + 2 * 800);
    const u64 frames = 600 * 3;
    CHECK(mixes_as(render(static_cast<u32>(frames)), looped(trimmed, 0, 600, frames), 2, 44100,
                   frames));
    mt_objc_release(&e.cpu, p);
}

TEST_CASE("audio.pack: corrupt, oversize and out-of-limit files fall back to the game's, logged "
          "once") {
    Clean clean;
    Env e;
    const std::vector<i16> game = tone(500, 1, 9);
    const fs::path dir = pack_dir();
    struct Bad {
        const char *name; // the pack's file, in place of the game's <stem>.wav
        std::vector<u8> bytes;
        const char *why;
    };
    std::vector<u8> cut = make_wav(tone(100, 1), 22050, 1);
    cut.resize(30); // in the fmt chunk
    std::vector<u8> garbage(5000);
    std::mt19937 rng(188);
    for (u8 &b : garbage) {
        b = static_cast<u8>(rng());
    }
    std::vector<u8> mp3_cut = file_bytes(k_data / "tone22.mp3");
    mp3_cut.resize(3);
    const Bad bad[] = {
        {"7.wav", garbage, "is no PCM WAV"},
        {"8.wav", cut, "is no PCM WAV"},
        {"9.wav", make_wav(tone(100, 3), 22050, 3), "has 3 channels (1 or 2)"},
        {"10.wav", make_wav(tone(100, 1), 4000, 1), "is at 4000 Hz (8000 to 192000)"},
        {"11.wav", make_wav(tone(8000 * 60 + 1, 1), 8000, 1), "is longer than 60 s"},
        {"14.wav", {}, "is empty"},
        {"15.mp3", garbage, "holds no MP3 audio"},
        {"16.mp3", mp3_cut, "holds no MP3 audio"},
        {"19.flac", garbage, "holds no FLAC audio"},
        {"17.wav", make_wav({}, 22050, 1), "holds no samples"},
    };
    for (const Bad &b : bad) {
        write_file(dir / b.name, b.bytes);
    }
    // larger than the limit: never read (a sparse file)
    {
        std::ofstream(dir / "18.wav", std::ios::binary).put('R');
        fs::resize_file(dir / "18.wav", std::uintmax_t{MT_AUDIO_PACK_MAX_FILE} + 1);
    }
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == static_cast<int>(std::size(bad)) + 1);
    const auto check_falls_back = [&](const char *pack_name, const char *why) {
        CAPTURE(pack_name);
        const std::string stem = fs::path(pack_name).stem().string();
        const std::string game_name = stem + ".wav";
        write_file(test_dir() / "Documents" / game_name, make_wav(game, 8000, 1));
        for (int twice = 0; twice < 2; twice++) {
            const Decoded d = game_decode(e, game_name.c_str());
            CHECK(d.samples == game);
            CHECK(d.rate == 8000);
        }
        const std::string line = std::string(pack_name) + " " + why;
        CHECK(count_logged(line.c_str()) == 1); // once, though asked twice
        CHECK(count_logged((std::string("rejected, the game's ") + game_name + " kept").c_str()) ==
              1);
    };
    for (const Bad &b : bad) {
        check_falls_back(b.name, b.why);
    }
    check_falls_back("18.wav", "is larger than 128 MiB");
    const mt_audio_pack_stats st = mt_audio_pack_get_stats();
    CHECK(st.effects_replaced == 0);
    CHECK(st.rejected == std::size(bad) + 1);
    // a track: a corrupt pack file plays the game's MP3, as without a pack
    write_file(test_dir() / "Documents" / "2.mp3", file_bytes(k_data / "tone44.mp3"));
    write_file(dir / "2.mp3", garbage);
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) > 0);
    const u32 p = play_track(e, "2.mp3");
    CHECK(logged("2.mp3 holds no MP3 audio"));
    CHECK(mt_audio_pack_get_stats().music_replaced == 0);
    mt_objc_release(&e.cpu, p);
}

TEST_CASE("audio.pack: random mutations of a WAV, an MP3 and a FLAC never read out of bounds") {
    Clean clean;
    Env e;
    const std::vector<u8> good[3] = {make_wav(tone(300, 2), 22050, 2, 10, 200),
                                     file_bytes(k_data / "tone22.mp3"),
                                     file_bytes(k_data / "ramp44.flac")};
    const char *const ext[3] = {".wav", ".mp3", ".flac"};
    const fs::path dir = pack_dir();
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 0);
    std::mt19937 rng(0xA0D10u);
    u32 replaced = 0, rejected = 0, streams = 0;
    constexpr int k_rounds = 450;
    for (int round = 0; round < k_rounds; round++) {
        const int format = round % 3;
        std::vector<u8> b = good[format];
        if ((round / 3) % 2 == 0) {
            b.resize(rng() % b.size()); // cut anywhere
        } else {
            for (int k = 0; k < 8; k++) { // bytes changed anywhere, chunk sizes among them
                b[rng() % b.size()] = static_cast<u8>(rng());
            }
        }
        const std::string name = std::to_string(round) + ext[format];
        write_file(dir / name, b);
        REQUIRE(mt_audio_pack_open(dir.string().c_str()) == round + 1);
        mt_audio::pack::Sound s;
        const bool ok = mt_audio::pack::effect("/x/sound/" + std::to_string(round) + ".wav", &s);
        if (ok) {
            CHECK(s.channels >= 1);
            CHECK(s.channels <= 2);
            CHECK(s.samples.size() % s.channels == 0);
            replaced++;
        } else {
            rejected++;
        }
        REQUIRE(mt_audio_pack_open(dir.string().c_str()) == round + 1); // nothing marked bad
        std::string from;
        const auto stream =
            mt_audio::pack::music("/x/sound/" + std::to_string(round) + ".mp3", false, &from);
        if (stream) {
            streams++;
            std::vector<i16> buf(std::size_t{4096} * stream->channels());
            stream->set_loops(1);
            stream->seek(stream->length() / 2);
            while (stream->read(buf.data(), 4096) == 4096) {
            }
        }
    }
    CHECK(replaced + rejected == k_rounds);
    CHECK(rejected > 0);
    CHECK(replaced > 0);
    MESSAGE(replaced << " decoded, " << rejected << " rejected, " << streams << " streams");
}

TEST_CASE("audio.pack: a FLAC in place of a track or an effect decodes losslessly") {
    Clean clean;
    Env e;
    write_file(test_dir() / "Documents" / "4.mp3", file_bytes(k_data / "tone44.mp3"));
    write_file(test_dir() / "Documents" / "21.wav", make_wav(tone(100, 1), 8000, 1));
    const fs::path dir = pack_dir();
    write_file(dir / "4.flac", file_bytes(k_data / "ramp44.flac"));
    write_file(dir / "21.FLAC", file_bytes(k_data / "ramp48_24.flac"));
    REQUIRE(mt_audio_pack_open(dir.string().c_str()) == 2);
    // the track: 16-bit stereo, 44.1 kHz, the triangles themselves, looped
    const std::vector<i16> pass = tone(13230, 2);
    const u32 p = play_track(e, "4.mp3");
    const u64 frames = 13230 + 5000;
    CHECK(mixes_as(render(static_cast<u32>(frames)), looped(pass, 0, 13230, frames), 2, 44100,
                   frames));
    mt_objc_release(&e.cpu, p);
    // the effect: 24-bit mono at 48 kHz, its top 16 bits
    const Decoded d = game_decode(e, "21.wav");
    CHECK(d.rate == 48000);
    CHECK(d.channels == 1);
    CHECK(d.samples == tone(4800, 1));
    const mt_audio_pack_stats st = mt_audio_pack_get_stats();
    CHECK(st.music_replaced == 1);
    CHECK(st.effects_replaced == 1);
}

TEST_CASE("audio.pack: without a pack the trace and the samples are the game's") {
    Clean clean;
    Env e;
    write_file(test_dir() / "Documents" / "12.wav", make_wav(tone(200, 1), 8000, 1));
    CHECK(game_decode(e, "12.wav").samples == tone(200, 1));
    CHECK(mt_audio_pack_get_stats().effects == 0);
    CHECK(!logged("audio pack"));
    CHECK(!logged("audiopack"));
}
