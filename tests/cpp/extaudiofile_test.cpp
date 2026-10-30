// ExtAudioFile decoding (#62) on a real guest address space with synthetic WAVs only: the calls
// of Apple's MyGetOpenALAudioData (open, 'ffmt', 'cfmt' = 16-bit signed packed at the file's
// channels and rate, '#frm', one read, dispose) on mono and stereo files at 8,000, 22,050 and
// 44,100 Hz, chunks before and after "data" (odd sizes padded, a longer fmt chunk), a
// truncated data chunk, reads in several pieces, ioNumberFrames and mDataByteSize at the end,
// 8-bit and 24-bit files converted to 16-bit, the client formats that are refused ('fmt?' and
// the rest), bad buffer lists, and the decoded samples through alBufferData into a voice.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "audio/audio_internal.h"
#include "gles/gles_internal.h" // the fixture's GL calls
#include "maplethief/audio.h"

#include "gles_fixture.h"

namespace {

using i16 = std::int16_t;

constexpr u32 k_ffmt = 0x66666D74u, k_cfmt = 0x63666D74u, k_frm = 0x2366726Du;
constexpr u32 k_lpcm = 0x6C70636Du;
constexpr u32 k_fmt_unsupported = 0x666D743Fu;      // 'fmt?'
constexpr u32 k_param = static_cast<u32>(-50);      // paramErr
constexpr u32 k_non_pcm = static_cast<u32>(-66563); // kExtAudioFileError_NonPCMClientFormat
constexpr u32 k_bad_property = static_cast<u32>(-66561);

// A synthetic WAV: `frames` frames of `channels` x `bits`-bit integer PCM at `rate` Hz, sample
// (i, ch) = sample_value(i, ch, bits), with the chunks `before` between "fmt " and "data" and
// `after` it (each padded to an even size), and `fmt_extra` bytes more in the fmt chunk.
struct Wav {
    u32 rate = 22050, channels = 1, bits = 16, frames = 100, tag = 1, fmt_extra = 0;
    std::vector<std::pair<std::string, std::vector<u8>>> before, after;
};

// The raw little-endian sample bytes of (i, ch).
u32 sample_value(u32 i, u32 ch, u32 bits) {
    const u32 v = i * 0x9E3779B1u + ch * 0x7F4A7C15u + 0x1234567u; // well mixed, never all equal
    return bits == 32 ? v : v & ((1u << bits) - 1u);
}
// The same sample as 16-bit signed, converted as the runtime says (the top 16 bits; 8-bit is
// unsigned with 128 as zero).
i16 sample_s16(u32 i, u32 ch, u32 bits) {
    const u32 v = sample_value(i, ch, bits);
    if (bits == 8) {
        return static_cast<i16>((static_cast<int>(v) - 128) * 256);
    }
    return static_cast<i16>(static_cast<std::uint16_t>(v >> (bits - 16)));
}

void put4(std::vector<u8> &b, const char *s) { b.insert(b.end(), s, s + 4); }
void le(std::vector<u8> &b, u32 v, u32 bytes) {
    for (u32 k = 0; k < bytes; k++) {
        b.push_back(static_cast<u8>(v >> (8 * k)));
    }
}
void chunk(std::vector<u8> &b, const std::string &id, const std::vector<u8> &body) {
    put4(b, id.c_str());
    le(b, static_cast<u32>(body.size()), 4);
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() % 2 != 0) {
        b.push_back(0); // the pad byte
    }
}

std::vector<u8> data_bytes(const Wav &w) {
    std::vector<u8> d;
    for (u32 i = 0; i < w.frames; i++) {
        for (u32 ch = 0; ch < w.channels; ch++) {
            le(d, sample_value(i, ch, w.bits), w.bits / 8);
        }
    }
    return d;
}

