// An MP3 file as a music stream for the mixer (#63, ADR 0046): what one AVAudioPlayer plays.
//
//   decoding   dr_mp3 (ADR 0003) over the whole file's bytes, held in host memory, decoded a
//              block at a time as the mixer pulls it (never decoded whole). Its scalar path
//              (DR_MP3_NO_SIMD) compiled without contraction gives the same samples on every
//              host.
//   timeline   as Apple's decoder gives it, which AVAudioPlayer played (ADR 0046): one pass is
//              length() = MP3 frames x samples per frame (the file's duration), starting
//              kPriming frames into the decoder's output (the decoder delay is skipped) and
//              ending with kPriming frames of silence. The encoder's own delay and padding stay
//              in: the files carry no Xing/LAME info to trim them by.
//   loops      AVAudioPlayer's numberOfLoops: at the end of a pass the stream goes on with the
//              next pass, from frame 0 (the decoder starts afresh), if loops < 0 (for ever) or
//              fewer than `loops` passes have been repeated; otherwise it has ended.
//   gapless    (optional, off by default: #125, ADR 0062) a stream opened gapless decodes its
//              pass once at open and keeps only the frames from its first to its last one that
//              is not digital silence (a frame whose samples are all exactly 0): its pass is
//              those frames, so a loop goes from the last sound straight to the first. Pass
//              frame p is then Apple frame trim_start() + p; length(), position(), seek() and
//              the loops are all on that shorter timeline. Off, nothing changes: trim_start()
//              is 0 and the pass is Apple's whole one.
//   packs      (#188, ADR 0106) a stream opened by open_pack() or open_pcm() plays an audio pack's
//              file, an MP3 (dr_mp3) or 16-bit PCM decoded from a WAV, on the file's own
//              timeline: no decoder delay dropped and no silent tail (kPriming is Apple's, not
//              the file's), so its pass is exactly the decoder's frames. An optional loop
//              [start, end) of those frames: the first pass plays frames 0 to end, each repeat
//              start to end, and the gapless trim is not applied (the loop says where it goes).
//              position(), length() and seek() count from frame 0 of the file; a repeat begins
//              at position start.
//
// read() runs on the render thread under the mixer's lock. seek() and restart() must run under
// that lock too while a voice plays the stream (mix::music_edit), or while no voice has it.
// set_loops() and ended() are atomic and may be called from the guest thread at any time.
#ifndef MAPLETHIEF_RUNTIME_AUDIO_MP3_STREAM_H
#define MAPLETHIEF_RUNTIME_AUDIO_MP3_STREAM_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/mixer.h"

namespace mt_audio {

class Mp3Stream final : public mix::MusicStream {
  public:
    // Apple's MP3 decoder's delay: the frames of decoder output it drops at the start of a
    // stream (measured: CoreAudio's output is dr_mp3's from frame 529 on, for every shipped
    // MP3 and both MPEG versions).
    static constexpr std::uint32_t kPriming = 529;

    // A stream over the bytes of a whole MP3 file; null if dr_mp3 finds no audio in them, or
    // not 1 or 2 channels. `gapless`: the pass without its leading and trailing digital silence
    // (a pass that is silent throughout stays whole).
    static std::shared_ptr<Mp3Stream> open(std::vector<std::uint8_t> bytes, bool gapless = false);

    // An audio pack's loop (#188): frames [start, end) of the file, start < end.
    struct Loop {
        std::uint64_t start = 0, end = 0;
    };
    // An audio pack's MP3 on its own timeline (see packs above); null as open(), or if it has
    // longer than `max_seconds`. A loop past the file's end is clamped to it, and one that
    // is then empty is dropped.
    static std::shared_ptr<Mp3Stream> open_pack(std::vector<std::uint8_t> bytes, const Loop *loop,
                                                bool gapless, std::uint32_t max_seconds);
    // An audio pack's decoded PCM (interleaved, `channels` 1 or 2, `rate` > 0) as a stream;
    // null if empty. The loop as open_pack().
    static std::shared_ptr<Mp3Stream> open_pcm(std::vector<std::int16_t> samples,
                                               std::uint32_t rate, std::uint32_t channels,
                                               const Loop *loop, bool gapless);
    // The loop the stream plays (end 0: none).
    Loop loop() const { return Loop{loop_start_, has_loop_ ? length_ : 0}; }
    ~Mp3Stream() override;
    Mp3Stream(const Mp3Stream &) = delete;
    Mp3Stream &operator=(const Mp3Stream &) = delete;

    std::uint32_t rate() const override { return rate_; }
    std::uint32_t channels() const override { return channels_; }
    std::uint32_t read(std::int16_t *out, std::uint32_t frames) override;

    // Frames in one pass, and the position in the current pass (0 .. length()).
    std::uint64_t length() const { return length_; }
    // Apple's whole pass (MPEG frames x samples per frame), and the frame of it that is pass
    // frame 0: 0 unless the stream was opened gapless and its pass starts in silence.
    std::uint64_t full_length() const { return full_; }
    std::uint64_t trim_start() const { return start_; }
    std::uint64_t position() const { return pos_; }
    // How many times the stream has gone back to frame 0 since it was opened or restarted.
    std::uint32_t passes_repeated() const { return repeated_; }
    // Moves to `frame` of the current pass (clamped to length()); the stream has not ended.
    void seek(std::uint64_t frame);
    // Frame 0, no pass repeated, not ended (a player that plays again after its end).
    void restart();
    void set_loops(std::int32_t loops) { loops_.store(loops); }
    std::int32_t loops() const { return loops_.load(); }
    // Whether read() has run out: the last pass ended and no loop was left.
    bool ended() const { return ended_.load(); }

  private:
    struct Decoder;    // what the frames come from (mp3_stream.cpp)
    struct Mp3Decoder; // dr_mp3 over bytes_
    struct PcmDecoder; // decoded PCM in memory (a pack's WAV)
    Mp3Stream();
    void trim_silence();              // gapless: start_ and length_ to the first and last sound
    void set_loop(const Loop *loop);  // a pack's loop, clamped to the file
    std::vector<std::uint8_t> bytes_; // the file (dr_mp3 reads it in place)
    std::unique_ptr<Decoder> dec_;
    std::uint32_t priming_ = kPriming; // decoder frames before pass frame 0 (a pack's: 0)
    std::uint64_t loop_start_ = 0;     // where a repeat begins (0 without a loop)
    bool has_loop_ = false;
    std::uint32_t rate_ = 0, channels_ = 0;
    std::uint64_t full_ = 0;    // frames of Apple's pass
    std::uint64_t start_ = 0;   // Apple frame of pass frame 0 (gapless: the first sound)
    std::uint64_t length_ = 0;  // frames per pass (gapless: up to the last sound)
    std::uint64_t audible_ = 0; // of them, decoded: full_ - kPriming - start_ at most (the
                                // rest is silence)
    std::uint64_t pos_ = 0;
    std::uint32_t repeated_ = 0;
    std::atomic<std::int32_t> loops_{0};
    std::atomic<bool> ended_{false};
};

// An audio pack's MP3 decoded whole to 16-bit interleaved PCM (#188: an effect, which the game
// reads in one piece): false if dr_mp3 finds no audio, not 1 or 2 channels, or more than
// `max_seconds` long (counted before anything is decoded).
bool decode_mp3(const std::vector<std::uint8_t> &bytes, std::uint32_t max_seconds,
                std::vector<std::int16_t> *samples, std::uint32_t *rate, std::uint32_t *channels);

} // namespace mt_audio

#endif // MAPLETHIEF_RUNTIME_AUDIO_MP3_STREAM_H
