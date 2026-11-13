// AVAudioPlayer's MP3 music (#63, ADR 0046) on synthetic MP3s only: tests/cpp/data/audio/
// tone44.mp3 and tone22.mp3, our own tones encoded by LAME like the game's two batches
// (make_tones.py there). The stream: its rate, channels and length (MP3 frames x samples per
// frame), its timeline (dr_mp3's output from frame 529 on, then 529 frames of silence), the
// same decoded samples on every host (a pinned hash), the tone itself, numberOfLoops, seeking
// and restarting, files that are no MP3. The player, through the ObjC runtime and the mixer:
// the game's LoadBGM and Play sequences, sample for sample at 44.1 kHz and resampled at
// 22.05 kHz, looping past the end, -stop keeping the exact frame, setCurrentTime:, volume
// (music off is volume 0 and keeps playing), a player that finishes, and its release.
// Gapless loops (#125, ADR 0062, off by default) on gap44.mp3 (silence, a tone, silence) and
// silence44.mp3: the trimmed pass, its loops, seeks and the player on it; off changes nothing.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_internal.h"
#include "audio/mixer.h"
#include "audio/mp3_stream.h"
#include "foundation/foundation.h"
#include "gles/gles_internal.h" // the fixture's GL calls
#include "maplethief/audio.h"
#include "snapshot/snapshot_internal.h"

#include <dr_mp3.h> // the reference decode: the runtime's own dr_mp3 (mp3_stream.cpp)

#include "gles_fixture.h"