std::vector<u8> make_wav(const Wav &w) {
    std::vector<u8> fmt;
    const u32 align = w.channels * (w.bits / 8);
    le(fmt, w.tag, 2);
    le(fmt, w.channels, 2);
    le(fmt, w.rate, 4);
    le(fmt, w.rate * align, 4);
    le(fmt, align, 2);
    le(fmt, w.bits, 2);
    if (w.fmt_extra != 0) {
        le(fmt, w.fmt_extra - 2, 2); // cbSize, then that many bytes
        fmt.insert(fmt.end(), w.fmt_extra - 2, 0xEE);
    }
    std::vector<u8> body;
    put4(body, "WAVE");
    chunk(body, "fmt ", fmt);
    for (const auto &c : w.before) {
        chunk(body, c.first, c.second);
    }
    chunk(body, "data", data_bytes(w));
    for (const auto &c : w.after) {
        chunk(body, c.first, c.second);
    }
    std::vector<u8> b;
    put4(b, "RIFF");
    le(b, static_cast<u32>(body.size()), 4);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

void write_file(const fs::path &path, const std::vector<u8> &bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// An AudioStreamBasicDescription in guest memory.
struct Asbd {
    double rate = 0;
    u32 format = k_lpcm, flags = 0xC, bytes_per_packet = 0, frames_per_packet = 1,
        bytes_per_frame = 0, channels = 0, bits = 0;
};
Asbd game_client(double rate, u32 channels) {
    // MyGetOpenALAudioData: lpcm, signed integer | packed (native endian), 16 bits
    Asbd a;
    a.rate = rate;
    a.channels = channels;
    a.bits = 16;
    a.bytes_per_frame = a.bytes_per_packet = 2 * channels;
    return a;
}

// One open WAV in the guest and the calls on it.
struct Ext {
    Env &e;
    u32 file = 0;
    u32 asbd = 0, size = 0, frames64 = 0, list = 0, io = 0;

    explicit Ext(Env &env) : e(env) {
        asbd = e.alloc(40);
        size = e.alloc(4);
        frames64 = e.alloc(8);
        list = e.alloc(4 + 12);
        io = e.alloc(4);
    }
    u32 open(const char *name, const std::vector<u8> &bytes) {
        write_file(test_dir() / "Documents" / name, bytes);
        const u32 out = e.alloc(4);
        const u32 status = e.call(mt_hle_ExtAudioFileOpenURL,
                                  {e.home_url((std::string("Documents/") + name).c_str()), out});
        file = e.get32(out);
        return status;
    }
    Asbd get_format(u32 id) {
        e.put32(size, 40);
        REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty, {file, id, size, asbd}) == 0);
        CHECK(e.get32(size) == 40);
        Asbd a;
        std::memcpy(&a.rate, e.cpu.mem + asbd, 8);
        a.format = e.get32(asbd + 8);
        a.flags = e.get32(asbd + 12);
        a.bytes_per_packet = e.get32(asbd + 16);
        a.frames_per_packet = e.get32(asbd + 20);
        a.bytes_per_frame = e.get32(asbd + 24);
        a.channels = e.get32(asbd + 28);
        a.bits = e.get32(asbd + 32);
        return a;
    }
    std::uint64_t length() {
        e.put32(size, 8);
        REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty, {file, k_frm, size, frames64}) == 0);
        CHECK(e.get32(size) == 8);
        std::uint64_t n = 0;
        std::memcpy(&n, e.cpu.mem + frames64, 8);
        return n;
    }
    u32 set_client(const Asbd &a) {
        std::memcpy(e.cpu.mem + asbd, &a.rate, 8);
        const u32 words[8] = {a.format,
                              a.flags,
                              a.bytes_per_packet,
                              a.frames_per_packet,
                              a.bytes_per_frame,
                              a.channels,
                              a.bits,
                              0};
        std::memcpy(e.cpu.mem + asbd + 8, words, sizeof words);
        return e.call(mt_hle_ExtAudioFileSetProperty, {file, k_cfmt, 40, asbd});
    }
    // ExtAudioFileRead of up to `frames` frames into `data` (`capacity` bytes); returns the
    // status, and *got the frames read (*ioNumberFrames), *bytes the mDataByteSize after.
    u32 read(u32 frames, u32 data, u32 capacity, u32 *got, u32 *bytes, u32 buffers = 1) {
        e.put32(list, buffers);
        e.put32(list + 4, 1);
        e.put32(list + 8, capacity);
        e.put32(list + 12, data);
        e.put32(io, frames);
        const u32 status = e.call(mt_hle_ExtAudioFileRead, {file, io, list});
        *got = e.get32(io);
        *bytes = e.get32(list + 8);
        return status;
    }
    u32 dispose() { return e.call(mt_hle_ExtAudioFileDispose, {file}); }
};

