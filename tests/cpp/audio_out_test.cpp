// The audio output (#61, ADR 0045, platform/src/audio_out.h): the frames a span of time covers
// (the remainder carried), the late-callback rule, the WAV capture of a synthetic mix (the same
// bytes every time, a 16-bit stereo 44.1 kHz header, each sample the mix rounded), and the
// device stream on SDL's dummy driver (it pulls the mixer) or on no driver at all (silent, no
// error).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/mixer.h"
#include "audio_out.h"
#include "maplethief/audio.h"

namespace fe = maplethief::frontend;
namespace mix = mt_audio::mix;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> read_all(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::uint32_t le32(const std::vector<std::uint8_t> &b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (std::uint32_t{b[at + 3]} << 24);
}

// A mono 16-bit tone at 22,050 Hz as a Pcm.
mix::PcmPtr tone(std::uint32_t frames, int scale) {
    std::vector<std::int16_t> s(frames);
    for (std::uint32_t i = 0; i < frames; i++) {
        s[i] = static_cast<std::int16_t>((static_cast<int>(i % 64) - 32) * scale);
    }
    return mix::make_pcm(reinterpret_cast<const std::uint8_t *>(s.data()), frames * 2, 1, 16,
                         22050);
}

// A scripted "run": 30 steps of 16.667 ms, an effect at the start and another at step 10, each
// step's audio captured after it. Returns the capture's bytes.
std::vector<std::uint8_t> scripted_capture(const fs::path &path, std::uint64_t *frames) {
    mix::reset();
    fe::AudioCapture capture;
    REQUIRE(capture.open(path.string()));
    mix::effect_play(1, tone(4000, 900));
    std::uint64_t elapsed = 0;
    for (int step = 0; step < 30; step++) {
        if (step == 10) {
            mix::effect_play(2, tone(3000, -500));
        }
        elapsed += 16667000; // ns
        REQUIRE(capture.render_to(elapsed));
    }
    *frames = capture.frames();
    REQUIRE(capture.close());
    return read_all(path);
}

fs::path temp_path(const char *name) {
    return fs::temp_directory_path() / (std::string("maplethief-audio-out-") +
                                        std::to_string(std::random_device{}()) + "-" + name);
}

} // namespace

TEST_CASE("audio_out: the frames a span covers, the remainder carried") {
    CHECK(fe::audio_frames_at(0) == 0);
    CHECK(fe::audio_frames_at(1000000000) == 44100);
    CHECK(fe::audio_frames_at(16667000) == 735);           // 735.01: the .01 is carried...
    CHECK(fe::audio_frames_at(60 * 16667000ull) == 44100); // ...44,100.88 after 60 steps
    CHECK(fe::audio_frames_at(22675) == 0);                // just under one frame (22,675.7 ns)
    CHECK(fe::audio_frames_at(22676) == 1);
    // five days: no overflow
    const std::uint64_t five_days = 5ull * 86400 * 1000000000ull;
    CHECK(fe::audio_frames_at(five_days) == 5ull * 86400 * 44100);
}

TEST_CASE("audio_out: a device callback is late after 1.5 x the audio it delivered last") {
    fe::LateDetector d;
    // 441 frames = 10 ms
    CHECK(!d.on_callback(1000000000, 441));
    CHECK(!d.on_callback(1010000000, 441));
    CHECK(!d.on_callback(1024900000, 441)); // 14.9 ms: within 15
    CHECK(d.on_callback(1040000000, 441));  // 15.1 ms: late
    CHECK(d.late() == 1);
    // two calls closer than 1 ms are one delivery of 882 frames (20 ms): 25 ms later is fine
    CHECK(!d.on_callback(1040500000, 441));
    CHECK(!d.on_callback(1065000000, 441));
    CHECK(!d.on_callback(1065000000, 0)); // nothing delivered: ignored
    CHECK(d.late() == 1);
}

TEST_CASE("audio_out: --audio-capture writes the same WAV every time, the mix rounded to 16 bits") {
    const fs::path a = temp_path("a.wav"), b = temp_path("b.wav");
    std::uint64_t frames_a = 0, frames_b = 0;
    const std::vector<std::uint8_t> wav_a = scripted_capture(a, &frames_a);
    const std::vector<std::uint8_t> wav_b = scripted_capture(b, &frames_b);
    CHECK(frames_a == fe::audio_frames_at(30 * 16667000ull));
    CHECK(frames_a == 22050); // 22,050.44
    CHECK(frames_b == frames_a);
    REQUIRE(wav_a.size() == 44 + 4 * frames_a);
    CHECK(wav_a == wav_b);
    // RIFF header: PCM, 2 channels, 44,100 Hz, 176,400 bytes/s, 4-byte frames, 16 bits
    CHECK(std::string(wav_a.begin(), wav_a.begin() + 4) == "RIFF");
    CHECK(le32(wav_a, 4) == 36 + 4 * frames_a);
    CHECK(std::string(wav_a.begin() + 8, wav_a.begin() + 16) == "WAVEfmt ");
    CHECK(le32(wav_a, 16) == 16);
    CHECK(le32(wav_a, 20) == (1u | 2u << 16));
    CHECK(le32(wav_a, 24) == 44100);
    CHECK(le32(wav_a, 28) == 176400);
    CHECK(le32(wav_a, 32) == (4u | 16u << 16));
    CHECK(std::string(wav_a.begin() + 36, wav_a.begin() + 40) == "data");
    CHECK(le32(wav_a, 40) == 4 * frames_a);
    // each sample is the mix's value x 32767, rounded: render the same script directly
    mix::reset();
    mix::effect_play(1, tone(4000, 900));
    std::vector<float> mixed(2 * 7350); // ten steps' worth (7,350.11 frames)
    mt_audio_render(mixed.data(), 7350);
    for (std::size_t i = 0; i < mixed.size(); i++) {
        const auto want = static_cast<std::int16_t>(std::lround(mixed[i] * 32767.0f));
        const auto got = static_cast<std::int16_t>(wav_a[44 + 2 * i] | (wav_a[45 + 2 * i] << 8));
        REQUIRE(got == want);
    }
    CHECK(wav_a[44 + 4 * 2] != 0); // not silence
    mix::reset();
    std::error_code ec;
    fs::remove(a, ec);
    fs::remove(b, ec);
    // a capture that cannot be written
    fe::AudioCapture bad;
    CHECK(!bad.open((temp_path("missing-dir") / "x.wav").string()));
    CHECK(!bad.render_to(1000000000));
}

TEST_CASE(
    "audio_out: the device stream pulls the mixer (SDL's dummy driver); no driver is silent") {
    mix::reset();
    mt_audio_reset_output_stats();
    {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "no-such-driver");
        fe::AudioDevice none;
        CHECK(!none.open()); // not an error: the game runs silent
        CHECK(!none.is_open());
        none.resume(); // harmless
    }
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    {
        fe::AudioDevice device;
        REQUIRE(device.open());
        CHECK(mt_audio_get_output_stats().frames == 0); // opened paused
        mt_audio_test_tone(440.0f, 200);
        device.resume();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (mt_audio_get_output_stats().frames < 8820 &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        device.close();
        const mt_audio_output_stats st = mt_audio_get_output_stats();
        CHECK(st.frames >= 8820); // the tone's 200 ms
        CHECK(st.renders >= 1);
    }
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    mix::reset();
}