namespace {

using i16 = std::int16_t;
using u64 = std::uint64_t;
namespace mix = mt_audio::mix;
using mt_audio::Mp3Stream;

const fs::path k_data = MT_AUDIO_DATA_DIR;

std::vector<u8> file_bytes(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

// dr_mp3's whole output for a file, as dr_mp3 gives it (interleaved).
struct Raw {
    std::vector<i16> samples;
    u32 channels = 0, rate = 0;
    u64 frames = 0;
};
Raw raw_decode(const std::vector<u8> &bytes) {
    drmp3_config cfg{};
    drmp3_uint64 frames = 0;
    drmp3_int16 *p = drmp3_open_memory_and_read_pcm_frames_s16(bytes.data(), bytes.size(), &cfg,
                                                               &frames, nullptr);
    REQUIRE(p != nullptr);
    Raw r;
    r.channels = cfg.channels;
    r.rate = cfg.sampleRate;
    r.frames = frames;
    r.samples.assign(p, p + frames * cfg.channels);
    drmp3_free(p, nullptr);
    return r;
}

// One pass as the stream must give it (ADR 0046): the decoder's frames from 529 on, then 529
// frames of silence, length = the decoder's frame count.
std::vector<i16> expected_pass(const Raw &r) {
    std::vector<i16> s(r.samples.size(), 0);
    const std::size_t skip = std::size_t{Mp3Stream::kPriming} * r.channels;
    std::copy(r.samples.begin() + static_cast<std::ptrdiff_t>(skip), r.samples.end(), s.begin());
    return s;
}

std::vector<i16> read_all(Mp3Stream &s, u64 frames, u32 block = mix::kMusicBlock) {
    std::vector<i16> out;
    std::vector<i16> buf(std::size_t{block} * s.channels());
    u64 left = frames;
    while (left > 0) {
        const u32 want = static_cast<u32>(std::min<u64>(left, block));
        const u32 got = s.read(buf.data(), want);
        out.insert(out.end(), buf.begin(), buf.begin() + std::size_t{got} * s.channels());
        left -= got;
        if (got < want) {
            break;
        }
    }
    return out;
}

u64 fnv1a(const std::vector<i16> &s) {
    u64 h = 14695981039346656037ull;
    for (const i16 v : s) {
        for (const int shift : {0, 8}) {
            h ^= static_cast<u8>(static_cast<std::uint16_t>(v) >> shift);
            h *= 1099511628211ull;
        }
    }
    return h;
}

// Upward zero crossings of channel `ch` over frames [from, to).
u32 crossings(const std::vector<i16> &s, u32 channels, u32 ch, u64 from, u64 to) {
    u32 n = 0;
    for (u64 i = from + 1; i < to; i++) {
        n += s[(i - 1) * channels + ch] < 0 && s[i * channels + ch] >= 0 ? 1u : 0u;
    }
    return n;
}

struct Fixture {
    const char *name;
    u32 rate;
    u64 frames; // MP3 frames x samples per frame, by mtport.audio's frame walk
    u64 hash;   // FNV-1a of dr_mp3's whole output (DR_MP3_NO_SIMD, no contraction)
};
const Fixture k_fixtures[] = {
    {"tone44.mp3", 44100, 13 * 1152, 0x1203D6C41763DBABull},
    {"tone22.mp3", 22050, 14 * 576, 0xE357D6C14C4967B9ull},
};

// --- the stream ----------------------------------------------------------------------------------

TEST_CASE("mp3: the synthetic MP3s open with their rate, channels and length, decoded the same "
          "everywhere") {
    for (const Fixture &fx : k_fixtures) {
        CAPTURE(fx.name);
        const std::vector<u8> bytes = file_bytes(k_data / fx.name);
        const Raw raw = raw_decode(bytes);
        auto s = Mp3Stream::open(bytes);
        REQUIRE(s != nullptr);
        CHECK(s->rate() == fx.rate);
        CHECK(s->channels() == 2);
        CHECK(s->length() == fx.frames);
        CHECK(raw.frames == fx.frames); // no Xing/LAME info: nothing trimmed
        CHECK(fnv1a(raw.samples) == fx.hash);
        // one pass: the decoder's output from frame 529 on, then 529 frames of silence
        s->set_loops(0);
        const std::vector<i16> pass = read_all(*s, fx.frames + 100);
        CHECK(pass == expected_pass(raw));
        CHECK(s->ended());
        CHECK(s->position() == fx.frames);
        // the tone: 440 Hz on the left, 660 Hz on the right, over the middle 0.2 s
        const u64 from = fx.rate / 20, to = from + fx.rate / 5;
        CHECK(crossings(pass, 2, 0, from, to) >= 87);
        CHECK(crossings(pass, 2, 0, from, to) <= 89);
        CHECK(crossings(pass, 2, 1, from, to) >= 131);
        CHECK(crossings(pass, 2, 1, from, to) <= 133);
    }
}

TEST_CASE("mp3: bytes that hold no MP3 audio open no stream") {
    CHECK(Mp3Stream::open({}) == nullptr);
    CHECK(Mp3Stream::open(std::vector<u8>(4096, 0)) == nullptr);
    std::vector<u8> text(2000);
    for (std::size_t i = 0; i < text.size(); i++) {
        text[i] = static_cast<u8>("RIFF....WAVEfmt "[i % 16]);
    }
    CHECK(Mp3Stream::open(text) == nullptr);
    // the ID3v2 tag alone, without the frames after it
    const std::vector<u8> bytes = file_bytes(k_data / "tone44.mp3");
    const u32 tag = 10u + ((u32(bytes[6]) << 21) | (u32(bytes[7]) << 14) | (u32(bytes[8]) << 7) |
                           u32(bytes[9]));
    REQUIRE(tag < bytes.size());
    CHECK(Mp3Stream::open(std::vector<u8>(bytes.begin(), bytes.begin() + tag)) == nullptr);
}

TEST_CASE("mp3: numberOfLoops: 0 plays once, n repeats n times, a negative count for ever") {
    const std::vector<u8> bytes = file_bytes(k_data / "tone44.mp3");
    const std::vector<i16> pass = expected_pass(raw_decode(bytes));
    const u64 n = pass.size() / 2;
    for (const std::int32_t loops : {0, 1, 2}) {
        CAPTURE(loops);
        auto s = Mp3Stream::open(bytes);
        s->set_loops(loops);
        const std::vector<i16> out = read_all(*s, 10 * n, 1000); // blocks across the loop points
        REQUIRE(out.size() == pass.size() * static_cast<std::size_t>(loops + 1));
        for (std::size_t i = 0; i < out.size(); i++) {
            REQUIRE(out[i] == pass[i % pass.size()]);
        }
        CHECK(s->ended());
        CHECK(s->passes_repeated() == static_cast<u32>(loops));
        std::vector<i16> buf(2 * 10);
        CHECK(s->read(buf.data(), 10) == 0); // stays ended
    }
    auto s = Mp3Stream::open(bytes);
    s->set_loops(-1);
    const std::vector<i16> out = read_all(*s, 5 * n + 77);
    REQUIRE(out.size() == 2 * (5 * n + 77));
    for (std::size_t i = 0; i < out.size(); i++) {
        REQUIRE(out[i] == pass[i % pass.size()]);
    }
    CHECK(!s->ended());
    CHECK(s->passes_repeated() == 5);
    CHECK(s->position() == 77);
    // the count can change while it plays: 0 now, so this pass is the last
    s->set_loops(0);
    CHECK(read_all(*s, 10 * n).size() == 2 * (n - 77));
    CHECK(s->ended());
}

TEST_CASE("mp3: seek and restart") {
    const std::vector<u8> bytes = file_bytes(k_data / "tone22.mp3");
    const std::vector<i16> pass = expected_pass(raw_decode(bytes));
    const u64 n = pass.size() / 2;
    auto s = Mp3Stream::open(bytes);
    for (const u64 to : {u64{1000}, u64{0}, u64{n - 529 - 3}, u64{n - 100}, u64{3333}}) {
        CAPTURE(to);
        s->seek(to);
        CHECK(s->position() == to);
        const std::vector<i16> out = read_all(*s, 600);
        REQUIRE(out.size() == std::min<u64>(600, n - to) * 2);
        for (std::size_t i = 0; i < out.size(); i++) {
            REQUIRE(out[i] == pass[2 * to + i]);
        }
    }
    s->seek(n + 1000); // past the end: the end
    CHECK(s->position() == n);
    std::vector<i16> buf(2 * 10);
    CHECK(s->read(buf.data(), 10) == 0);
    CHECK(s->ended());
    s->seek(n - 5); // a seek after the end plays the rest of the pass
    CHECK(!s->ended());
    CHECK(s->read(buf.data(), 10) == 5);
    CHECK(s->ended());
    s->set_loops(1);
    s->restart();
    CHECK(!s->ended());
    CHECK(s->position() == 0);
    CHECK(s->passes_repeated() == 0);
    CHECK(read_all(*s, 3 * n).size() == 2 * 2 * n);
}

// --- gapless loops (#125, ADR 0062) -----------------------------------------------------------

// The frames of a pass that sound: [first, last] with a sample other than 0; false if none.
bool sounding(const std::vector<i16> &pass, u32 channels, u64 *first, u64 *last) {
    const u64 n = pass.size() / channels;
    const auto zero = [&](u64 i) {
        for (u32 c = 0; c < channels; c++) {
            if (pass[i * channels + c] != 0) {
                return false;
            }
        }
        return true;
    };
    u64 a = 0;
    while (a < n && zero(a)) {
        a++;
    }
    if (a == n) {
        return false;
    }
    u64 b = n - 1;
    while (zero(b)) {
        b--;
    }
    *first = a;
    *last = b;
    return true;
}

// The gapless pass the stream must give: Apple's pass from its first to its last sound.
std::vector<i16> trimmed_pass(const std::vector<i16> &pass, u32 channels) {
    u64 first = 0, last = 0;
    REQUIRE(sounding(pass, channels, &first, &last));
    return std::vector<i16>(pass.begin() + static_cast<std::ptrdiff_t>(first * channels),
                            pass.begin() + static_cast<std::ptrdiff_t>((last + 1) * channels));
}

TEST_CASE("mp3: gapless: the pass runs from the file's first sound to its last") {
    const std::vector<u8> bytes = file_bytes(k_data / "gap44.mp3");
    const Raw raw = raw_decode(bytes);
    const std::vector<i16> pass = expected_pass(raw);
    u64 first = 0, last = 0;
    REQUIRE(sounding(pass, 2, &first, &last));
    // The fixture: 0.1 s of silence, 0.2 s of tone, 0.25 s of silence, behind LAME's delay and
    // in front of its padding and the 529-frame tail (an MDCT granule may smear the edges)
    CHECK(first >= 4410 - 576);
    CHECK(first <= 4410 + 1152);
    CHECK(raw.frames - 1 - last >= 11025 + 529 - 1152);
    // Off (the default): the whole pass, as before
    for (const bool off : {false, true}) {
        auto s = off ? Mp3Stream::open(bytes, false) : Mp3Stream::open(bytes);
        REQUIRE(s != nullptr);
        CHECK(s->trim_start() == 0);
        CHECK(s->length() == raw.frames);
        CHECK(s->full_length() == raw.frames);
        CHECK(read_all(*s, raw.frames + 10) == pass);
    }
    // On: frames first .. last of it, no more
    auto s = Mp3Stream::open(bytes, true);
    REQUIRE(s != nullptr);
    CHECK(s->rate() == 44100);
    CHECK(s->channels() == 2);
    CHECK(s->trim_start() == first);
    CHECK(s->length() == last + 1 - first);
    CHECK(s->full_length() == raw.frames);
    CHECK(s->position() == 0);
    const std::vector<i16> out = read_all(*s, raw.frames + 10, 1000);
    CHECK(out == trimmed_pass(pass, 2));
    CHECK(s->ended());
    CHECK(s->position() == s->length());
    // the tone fixtures trim too, from and to a sound
    for (const Fixture &fx : k_fixtures) {
        CAPTURE(fx.name);
        const std::vector<u8> b = file_bytes(k_data / fx.name);
        const std::vector<i16> p = expected_pass(raw_decode(b));
        auto g = Mp3Stream::open(b, true);
        REQUIRE(g != nullptr);
        const std::vector<i16> o = read_all(*g, fx.frames);
        CHECK(o == trimmed_pass(p, 2));
        CHECK(g->full_length() - g->length() >= Mp3Stream::kPriming); // the tail at least
    }
}

TEST_CASE("mp3: gapless: a file that is silent throughout keeps its whole pass") {
    const std::vector<u8> bytes = file_bytes(k_data / "silence44.mp3");
    const Raw raw = raw_decode(bytes);
    u64 first = 0, last = 0;
    CHECK_FALSE(sounding(expected_pass(raw), 2, &first, &last));
    auto s = Mp3Stream::open(bytes, true);
    REQUIRE(s != nullptr);
    CHECK(s->trim_start() == 0);
    CHECK(s->length() == raw.frames);
    s->set_loops(2);
    const std::vector<i16> out = read_all(*s, 10 * raw.frames, 777);
    CHECK(out.size() == 3 * 2 * raw.frames);
    CHECK(s->ended());
}

TEST_CASE("mp3: gapless: loops go from the last sound straight to the first; seeks and "
          "restarts are on the trimmed pass") {
    const std::vector<u8> bytes = file_bytes(k_data / "gap44.mp3");
    const std::vector<i16> pass = trimmed_pass(expected_pass(raw_decode(bytes)), 2);
    const u64 n = pass.size() / 2;
    for (const std::int32_t loops : {0, 1, 2}) {
        CAPTURE(loops);
        auto s = Mp3Stream::open(bytes, true);
        s->set_loops(loops);
        const std::vector<i16> out = read_all(*s, 10 * n, 1000);
        REQUIRE(out.size() == pass.size() * static_cast<std::size_t>(loops + 1));
        for (std::size_t i = 0; i < out.size(); i++) {
            REQUIRE(out[i] == pass[i % pass.size()]);
        }
        CHECK(s->ended());
        CHECK(s->passes_repeated() == static_cast<u32>(loops));
    }
    auto s = Mp3Stream::open(bytes, true);
    s->set_loops(-1);
    const std::vector<i16> out = read_all(*s, 4 * n + 55, 1000);
    REQUIRE(out.size() == 2 * (4 * n + 55));
    for (std::size_t i = 0; i < out.size(); i++) {
        REQUIRE(out[i] == pass[i % pass.size()]);
    }
    CHECK(s->passes_repeated() == 4);
    CHECK(s->position() == 55);
    // no silent frame on either side of a loop point
    for (u64 k = 1; k < 4; k++) {
        const std::size_t at = static_cast<std::size_t>(2 * k * n);
        CHECK((out[at - 2] != 0 || out[at - 1] != 0));
        CHECK((out[at] != 0 || out[at + 1] != 0));
    }
    // seeks: frames of the trimmed pass
    for (const u64 to : {u64{1000}, u64{0}, u64{n - 3}, u64{3333}}) {
        CAPTURE(to);
        s->seek(to);
        CHECK(s->position() == to);
        s->set_loops(0);
        const std::vector<i16> o = read_all(*s, 600);
        REQUIRE(o.size() == std::min<u64>(600, n - to) * 2);
        for (std::size_t i = 0; i < o.size(); i++) {
            REQUIRE(o[i] == pass[2 * to + i]);
        }
    }
    s->seek(n + 1000);
    CHECK(s->position() == n);
    s->restart();
    CHECK(s->position() == 0);
    CHECK(read_all(*s, 100) == std::vector<i16>(pass.begin(), pass.begin() + 200));
}

// --- AVAudioPlayer through the runtime and the mixer -------------------------------------------

std::vector<float> render(u32 frames) {
    std::vector<float> out(std::size_t{frames} * 2, 123.0f);
    mt_audio_render(out.data(), frames);
    return out;
}

bool silent(const std::vector<float> &out) {
    return std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; });
}