// The 16-bit samples at guest `addr`, `count` of them.
std::vector<i16> s16_at(const Env &e, u32 addr, u32 count) {
    std::vector<i16> v(count);
    std::memcpy(v.data(), e.cpu.mem + addr, 2u * count);
    return v;
}
std::vector<i16> expected_s16(const Wav &w, u32 first, u32 frames) {
    std::vector<i16> v;
    for (u32 i = first; i < first + frames; i++) {
        for (u32 ch = 0; ch < w.channels; ch++) {
            v.push_back(sample_s16(i, ch, w.bits));
        }
    }
    return v;
}

} // namespace

TEST_CASE("audio: ExtAudioFile decodes mono and stereo WAVs at 8, 22.05 and 44.1 kHz") {
    Env e;
    for (const u32 rate : {8000u, 22050u, 44100u}) {
        for (const u32 channels : {1u, 2u}) {
            CAPTURE(rate);
            CAPTURE(channels);
            Wav w;
            w.rate = rate;
            w.channels = channels;
            w.frames = rate / 10 + channels; // 0.1 s and a bit
            Ext x(e);
            REQUIRE(x.open("pcm.wav", make_wav(w)) == 0);
            // MyGetOpenALAudioData: the file format, then the client format from it
            const Asbd f = x.get_format(k_ffmt);
            CHECK(f.rate == static_cast<double>(rate));
            CHECK(f.format == k_lpcm);
            CHECK(f.flags == 0xCu);
            CHECK(f.channels == channels);
            CHECK(f.bits == 16);
            CHECK(f.bytes_per_frame == 2 * channels);
            CHECK(f.bytes_per_packet == 2 * channels);
            CHECK(f.frames_per_packet == 1);
            REQUIRE(x.set_client(game_client(f.rate, f.channels)) == 0);
            const Asbd c = x.get_format(k_cfmt);
            CHECK(c.bits == 16);
            CHECK(c.channels == channels);
            const std::uint64_t n = x.length();
            CHECK(n == w.frames);
            // one read of the whole file into frames * mBytesPerFrame bytes
            const u32 bytes = static_cast<u32>(n) * c.bytes_per_frame;
            const u32 pcm = e.alloc(bytes + 4);
            std::memset(e.cpu.mem + pcm, 0x5A, bytes + 4);
            u32 got = 0, size = 0;
            REQUIRE(x.read(static_cast<u32>(n), pcm, bytes, &got, &size) == 0);
            CHECK(got == w.frames);
            CHECK(size == bytes);
            CHECK(s16_at(e, pcm, w.frames * channels) == expected_s16(w, 0, w.frames));
            CHECK(e.cpu.mem[pcm + bytes] == 0x5A); // nothing past the buffer
            // the end: no frames, mDataByteSize 0, no error
            REQUIRE(x.read(100, pcm, bytes, &got, &size) == 0);
            CHECK(got == 0);
            CHECK(size == 0);
            CHECK(x.dispose() == 0);
        }
    }
}

TEST_CASE("audio: ExtAudioFile skips the chunks around data and keeps their padding") {
    Env e;
    Wav w;
    w.rate = 8000;
    w.frames = 77;
    w.fmt_extra = 4; // a WAVEFORMATEX: cbSize 2 and two bytes more
    w.before = {{"fact", {77, 0, 0, 0}}, {"odd ", {'a', 'b', 'c'}}};
    // as in the shipped files: LIST/INFO, DISP, an ID3 chunk, an XMP _PMX, odd sizes padded
    w.after = {
        {"LIST", {'I', 'N', 'F', 'O', 'I', 'N', 'A', 'M', 5, 0, 0, 0, 'D', 'r', 'o', 'p', 0}},
        {"DISP", {1, 0, 0, 0, 'x'}},
        {"id3 ", std::vector<u8>(31, 0x49)},
        {"_PMX", std::vector<u8>(9, '<')}};
    Ext x(e);
    REQUIRE(x.open("chunks.wav", make_wav(w)) == 0);
    const Asbd f = x.get_format(k_ffmt);
    REQUIRE(x.set_client(game_client(f.rate, f.channels)) == 0);
    CHECK(x.length() == 77);
    const u32 pcm = e.alloc(1000);
    u32 got = 0, size = 0;
    REQUIRE(x.read(1000, pcm, 1000, &got, &size) == 0);
    CHECK(got == 77);
    CHECK(size == 154);
    CHECK(s16_at(e, pcm, 77) == expected_s16(w, 0, 77));
    // a second data chunk later in the file is not the sound
    Wav two = w;
    two.after = {{"data", std::vector<u8>(64, 0x7F)}};
    REQUIRE(x.open("two-data.wav", make_wav(two)) == 0);
    CHECK(x.length() == 77);
    REQUIRE(x.read(1000, pcm, 1000, &got, &size) == 0);
    CHECK(got == 77);
    CHECK(s16_at(e, pcm, 77) == expected_s16(w, 0, 77));
}

