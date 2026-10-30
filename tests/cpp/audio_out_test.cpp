// The audio output (#61, ADR 0045, platform/src/audio_out.h): the frames a span of time covers
// (the remainder carried), the late-callback rule, the WAV capture of a synthetic mix (the same
// bytes every time, a 16-bit stereo 44.1 kHz header, each sample the mix rounded), and the
// device stream on SDL's dummy driver (it pulls the mixer) or on no driver at all (silent, no
// error); settings.ini's volumes on the buses, checked by rendering constant sources, and
// mute-on-unfocus from SDL's focus events (#64).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

// --- the settings' buses and mute-on-unfocus (#64) -------------------------------------------

namespace {

// A constant mono 16-bit source at 44,100 Hz (step 1: every output frame is the sample).
mix::PcmPtr constant(std::int16_t value, std::uint32_t frames) {
    const std::vector<std::int16_t> s(frames, value);
    return mix::make_pcm(reinterpret_cast<const std::uint8_t *>(s.data()), frames * 2, 1, 16,
                         MT_AUDIO_RATE);
}

// A music stream of one constant mono sample at 44,100 Hz, for ever.
class ConstantMusic : public mix::MusicStream {
  public:
    explicit ConstantMusic(std::int16_t v) : v_(v) {}
    std::uint32_t rate() const override { return MT_AUDIO_RATE; }
    std::uint32_t channels() const override { return 1; }
    std::uint32_t read(std::int16_t *out, std::uint32_t frames) override {
        std::fill(out, out + frames, v_);
        return frames;
    }

  private:
    std::int16_t v_;
};

// The first left sample of a 256-frame render.
float level() {
    std::vector<float> out(2 * 256);
    mt_audio_render(out.data(), 256);
    CHECK(out[0] == out[1]); // mono to both channels
    CHECK(out[0] == out[2 * 255]);
    return out[0];
}

} // namespace

TEST_CASE("audio_out: settings.ini's volumes set the buses, and a tone plays at that level") {
    CHECK(fe::volume_gain(0) == 0.0f);
    CHECK(fe::volume_gain(50) == 0.5f);
    CHECK(fe::volume_gain(100) == 1.0f);
    CHECK(fe::volume_gain(250) == 1.0f); // clamped
    CHECK(fe::volume_gain(25) == 0.25f);

    fe::Settings s;
    s.master_volume = 50;
    s.music_volume = 25;
    s.effects_volume = 80;
    fe::apply_audio_settings(s);
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MASTER) == 0.5f);
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MUSIC) == 0.25f);
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_EFFECTS) == fe::volume_gain(80));

    // A half-scale effect (16,384 / 32,768) at effects 80 % and master 50 %: 0.5 x 0.8 x 0.5.
    mix::reset();
    mix::effect_play(1, constant(16384, 4096));
    CHECK(level() == doctest::Approx(0.2).epsilon(1e-6));
    // The game's own listener gain still multiplies (alListenerf), separately from the bus.
    mix::set_listener_gain(0.5f);
    CHECK(level() == doctest::Approx(0.1).epsilon(1e-6));
    mix::set_listener_gain(1.0f);
    mix::effect_stop_all();
    // Music at music 25 % and master 50 %, at the player's volume 0.7 (the game's).
    mix::music_play(7, std::make_shared<ConstantMusic>(16384), 0.7f);
    CHECK(level() == doctest::Approx(0.5 * 0.7 * 0.25 * 0.5).epsilon(1e-6));
    // Both together: the sum.
    mix::effect_play(1, constant(16384, 4096));
    CHECK(level() == doctest::Approx(0.2 + 0.5 * 0.7 * 0.25 * 0.5).epsilon(1e-6));
    // Volume 0 on a bus silences only that bus; master 0 silences everything.
    s.effects_volume = 0;
    fe::apply_audio_settings(s);
    mix::effect_play(1, constant(16384, 4096));
    CHECK(level() == doctest::Approx(0.5 * 0.7 * 0.25 * 0.5).epsilon(1e-6));
    s.master_volume = 0;
    fe::apply_audio_settings(s);
    CHECK(level() == 0.0f);
    // The defaults: every bus at 1, the music at its own volume only.
    fe::apply_audio_settings(fe::Settings{});
    mix::effect_stop_all();
    CHECK(level() == doctest::Approx(0.5 * 0.7).epsilon(1e-6));
    for (std::uint32_t b = 0; b < MT_AUDIO_BUS_COUNT; b++) {
        CHECK(mt_audio_get_gain(static_cast<mt_audio_bus>(b)) == 1.0f);
    }
    mix::reset();
    // gapless_music (#125) sets the runtime's switch; the defaults turn it off again.
    CHECK_FALSE(mt_audio_get_gapless_music());
    s.gapless_music = true;
    fe::apply_audio_settings(s);
    CHECK(mt_audio_get_gapless_music());
    fe::apply_audio_settings(fe::Settings{});
    CHECK_FALSE(mt_audio_get_gapless_music());
}

TEST_CASE("audio_out: focus loss mutes and focus gain unmutes, only with mute_unfocused") {
    mix::reset();
    mt_audio_set_muted(false);
    mix::music_play(1, std::make_shared<ConstantMusic>(16384), 1.0f);
    CHECK(level() == 0.5f);

    fe::FocusMute on(true);
    CHECK_FALSE(on.muted());
    CHECK_FALSE(on.on_event(SDL_EVENT_WINDOW_FOCUS_GAINED)); // already unmuted
    CHECK(on.on_event(SDL_EVENT_WINDOW_FOCUS_LOST));
    CHECK(on.muted());
    CHECK(mt_audio_get_muted());
    CHECK(level() == 0.0f);
    CHECK_FALSE(on.on_event(SDL_EVENT_WINDOW_FOCUS_LOST)); // twice: no change
    // Other events change nothing (a mouse move, a resize, a key, the window hidden).
    for (std::uint32_t type : {static_cast<std::uint32_t>(SDL_EVENT_MOUSE_MOTION),
                               static_cast<std::uint32_t>(SDL_EVENT_WINDOW_RESIZED),
                               static_cast<std::uint32_t>(SDL_EVENT_KEY_DOWN),
                               static_cast<std::uint32_t>(SDL_EVENT_WINDOW_HIDDEN), 0u}) {
        CHECK_FALSE(on.on_event(type));
        CHECK(mt_audio_get_muted());
    }
    CHECK(on.on_event(SDL_EVENT_WINDOW_FOCUS_GAINED));
    CHECK_FALSE(on.muted());
    CHECK_FALSE(mt_audio_get_muted());
    CHECK(level() == 0.5f);
    // Mute keeps the bus gains (they are not what mutes).
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MASTER) == 1.0f);

    // The option off: focus never mutes.
    fe::FocusMute off(false);
    CHECK_FALSE(off.on_event(SDL_EVENT_WINDOW_FOCUS_LOST));
    CHECK_FALSE(mt_audio_get_muted());
    CHECK(level() == 0.5f);
    CHECK_FALSE(off.on_event(SDL_EVENT_WINDOW_FOCUS_GAINED));
    CHECK_FALSE(fe::FocusMute().enabled()); // the default (a capture, a test): off

    // Through a real SDL_Event as the game's handler gets it.
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    CHECK(on.on_event(e.type));
    CHECK(level() == 0.0f);
    e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    CHECK(on.on_event(e.type));
    CHECK(level() == 0.5f);
    mt_audio_set_muted(false);
    mix::reset();
}