float f(std::int32_t sample) { return static_cast<float>(sample) / 32768.0f; }

void write_file(const fs::path &p, const std::vector<u8> &data) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

// A player for Documents/<name> the way CMSoundPlayer::LoadBGM makes one, and its Play.
struct Player {
    Env &e;
    u32 self = 0;
    Player(Env &env, const char *name, float volume = 0.7f) : e(env) {
        self = e.send(e.send(e.klass("AVAudioPlayer"), "alloc"),
                      "initWithContentsOfURL:error:", {e.home_url(name), 0});
        REQUIRE(self != 0);
        e.send(self, "setVolume:", {fb(volume)});
        e.send(self, "setNumberOfLoops:", {static_cast<u32>(-1)});
        CHECK(e.send(self, "prepareToPlay") == 1);
    }
    bool playing() { return (e.send(self, "isPlaying") & 0xFFu) != 0; }
    void set_time(double t) {
        u64 b;
        std::memcpy(&b, &t, 8);
        e.send(self, "setCurrentTime:", {static_cast<u32>(b), static_cast<u32>(b >> 32)});
    }
    // CMSoundPlayer::Play(id, resume) for this track, music on (0.7) or off (0)
    void game_play(bool resume, bool music_on = true) {
        if (playing()) {
            e.send(self, "stop");
        }
        if (!resume) {
            set_time(0.0);
        }
        e.send(self, "setVolume:", {fb(music_on ? 0.7f : 0.0f)});
        CHECK(e.send(self, "play") == 1);
    }
};