TEST_CASE("audio: ExtAudioFile reads a truncated data chunk up to its last whole frame") {
    Env e;
    Wav w;
    w.channels = 2;
    w.frames = 50;
    std::vector<u8> bytes = make_wav(w);
    bytes.resize(bytes.size() - 61); // 15 frames and one byte of the 16th from the end
    Ext x(e);
    REQUIRE(x.open("truncated.wav", bytes) == 0);
    REQUIRE(x.set_client(game_client(22050, 2)) == 0);
    CHECK(x.length() == 34); // (200 - 61) / 4
    const u32 pcm = e.alloc(400);
    u32 got = 0, size = 0;
    REQUIRE(x.read(50, pcm, 400, &got, &size) == 0);
    CHECK(got == 34);
    CHECK(size == 136);
    CHECK(s16_at(e, pcm, 68) == expected_s16(w, 0, 34));
}

TEST_CASE("audio: ExtAudioFile reads in pieces, bounded by ioNumberFrames and mDataByteSize") {
    Env e;
    Wav w;
    w.rate = 44100;
    w.channels = 2;
    w.frames = 1000;
    Ext x(e);
    REQUIRE(x.open("pieces.wav", make_wav(w)) == 0);
    REQUIRE(x.set_client(game_client(44100, 2)) == 0);
    const u32 pcm = e.alloc(4000 + 64);
    u32 got = 0, size = 0, at = 0;
    // ioNumberFrames 0: nothing read, no error
    REQUIRE(x.read(0, pcm, 4000, &got, &size) == 0);
    CHECK(got == 0);
    CHECK(size == 0);
    // pieces of 333 frames: 333, 333, 333, then the last frame, then the end
    for (const u32 want : {333u, 333u, 333u, 1u, 0u}) {
        CAPTURE(at);
        REQUIRE(x.read(333, pcm + 4 * at, 4000 - 4 * at + 64, &got, &size) == 0);
        CHECK(got == want);
        CHECK(size == 4 * want);
        at += got;
    }
    CHECK(at == 1000);
    CHECK(s16_at(e, pcm, 2000) == expected_s16(w, 0, 1000));
    // a fresh open, a buffer smaller than ioNumberFrames asks: as many whole frames as fit
    REQUIRE(x.open("pieces.wav", make_wav(w)) == 0);
    REQUIRE(x.set_client(game_client(44100, 2)) == 0);
    std::memset(e.cpu.mem + pcm, 0, 4000);
    REQUIRE(x.read(1000, pcm, 41, &got, &size) == 0); // 10 frames and 1 byte
    CHECK(got == 10);
    CHECK(size == 40);
    CHECK(s16_at(e, pcm, 20) == expected_s16(w, 0, 10));
    CHECK(e.cpu.mem[pcm + 40] == 0);
    REQUIRE(x.read(5, pcm, 4000, &got, &size) == 0); // goes on at frame 10
    CHECK(got == 5);
    CHECK(s16_at(e, pcm, 10) == expected_s16(w, 10, 5));
}

