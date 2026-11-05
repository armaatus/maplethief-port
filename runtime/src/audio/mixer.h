// The audio mixer (#61, ADR 0045): what the OpenAL layer (audio.cpp) and the AVAudioPlayer
// class (avfoundation.cpp) play, mixed into MT_AUDIO_RATE Hz stereo float for whoever pulls
// it through mt_audio_render (maplethief/audio.h): the platform's device stream, a capture, a
// test.
//
//   voices       effects: one per OpenAL source name, playing a shared, immutable Pcm from
//                frame 0 to its end (effect_play restarts it), at listener gain x effects bus.
//                music (#63): one per key (e.g. the player's guest address), pulling 16-bit
//                PCM from a MusicStream in blocks of at most kMusicBlock frames, at the
//                voice's volume x music bus. Both step through their source with a 32.32
//                fixed-point position (step = source rate / MT_AUDIO_RATE) and interpolate
//                linearly between neighbouring frames in integers, so the mix is the same
//                bytes on every host; past the last frame a source is silence.
//   the mix      effects in source-name order, then music in key order, summed in float,
//                (mono, #195: (L + R) / 2 in both channels), times the master bus (0 while
//                muted), clamped to [-1, 1].
//   the lock     one mutex guards everything here. It is taken by each function below for
//                host work only (no guest code runs under it, no guest memory is read under
//                it: Pcm is host memory), and by mt_audio_render on any thread.
//
// The bus gains, mute and mono (mt_audio_set_gain, mt_audio_set_muted, mt_audio_set_mono) are
// the host's settings (#64, #195): they act on the game's mix and survive reset(). mixer.cpp is
// compiled without floating-point contraction (maplethief_strict_fp).
#ifndef MAPLETHIEF_RUNTIME_AUDIO_MIXER_H
#define MAPLETHIEF_RUNTIME_AUDIO_MIXER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "maplethief/audio.h"

namespace mt_audio::mix {

using u32 = std::uint32_t;

// 16-bit PCM in host memory, interleaved, immutable once made (shared by an AL buffer and the
// voices that play it).
struct Pcm {
    std::vector<std::int16_t> samples; // frames * channels
    u32 channels = 1;                  // 1 or 2
    u32 rate = MT_AUDIO_RATE;          // Hz, > 0
    u32 frames = 0;
};
using PcmPtr = std::shared_ptr<const Pcm>;

// PCM from `bytes` bytes of OpenAL sample data (8-bit unsigned or 16-bit signed native-endian,
// 1 or 2 channels, interleaved). `data` may be null: `bytes` of silence. A trailing partial
// frame is dropped.
PcmPtr make_pcm(const std::uint8_t *data, u32 bytes, u32 channels, u32 bits, u32 rate);

// The 32.32 fixed-point step through a source of `rate` Hz per output frame:
// floor(rate * 2^32 / MT_AUDIO_RATE).
std::uint64_t step_for(u32 rate);

// The source sample at 32.32 position `pos` of channel `ch`, linearly interpolated between
// frames i = pos >> 32 and i + 1 (silence past the end): s0 + ((s1 - s0) * frac) >> 32, with
// an arithmetic shift. Exposed for the tests, which check the mixer against it.
std::int32_t interpolate(const std::int16_t *samples, u32 frames, u32 channels, u32 ch,
                         std::uint64_t pos);

// --- effects (OpenAL sources) ------------------------------------------------------------------

// (Re)starts the voice of source `key` at frame 0 of `pcm` (null or empty: the voice stops).
void effect_play(u32 key, PcmPtr pcm);
// Stops the voice of source `key` (if any) and forgets it.
void effect_stop(u32 key);
// Stops every effect voice.
void effect_stop_all();
// Whether the voice of source `key` is still playing (it ends after its last frame).
bool effect_playing(u32 key);
// The listener gain (alListenerf AL_GAIN), >= 0.
void set_listener_gain(float gain);
// Pauses (keeping their positions) or resumes every effect voice: the OpenAL context is
// suspended or not current.
void set_effects_paused(bool paused);

// --- music (AVAudioPlayer, #63) -------------------------------------------------------------

// Decoded music for a music voice. read() is called by the mixer on the render thread, under
// the mixer's lock: it must not block and should decode no more than asked.
class MusicStream {
  public:
    virtual ~MusicStream() = default;
    virtual u32 rate() const = 0;     // Hz, > 0; fixed for the stream's life
    virtual u32 channels() const = 0; // 1 or 2
    // Writes up to `frames` frames of interleaved 16-bit PCM to `out`; returns how many. Fewer
    // than asked means the stream has ended (a looping stream loops inside read() and never
    // ends); the voice then plays out what it has and stops.
    virtual u32 read(std::int16_t *out, u32 frames) = 0;
};
constexpr u32 kMusicBlock = 4096; // frames per MusicStream::read at most

// Starts (or keeps) the music voice `key` playing `stream` at `volume` (>= 0) from the
// stream's current position. A different stream than the voice had replaces it (and drops
// what the voice had buffered).
void music_play(u32 key, std::shared_ptr<MusicStream> stream, float volume);
// Stops the music voice `key`, keeping it, its stream and the frames it had pulled but not
// played yet (AVAudioPlayer -stop keeps the position: the next music_play of the same stream
// goes on from the exact frame where this one stopped).
void music_stop(u32 key);
// Forgets the music voice `key` and its stream (a released player).
void music_remove(u32 key);
void music_set_volume(u32 key, float volume);
bool music_playing(u32 key);
// Runs `fn` on the stream of voice `key` under the mixer's lock (e.g. to seek it for
// setCurrentTime:, while the render thread may be reading it), after dropping what the voice
// had buffered. Returns false if there is no such voice.
bool music_edit(u32 key, const std::function<void(MusicStream &)> &fn);

// --- the whole mixer ---------------------------------------------------------------------------

// A new process (a new current ObjC runtime): every voice is gone, the listener gain is 1 and
// effects are not paused. The bus gains, mute and output stats are kept.
void reset();

// Mixes the next `frames` frames into `out` (2 * frames floats, overwritten).
void render(float *out, u32 frames);

// Voices playing now (effects, music), for stats and tests.
u32 effects_playing();
u32 music_voices_playing();

// --- save states (#203, ADR 0105) --------------------------------------------------------------

// Every voice of the game's mix, as values: an effect's Pcm and 32.32 position, a music voice's
// stream, the frames it pulled and has not played yet and its position in them.
struct Voices {
    struct Effect {
        u32 key = 0;
        PcmPtr pcm;
        std::uint64_t pos = 0, step = 0;
        bool playing = false;
    };
    struct Music {
        u32 key = 0;
        std::shared_ptr<MusicStream> stream;
        float volume = 1.0f;
        bool playing = false;
        std::vector<std::int16_t> buf; // frames * channels samples
        u32 frames = 0, channels = 0;
        std::uint64_t pos = 0, step = 0;
        bool ended = false;
    };
    std::vector<Effect> effects; // in key order
    std::vector<Music> music;    // in key order
    float listener_gain = 1.0f;
    bool effects_paused = false;
};
// A copy of the voices, taken under the mixer's lock.
Voices voices_copy();
// Replaces every voice, the listener gain and the effects' pause with `v`, under the mixer's
// lock (the render thread sees the old mix or the new one, never a mixture). The host's bus
// gains, mute and mono stay.
void voices_replace(Voices v);

} // namespace mt_audio::mix

#endif // MAPLETHIEF_RUNTIME_AUDIO_MIXER_H