struct Buses {
    Buses() { restore(); }
    ~Buses() { restore(); }
    static void restore() {
        mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 1.0f);
        mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 1.0f);
        mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, 1.0f);
        mt_audio_set_muted(false);
    }
};

void put_fixtures() {
    for (const Fixture &fx : k_fixtures) {
        write_file(test_dir() / "Documents" / fx.name, file_bytes(k_data / fx.name));
    }
    write_file(test_dir() / "Documents" / "gap44.mp3", file_bytes(k_data / "gap44.mp3"));
}

// mt_audio_set_gapless_music for one test, off again after it.
struct Gapless {
    explicit Gapless(bool on) { mt_audio_set_gapless_music(on); }
    ~Gapless() { mt_audio_set_gapless_music(false); }
    Gapless(const Gapless &) = delete;
    Gapless &operator=(const Gapless &) = delete;
};

TEST_CASE("mp3: AVAudioPlayer opens MP3s only") {
    Env e;
    put_fixtures();
    write_file(test_dir() / "Documents" / "zeros.mp3", std::vector<u8>(16, 0));
    const auto open = [&](const char *name) {
        return e.send(e.send(e.klass("AVAudioPlayer"), "alloc"),
                      "initWithContentsOfURL:error:", {e.home_url(name), 0});
    };
    CHECK(open("Documents/none.mp3") == 0);
    CHECK(open("Documents/zeros.mp3") == 0);
    CHECK(open("Documents/../tone44.mp3") == 0);
    const u32 p = open("Documents/tone44.mp3");
    REQUIRE(p != 0);
    CHECK(mt_audio_get_stats().players == 1);
    CHECK((e.send(p, "isPlaying") & 0xFFu) == 0);
    mt_objc_release(&e.cpu, p);
}