TEST_CASE("audio: ExtAudioFile converts 8-bit and 24-bit WAVs to the 16-bit client format") {
    Env e;
    for (const u32 bits : {8u, 24u, 32u}) {
        CAPTURE(bits);
        Wav w;
        w.bits = bits;
        w.channels = 2;
        w.frames = 64;
        Ext x(e);
        REQUIRE(x.open("depth.wav", make_wav(w)) == 0);
        const Asbd f = x.get_format(k_ffmt);
        CHECK(f.bits == bits);
        CHECK(f.flags == (bits == 8 ? 0x8u : 0xCu)); // 8-bit WAV is unsigned
        // without a client format, the file's own bytes
        const u32 pcm = e.alloc(64 * 2 * 4);
        u32 got = 0, size = 0;
        REQUIRE(x.read(8, pcm, 64 * 2 * 4, &got, &size) == 0);
        CHECK(got == 8);
        CHECK(size == 8 * f.bytes_per_frame);
        const std::vector<u8> raw = data_bytes(w);
        CHECK(std::memcmp(e.cpu.mem + pcm, raw.data(), size) == 0);
        // then the game's client format: the rest as 16-bit
        REQUIRE(x.set_client(game_client(f.rate, 2)) == 0);
        REQUIRE(x.read(100, pcm, 64 * 2 * 4, &got, &size) == 0);
        CHECK(got == 56);
        CHECK(size == 56 * 4);
        CHECK(s16_at(e, pcm, 112) == expected_s16(w, 8, 56));
    }
    // the edges of the 8-bit conversion: 0 -> -32768, 128 -> 0, 255 -> 32512
    CHECK(sample_s16(0, 0, 8) ==
          static_cast<i16>((static_cast<int>(sample_value(0, 0, 8)) - 128) * 256));
    Wav w;
    w.bits = 8;
    w.frames = 3;
    std::vector<u8> bytes = make_wav(w);
    const std::size_t data = bytes.size() - 4; // 3 samples + the pad byte
    bytes[data] = 0;
    bytes[data + 1] = 128;
    bytes[data + 2] = 255;
    Ext x(e);
    REQUIRE(x.open("edges.wav", bytes) == 0);
    REQUIRE(x.set_client(game_client(22050, 1)) == 0);
    const u32 pcm = e.alloc(8);
    u32 got = 0, size = 0;
    REQUIRE(x.read(3, pcm, 8, &got, &size) == 0);
    CHECK(s16_at(e, pcm, 3) == std::vector<i16>{-32768, 0, 32512});
}

TEST_CASE("audio: ExtAudioFile refuses the client formats it doesn't convert to") {
    Env e;
    Wav w;
    w.rate = 22050;
    w.channels = 1;
    w.frames = 10;
    Ext x(e);
    REQUIRE(x.open("mono.wav", make_wav(w)) == 0);
    const Asbd good = game_client(22050, 1);
    REQUIRE(x.set_client(good) == 0);
    const auto refused = [&](Asbd a, u32 status) {
        CHECK(x.set_client(a) == status);
        const Asbd kept = x.get_format(k_cfmt); // the client format is unchanged
        CHECK(kept.rate == 22050.0);
        CHECK(kept.channels == 1);
        CHECK(kept.bits == 16);
    };
    refused(game_client(44100, 1), k_fmt_unsupported); // a rate conversion
    refused(game_client(11025, 1), k_fmt_unsupported);
    refused(game_client(22050, 2), k_fmt_unsupported); // mono to stereo
    Asbd a = good;
    a.flags = 0x9; // float
    a.bits = 32;
    a.bytes_per_frame = a.bytes_per_packet = 4;
    refused(a, k_fmt_unsupported);
    a = good;
    a.flags = 0xE; // big-endian
    refused(a, k_fmt_unsupported);
    a = good;
    a.flags = 0x8; // unsigned 16-bit
    refused(a, k_fmt_unsupported);
    a = good;
    a.bits = 32; // another depth
    a.bytes_per_frame = a.bytes_per_packet = 4;
    refused(a, k_fmt_unsupported);
    a = good;
    a.bits = 8;
    a.flags = 0x8;
    a.bytes_per_frame = a.bytes_per_packet = 1;
    refused(a, k_fmt_unsupported);
    a = good;
    a.frames_per_packet = 2;
    refused(a, k_fmt_unsupported);
    a = good;
    a.format = 0x696D6134; // 'ima4'
    refused(a, k_non_pcm);
    a = good;
    a.channels = 0;
    refused(a, k_param);
    a = good;
    a.rate = 0;
    refused(a, k_param);
    // mono non-interleaved is the same layout: accepted
    a = good;
    a.flags = 0xC | 0x20;
    CHECK(x.set_client(a) == 0);
    // non-interleaved stereo needs two buffers: refused
    Wav sw = w;
    sw.channels = 2;
    REQUIRE(x.open("stereo.wav", make_wav(sw)) == 0);
    a = game_client(22050, 2);
    a.flags = 0xC | 0x20;
    a.bytes_per_frame = a.bytes_per_packet = 2;
    CHECK(x.set_client(a) == k_fmt_unsupported);
    // other properties and sizes
    CHECK(e.call(mt_hle_ExtAudioFileSetProperty, {x.file, k_frm, 8, x.frames64}) == k_bad_property);
    CHECK(e.call(mt_hle_ExtAudioFileSetProperty, {x.file, k_cfmt, 39, x.asbd}) ==
          static_cast<u32>(-66562));
}

