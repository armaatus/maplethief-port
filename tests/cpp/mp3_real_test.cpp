// AVAudioPlayer's MP3 music on the real bundle (#63, ADR 0046), local only: skipped (exit 77)
// without work/ipa/ (scripts/bootstrap).
//
//   1. All 18 MP3s of sound/ open as an Mp3Stream: stereo, 44,100 Hz (0-6, 30, 31, 34, 42) or
//      22,050 Hz (35-41), and a pass of exactly the samples of their MPEG frames, counted by a
//      frame-header walk like mtport.audio's (ID3v2 skipped, ID3v1 dropped, every header
//      walked).
//   2. Each one streams to its end in blocks of kMusicBlock frames and ends (numberOfLoops 0),
//      and loops past its end (numberOfLoops -1): the frames after the end are the first ones
//      again, as decoded from the start.
//   3. The menu track (31) through the mixer, sample for sample across its loop point.
//   4. macOS only: every file and both synthetic fixtures against Apple's own decoder
//      (ExtAudioFile, AudioToolbox, the decoder AVAudioPlayer played through): the same number
//      of frames, aligned at frame 0, within a few LSB.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "audio/mp3_stream.h"
#include "maplethief/audio.h"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;
namespace fs = std::filesystem;
namespace mix = mt_audio::mix;
using mt_audio::Mp3Stream;

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

std::vector<u8> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

// --- the frame walk (as mtport.audio's read_mp3) ------------------------------------------------

struct Walk {
    u32 rate = 0, frames = 0;
    u64 samples = 0;
    bool ok = false;
};

Walk walk(const std::vector<u8> &d) {
    static const u32 k_v1[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                 112, 128, 160, 192, 224, 256, 320, 0};
    static const u32 k_v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const u32 k_rates[4][3] = {
        {11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    Walk w;
    std::size_t pos = 0;
    while (pos + 10 <= d.size() && d[pos] == 'I' && d[pos + 1] == 'D' && d[pos + 2] == '3') {
        const std::size_t size = (std::size_t{d[pos + 6]} << 21) | (std::size_t{d[pos + 7]} << 14) |
                                 (std::size_t{d[pos + 8]} << 7) | d[pos + 9];
        pos += 10 + size + ((d[pos + 5] & 0x10) != 0 ? 10 : 0);
    }
    std::size_t end = d.size();
    if (end >= pos + 128 && d[end - 128] == 'T' && d[end - 127] == 'A' && d[end - 126] == 'G') {
        end -= 128;
    }
    while (pos < end && d[pos] == 0) {
        pos++;
    }
    while (end - pos >= 4) {
        const u32 h =
            (u32{d[pos]} << 24) | (u32{d[pos + 1]} << 16) | (u32{d[pos + 2]} << 8) | d[pos + 3];
        const u32 version = (h >> 19) & 3, layer = (h >> 17) & 3, bi = (h >> 12) & 15,
                  ri = (h >> 10) & 3, pad = (h >> 9) & 1;
        if ((h >> 21) != 0x7FF || version == 1 || layer != 1 || bi == 0 || bi == 15 || ri == 3) {
            return w; // lost sync, or not Layer III
        }
        const bool v1 = version == 3;
        const u32 rate = k_rates[version][ri];
        const u32 kbps = v1 ? k_v1[bi] : k_v2[bi];
        const std::size_t size = (v1 ? 144000u : 72000u) * kbps / rate + pad;
        if (w.rate != 0 && rate != w.rate) {
            return w;
        }
        if (pos + size > end) {
            break; // a cut-off last frame
        }
        w.rate = rate;
        w.frames++;
        w.samples += v1 ? 1152 : 576;
        pos += size;
    }
    w.ok = w.frames > 0;
    return w;
}

// --- streams ------------------------------------------------------------------------------------

std::vector<i16> read_all(Mp3Stream &s, u64 frames) {
    std::vector<i16> out;
    out.reserve(static_cast<std::size_t>(frames) * s.channels());
    std::vector<i16> buf(std::size_t{mix::kMusicBlock} * s.channels());
    u64 left = frames;
    while (left > 0) {
        const u32 want = static_cast<u32>(std::min<u64>(left, mix::kMusicBlock));
        const u32 got = s.read(buf.data(), want);
        out.insert(out.end(), buf.begin(), buf.begin() + std::size_t{got} * s.channels());
        left -= got;
        if (got < want) {
            break;
        }
    }
    return out;
}

#if defined(__APPLE__)
// Apple's decode of `path` as 16-bit interleaved PCM at the file's rate and channel count.
bool apple_decode(const fs::path &path, std::vector<i16> *out, u32 *channels) {
    const std::string p = path.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8 *>(p.c_str()), static_cast<CFIndex>(p.size()), false);
    ExtAudioFileRef f = nullptr;
    const OSStatus st = ExtAudioFileOpenURL(url, &f);
    CFRelease(url);
    if (st != noErr) {
        return false;
    }
    AudioStreamBasicDescription file{};
    UInt32 size = sizeof file;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &size, &file);
    AudioStreamBasicDescription client{};
    client.mSampleRate = file.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = file.mChannelsPerFrame;
    client.mBitsPerChannel = 16;
    client.mBytesPerFrame = 2 * file.mChannelsPerFrame;
    client.mFramesPerPacket = 1;
    client.mBytesPerPacket = client.mBytesPerFrame;
    ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client);
    *channels = file.mChannelsPerFrame;
    std::vector<i16> buf(std::size_t{8192} * *channels);
    out->clear();
    for (;;) {
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = *channels;
        list.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size() * sizeof(i16));
        list.mBuffers[0].mData = buf.data();
        UInt32 frames = 8192;
        if (ExtAudioFileRead(f, &frames, &list) != noErr) {
            ExtAudioFileDispose(f);
            return false;
        }
        if (frames == 0) {
            break;
        }
        out->insert(out->end(), buf.begin(), buf.begin() + std::size_t{frames} * *channels);
    }
    ExtAudioFileDispose(f);
    return true;
}