TEST_CASE("mp3: a save state's player path outside the home directory and the bundle is refused "
          "before it is opened (#324)") {
    Env e;
    put_fixtures();
    // The "audio" section's players, as players_save writes them: one player of `path`.
    const auto load = [](const std::string &path, std::string *why) {
        mt_snap::Writer w;
        w.u32(1);
        w.u32(0x1234u);
        w.str(path);
        w.boolean(false);
        w.u64(0);
        w.u32(0);
        w.boolean(false);
        w.u32(0);
        mt_snap::Reader r(w.bytes().data(), w.bytes().size(), "audio");
        std::map<u32, std::shared_ptr<mt_audio::mix::MusicStream>> streams;
        std::function<void()> apply;
        const bool ok = mt_audio::snapshot::players_load(r, &streams, &apply);
        *why = r.error();
        CHECK(ok == static_cast<bool>(apply));
        CHECK(ok == !streams.empty());
        return ok;
    };
    const std::string inside =
        mt_ns::host_path(mt_ns::home_guest_path() + "/Documents/tone44.mp3", false);
    REQUIRE_FALSE(inside.empty());
    std::string why;
    CHECK(load(inside, &why));
    // A real MP3, outside the data folder: never opened.
    const std::string data = test_dir().string();
    for (const std::string &path :
         {(k_data / "tone44.mp3").string(), data + "/Documents/../../tone44.mp3",
          data + "/Documents/./tone44.mp3", data + "/Documents\\..\\tone44.mp3",
          data + "x/Documents/tone44.mp3", std::string("tone44.mp3")}) {
        CAPTURE(path);
        CHECK_FALSE(load(path, &why));
        CHECK(why.find("is not a file the game can open") != std::string::npos);
    }
}