TEST_CASE("audio: ExtAudioFileRead refuses bad buffer lists and handles") {
    Env e;
    Wav w;
    w.frames = 10;
    Ext x(e);
    REQUIRE(x.open("list.wav", make_wav(w)) == 0);
    REQUIRE(x.set_client(game_client(22050, 1)) == 0);
    const u32 pcm = e.alloc(64);
    u32 got = 0, size = 0;
    CHECK(x.read(10, pcm, 64, &got, &size, 0) == k_param); // no buffer
    CHECK(x.read(10, pcm, 64, &got, &size, 2) == k_param); // two: the client is interleaved
    CHECK(x.read(10, 0, 64, &got, &size) == k_param);      // no mData
    REQUIRE(x.read(4, pcm, 64, &got, &size) == 0);         // the failures read nothing
    CHECK(got == 4);
    CHECK(s16_at(e, pcm, 4) == expected_s16(w, 0, 4));
    CHECK(e.call(mt_hle_ExtAudioFileRead, {x.file, 0, x.list}) == k_param);
    CHECK(e.call(mt_hle_ExtAudioFileRead, {x.file, x.io, 0}) == k_param);
    CHECK(x.dispose() == 0);
    CHECK(e.call(mt_hle_ExtAudioFileRead, {x.file, x.io, x.list}) == k_param); // disposed
}

TEST_CASE("audio: decoded samples go through alBufferData into an effect voice") {
    Env e;
    Wav w;
    w.rate = 44100; // the mixer's rate: the voice plays the samples as they are
    w.channels = 2;
    w.frames = 256;
    Ext x(e);
    REQUIRE(x.open("voice.wav", make_wav(w)) == 0);
    const Asbd f = x.get_format(k_ffmt);
    REQUIRE(x.set_client(game_client(f.rate, f.channels)) == 0);
    const u32 n = static_cast<u32>(x.length());
    const u32 pcm = e.alloc(n * 4);
    u32 got = 0, size = 0;
    REQUIRE(x.read(n, pcm, n * 4, &got, &size) == 0);
    REQUIRE(x.dispose() == 0);
    // CMSoundPlayer: a device, a context, a buffer and a source; AL_FORMAT_STEREO16
    const u32 device = e.call(mt_hle_alcOpenDevice, {0});
    const u32 context = e.call(mt_hle_alcCreateContext, {device, 0});
    REQUIRE(e.call(mt_hle_alcMakeContextCurrent, {context}) == 1);
    const u32 names = e.alloc(8);
    e.call(mt_hle_alGenBuffers, {1, names});
    e.call(mt_hle_alGenSources, {1, names + 4});
    e.call(mt_hle_alBufferData, {e.get32(names), 0x1103u, pcm, size, 44100});
    e.call(mt_hle_alSourcei, {e.get32(names + 4), 0x1009u /* AL_BUFFER */, e.get32(names)});
    CHECK(e.call(mt_hle_alGetError, {}) == 0);
    CHECK(mt_audio_get_stats().buffers_filled == 1);
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 1.0f);
    mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, 1.0f);
    mt_audio_set_muted(false);
    e.call(mt_hle_alSourcePlay, {e.get32(names + 4)});
    std::vector<float> out(2 * 256);
    mt_audio_render(out.data(), 256);
    const std::vector<i16> want = expected_s16(w, 0, 256);
    u32 same = 0;
    for (u32 k = 0; k < 2 * 256; k++) {
        same += out[k] == static_cast<float>(want[k]) / 32768.0f ? 1u : 0u;
    }
    CHECK(same == 2 * 256);
    CHECK(!logged("unimplemented"));
}