// Our pass of `path` against Apple's decode: the same frames, at most `tolerance` apart.
void compare_with_apple(const fs::path &path, const std::vector<i16> &ours, u32 channels,
                        int tolerance) {
    std::vector<i16> apple;
    u32 apple_channels = 0;
    const std::string name = path.filename().string();
    if (!apple_decode(path, &apple, &apple_channels)) {
        check(false, name + ": ExtAudioFile decodes it");
        return;
    }
    int worst = 0;
    const std::size_t n = std::min(apple.size(), ours.size());
    for (std::size_t i = 0; i < n; i++) {
        worst = std::max(worst, std::abs(int{apple[i]} - int{ours[i]}));
    }
    char line[200];
    std::snprintf(line, sizeof line,
                  "%s: as Apple's decoder: %zu frames (ours %zu), largest difference %d LSB",
                  name.c_str(), apple.size() / std::max(apple_channels, 1u), ours.size() / channels,
                  worst);
    check(apple_channels == channels && apple.size() == ours.size() && worst <= tolerance, line);
}
#endif

} // namespace

int main() {
    // The synthetic fixtures against Apple's decoder first (no game files needed).
#if defined(__APPLE__)
    for (const char *name : {"tone44.mp3", "tone22.mp3"}) {
        const fs::path p = fs::path(MT_AUDIO_DATA_DIR) / name;
        auto s = Mp3Stream::open(read_file(p));
        check(s != nullptr, std::string(name) + ": opens");
        if (s != nullptr) {
            compare_with_apple(p, read_all(*s, s->length()), s->channels(), 4);
        }
    }
#endif
    const fs::path sound = fs::path(MT_BUNDLE_DIR) / "sound";
    if (!fs::is_directory(sound)) {
        std::printf("skipped: %s is missing (run scripts/bootstrap)\n", sound.string().c_str());
        return g_failures == 0 ? 77 : 1;
    }
    const u32 ids[] = {0, 1, 2, 3, 4, 5, 6, 30, 31, 34, 35, 36, 37, 38, 39, 40, 41, 42};
    u32 files = 0;
    double seconds = 0, decode_ms = 0;
    for (const fs::directory_entry &e : fs::directory_iterator(sound)) {
        files += e.path().extension() == ".mp3" ? 1u : 0u;
    }
    check(files == 18, "sound/ holds 18 MP3s (" + std::to_string(files) + ")");
    std::printf("  id   rate  ch      frames  seconds  passes  decode ms\n");
    for (const u32 id : ids) {
        const fs::path p = sound / (std::to_string(id) + ".mp3");
        const std::string name = p.filename().string();
        const std::vector<u8> bytes = read_file(p);
        const Walk w = walk(bytes);
        auto s = Mp3Stream::open(bytes);
        if (s == nullptr || !w.ok) {
            check(false, name + ": opens and has a frame walk");
            continue;
        }
        const u32 rate = id >= 35 && id <= 41 ? 22050 : 44100;
        check(s->rate() == rate && w.rate == rate && s->channels() == 2,
              name + ": stereo " + std::to_string(s->rate()) + " Hz");
        check(s->length() == w.samples, name + ": " + std::to_string(s->length()) +
                                            " frames = " + std::to_string(w.frames) +
                                            " MPEG frames x " + (rate == 44100 ? "1152" : "576"));
        // to the end, once
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<i16> pass = read_all(*s, s->length() + 10000);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        decode_ms += ms;
        seconds += static_cast<double>(s->length()) / rate;
        check(pass.size() == s->length() * 2 && s->ended(), name + ": streams to its end and ends");
        bool tail_silent = true;
        for (std::size_t i = pass.size() - 2 * Mp3Stream::kPriming; i < pass.size(); i++) {
            tail_silent = tail_silent && pass[i] == 0;
        }
        check(tail_silent, name + ": the pass ends with 529 frames of silence");
        // past its end, for ever
        auto l = Mp3Stream::open(bytes);
        l->set_loops(-1);
        l->seek(l->length() - 2000);
        const std::vector<i16> across = read_all(*l, 2000 + 3 * u64{mix::kMusicBlock});
        bool same = across.size() == 2 * (2000 + 3 * std::size_t{mix::kMusicBlock});
        for (std::size_t i = 0; same && i < across.size(); i++) {
            const std::size_t at = (pass.size() - 4000 + i) % pass.size();
            same = across[i] == pass[at];
        }
        check(same && !l->ended() && l->passes_repeated() == 1,
              name + ": loops past its end to its first frames");
        std::printf("  %2u  %5u  %2u  %10llu  %7.3f  %6u  %9.1f\n", id, s->rate(), s->channels(),
                    static_cast<unsigned long long>(s->length()),
                    static_cast<double>(s->length()) / rate, l->passes_repeated(), ms);
#if defined(__APPLE__)
        compare_with_apple(p, pass, 2, 4);
#endif
        // the menu track through the mixer, across its loop point, sample for sample
        if (id == 31) {
            mix::reset();
            auto m = Mp3Stream::open(bytes);
            m->set_loops(-1);
            m->seek(m->length() - 30000);
            mix::music_play(31, m, 1.0f);
            std::vector<float> out(2 * 60000);
            mt_audio_render(out.data(), 60000);
            bool exact = true;
            for (std::size_t k = 0; exact && k < 60000; k++) {
                for (u32 ch = 0; ch < 2; ch++) {
                    const i16 want = pass[(pass.size() - 60000 + 2 * k + ch) % pass.size()];
                    exact = exact && out[2 * k + ch] == static_cast<float>(want) / 32768.0f;
                }
            }
            check(exact && mix::music_playing(31),
                  "31.mp3 through the mixer: 60,000 frames across the loop point, exact");
            mix::music_remove(31);
        }
    }
    std::printf("18 tracks, %.1f s of music, decoded in %.0f ms (%.0f x real time)\n", seconds,
                decode_ms, seconds * 1000 / std::max(decode_ms, 1.0));
    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