TEST_CASE("mp3: the game's music calls, sample for sample through the mixer") {
    Env e;
    Buses buses;
    put_fixtures();
    const std::vector<i16> pass =
        expected_pass(raw_decode(file_bytes(k_data / "tone44.mp3"))); // 44.1 kHz: step 1
    const u64 n = pass.size() / 2;
    const auto sample = [&](u64 k, u32 ch) { return pass[2 * (k % n) + ch]; };
    // the music at `volume` from stream frame `first` on, for out's frames
    const auto check_from = [&](const std::vector<float> &out, u64 first, float volume) {
        for (std::size_t k = 0; k < out.size() / 2; k++) {
            for (u32 ch = 0; ch < 2; ch++) {
                const float want = static_cast<float>(sample(first + k, ch)) * (volume / 32768.0f);
                if (std::fabs(out[2 * k + ch] - want) > 1e-6f) {
                    FAIL_CHECK("frame " << k << " channel " << ch << ": " << out[2 * k + ch]
                                        << " != " << want);
                    return;
                }
            }
        }
    };
    Player p(e, "Documents/tone44.mp3");
    CHECK(!p.playing());
    CHECK(silent(render(100))); // prepared, not playing
    p.game_play(false);
    CHECK(p.playing());
    CHECK(mt_audio_get_stats().players_playing == 1);
    CHECK(mt_audio_get_output_stats().music_playing == 1);
    // the first pass and on past its end: it loops (numberOfLoops -1)
    std::vector<float> out = render(static_cast<u32>(n + 5000));
    check_from(out, 0, 0.7f);
    CHECK(p.playing());
    u64 at = n + 5000;
    // StopAllBGM, then Play(resume 1): on from the exact frame
    e.send(p.self, "stop");
    CHECK(!p.playing());
    CHECK(mt_audio_get_stats().players_playing == 0);
    CHECK(silent(render(700)));
    p.game_play(true);
    out = render(3000);
    check_from(out, at, 0.7f);
    at += 3000;
    // MusicOff: volume 0, still playing; MusicOn: 0.7 again, on from where it was
    e.send(p.self, "setVolume:", {fb(0.0f)});
    CHECK(silent(render(1000)));
    CHECK(p.playing());
    at += 1000;
    e.send(p.self, "setVolume:", {fb(0.7f)});
    out = render(500);
    check_from(out, at, 0.7f);
    // Play(resume 0): stop, setCurrentTime:0, play: from the start
    p.game_play(false);
    out = render(2000);
    check_from(out, 0, 0.7f);
    // setCurrentTime: while playing, in seconds (floor(t x rate))
    p.set_time(0.1);
    out = render(1000);
    check_from(out, 4410, 0.7f);
    p.set_time(-3.0); // clamped to the start
    out = render(10);
    check_from(out, 0, 0.7f);
    // full volume and the host's music bus
    e.send(p.self, "setVolume:", {fb(1.0f)});
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 0.5f);
    out = render(100);
    check_from(out, 10, 0.5f);
    mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, 1.0f);
    e.send(p.self, "setVolume:", {fb(7.0f)}); // clamped to 1
    out = render(100);
    check_from(out, 110, 1.0f);
    // released: the voice is gone
    mt_objc_release(&e.cpu, p.self);
    CHECK(mt_audio_get_output_stats().music_playing == 0);
    CHECK(mt_audio_get_stats().players_playing == 0);
    CHECK(silent(render(100)));
    CHECK(!logged("unrecognized selector"));
}

TEST_CASE("mp3: a 22.05 kHz track is resampled like any voice") {
    Env e;
    Buses buses;
    put_fixtures();
    const std::vector<i16> pass = expected_pass(raw_decode(file_bytes(k_data / "tone22.mp3")));
    const u32 n = static_cast<u32>(pass.size() / 2);
    Player p(e, "Documents/tone22.mp3", 1.0f);
    p.game_play(false);
    e.send(p.self, "setVolume:", {fb(1.0f)});
    // two passes and a bit: interpolated across the loop point as well
    std::vector<i16> two(pass);
    two.insert(two.end(), pass.begin(), pass.end());
    two.insert(two.end(), pass.begin(), pass.end());
    const std::uint64_t step = mix::step_for(22050);
    const u32 frames = 2 * (2 * n) + 100;
    const std::vector<float> out = render(frames);
    for (u32 k = 0; k < frames; k++) {
        for (u32 ch = 0; ch < 2; ch++) {
            REQUIRE(out[2 * k + ch] ==
                    f(mix::interpolate(two.data(), 3 * n, 2, ch, std::uint64_t{k} * step)));
        }
    }
    mt_objc_release(&e.cpu, p.self);
}

TEST_CASE("mp3: a player with numberOfLoops 0 finishes, and plays again from the start") {
    Env e;
    Buses buses;
    put_fixtures();
    const std::vector<i16> pass = expected_pass(raw_decode(file_bytes(k_data / "tone44.mp3")));
    const u32 n = static_cast<u32>(pass.size() / 2);
    Player p(e, "Documents/tone44.mp3", 1.0f);
    e.send(p.self, "setNumberOfLoops:", {0});
    CHECK(e.send(p.self, "play") == 1);
    render(n - 10);
    CHECK(p.playing());
    std::vector<float> out = render(20);
    for (u32 k = 0; k < 10; k++) {
        CHECK(out[2 * k] == f(pass[2 * (n - 10 + k)]));
    }
    CHECK(silent(std::vector<float>(out.begin() + 20, out.end())));
    CHECK(!p.playing());
    CHECK(mt_audio_get_stats().players_playing == 0);
    // -play after the end: from the start, as AVAudioPlayer rewinds when it finishes
    CHECK(e.send(p.self, "play") == 1);
    CHECK(p.playing());
    out = render(50);
    for (u32 k = 0; k < 50; k++) {
        REQUIRE(out[2 * k] == f(pass[2 * k]));
    }
    // stopped near the end and played again: it plays out the rest and finishes
    p.set_time((n - 100 + 0.5) / 44100.0); // floor(t x rate) = n - 100
    render(10);
    e.send(p.self, "stop");
    CHECK(e.send(p.self, "play") == 1);
    out = render(200);
    for (u32 k = 0; k < 90; k++) {
        REQUIRE(out[2 * k] == f(pass[2 * (n - 90 + k)]));
    }
    CHECK(!p.playing());
    mt_objc_release(&e.cpu, p.self);
}

TEST_CASE("mp3: two players, one playing at a time as the maps switch") {
    Env e;
    Buses buses;
    put_fixtures();
    Player town(e, "Documents/tone44.mp3");
    Player field(e, "Documents/tone22.mp3");
    town.game_play(false);
    render(1000);
    // StopAllBGM, then the next map's track
    for (Player *p : {&town, &field}) {
        if (p->playing()) {
            e.send(p->self, "stop");
        }
    }
    field.game_play(true);
    CHECK(!town.playing());
    CHECK(field.playing());
    CHECK(mt_audio_get_output_stats().music_playing == 1);
    CHECK(!silent(render(1000)));
    mt_objc_release(&e.cpu, town.self);
    mt_objc_release(&e.cpu, field.self);
}

TEST_CASE("mp3: gapless: the player on the trimmed pass, through the mixer") {
    Env e;
    Buses buses;
    put_fixtures();
    CHECK_FALSE(mt_audio_get_gapless_music()); // the default
    const std::vector<i16> whole = expected_pass(raw_decode(file_bytes(k_data / "gap44.mp3")));
    const std::vector<i16> pass = trimmed_pass(whole, 2);
    const u64 n = pass.size() / 2;
    // out is p (looped) at the player's 0.7 from frame `first` on
    const auto check_from = [](const std::vector<i16> &p, const std::vector<float> &out,
                               u64 first) {
        const u64 len = p.size() / 2;
        for (std::size_t k = 0; k < out.size() / 2; k++) {
            for (u32 ch = 0; ch < 2; ch++) {
                const float want =
                    static_cast<float>(p[2 * ((first + k) % len) + ch]) * (0.7f / 32768.0f);
                if (std::fabs(out[2 * k + ch] - want) > 1e-6f) {
                    FAIL_CHECK("frame " << k << " channel " << ch << ": " << out[2 * k + ch]
                                        << " != " << want);
                    return;
                }
            }
        }
    };
    // Opened while the option is off: the whole pass, kept when the option is set later
    Player before(e, "Documents/gap44.mp3");
    Gapless on(true);
    CHECK(mt_audio_get_gapless_music());
    Player p(e, "Documents/gap44.mp3");
    p.game_play(false);
    // two passes and a bit: from the first sound, the loop straight from the last to the first
    std::vector<float> out = render(static_cast<u32>(2 * n + 5000));
    check_from(pass, out, 0);
    for (const u64 at : {n, 2 * n}) { // no silent frame on either side of the loop points
        CHECK((out[2 * at - 2] != 0.0f || out[2 * at - 1] != 0.0f));
        CHECK((out[2 * at] != 0.0f || out[2 * at + 1] != 0.0f));
    }
    const u64 at = 2 * n + 5000;
    // StopAllBGM and Play(resume 1): on from the exact frame of the trimmed pass
    e.send(p.self, "stop");
    CHECK(silent(render(300)));
    p.game_play(true);
    out = render(2000);
    check_from(pass, out, at);
    // setCurrentTime: seconds into the trimmed pass; Play(resume 0) from its first sound
    p.set_time(0.1);
    out = render(1000);
    check_from(pass, out, 4410);
    p.game_play(false);
    out = render(1000);
    check_from(pass, out, 0);
    e.send(p.self, "stop");
    // the player opened before the option: Apple's whole pass, silence included
    before.game_play(false);
    out = render(static_cast<u32>(whole.size() / 2 + 100));
    check_from(whole, out, 0);
    mt_objc_release(&e.cpu, before.self);
    mt_objc_release(&e.cpu, p.self);
    CHECK(!logged("unrecognized selector"));
}

} // namespace
