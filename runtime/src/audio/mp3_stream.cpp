// An MP3 file as a music stream (mp3_stream.h, #63, ADR 0046). The only file that defines
// DR_MP3_IMPLEMENTATION (ADR 0003); compiled without floating-point contraction
// (maplethief_strict_fp), with dr_mp3's scalar float path, so the decode is the same on every
// host.
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD
#define DR_MP3_NO_STDIO
#include "audio/mp3_stream.h"

#include <algorithm>
#include <cstring>

#include <dr_mp3.h> // a SYSTEM include directory: its warnings are not ours

namespace mt_audio {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Where a stream's frames come from: decoder frame 0 is the first frame the decoder gives.
struct Mp3Stream::Decoder {
    virtual ~Decoder() = default;
    virtual bool seek(u64 frame) = 0;
    virtual u32 read(std::int16_t *out, u32 frames) = 0;
};

struct Mp3Stream::Mp3Decoder final : Mp3Stream::Decoder {
    drmp3 mp3{};
    bool open = false;
    ~Mp3Decoder() override {
        if (open) {
            drmp3_uninit(&mp3);
        }
    }
    bool seek(u64 frame) override { return drmp3_seek_to_pcm_frame(&mp3, frame) != 0; }
    u32 read(std::int16_t *out, u32 frames) override {
        return static_cast<u32>(drmp3_read_pcm_frames_s16(&mp3, frames, out));
    }
};

struct Mp3Stream::PcmDecoder final : Mp3Stream::Decoder {
    std::vector<std::int16_t> samples;
    u32 channels = 1;
    u64 frames = 0, at = 0;
    bool seek(u64 frame) override {
        at = std::min(frame, frames);
        return true;
    }
    u32 read(std::int16_t *out, u32 n) override {
        const u32 got = static_cast<u32>(std::min<u64>(n, frames - at));
        std::memcpy(out, samples.data() + at * channels,
                    std::size_t{got} * channels * sizeof(std::int16_t));
        at += got;
        return got;
    }
};

Mp3Stream::Mp3Stream() = default;
Mp3Stream::~Mp3Stream() = default;

namespace {

// dr_mp3 over `bytes` (which must outlive it); false if it finds no audio or not 1-2 channels.
bool init_mp3(drmp3 *mp3, const std::vector<std::uint8_t> &bytes) {
    if (bytes.empty() || !drmp3_init_memory(mp3, bytes.data(), bytes.size(), nullptr)) {
        return false;
    }
    if (mp3->sampleRate == 0 || mp3->channels < 1 || mp3->channels > 2) {
        drmp3_uninit(mp3);
        return false;
    }
    return true;
}

} // namespace

std::shared_ptr<Mp3Stream> Mp3Stream::open(std::vector<std::uint8_t> bytes, bool gapless) {
    std::shared_ptr<Mp3Stream> s(new Mp3Stream());
    s->bytes_ = std::move(bytes); // dr_mp3 keeps a pointer to these bytes: never moved again
    auto dec = std::make_unique<Mp3Decoder>();
    if (!init_mp3(&dec->mp3, s->bytes_)) {
        return nullptr;
    }
    dec->open = true;
    drmp3 &m = dec->mp3;
    s->dec_ = std::move(dec);
    s->rate_ = m.sampleRate;
    s->channels_ = m.channels;
    // Every frame's samples: dr_mp3 trims nothing without a Xing/LAME info frame, and these
    // files have none (with one, its delay and padding would already be left out here).
    s->length_ = drmp3_get_pcm_frame_count(&m);
    if (s->length_ == 0) {
        return nullptr;
    }
    s->audible_ = s->length_ > kPriming ? s->length_ - kPriming : 0;
    s->full_ = s->length_;
    if (gapless) {
        s->trim_silence();
    }
    s->seek(0);
    return s;
}

std::shared_ptr<Mp3Stream> Mp3Stream::open_pack(std::vector<std::uint8_t> bytes, const Loop *loop,
                                                bool gapless, u32 max_seconds) {
    std::shared_ptr<Mp3Stream> s(new Mp3Stream());
    s->bytes_ = std::move(bytes);
    auto dec = std::make_unique<Mp3Decoder>();
    if (!init_mp3(&dec->mp3, s->bytes_)) {
        return nullptr;
    }
    dec->open = true;
    drmp3 &m = dec->mp3;
    s->dec_ = std::move(dec);
    s->rate_ = m.sampleRate;
    s->channels_ = m.channels;
    s->priming_ = 0;
    // A file with a Xing/LAME info frame: dr_mp3 leaves out the encoder's delay and padding.
    s->length_ = drmp3_get_pcm_frame_count(&m);
    if (s->length_ == 0 || s->length_ > u64{s->rate_} * max_seconds) {
        return nullptr;
    }
    s->audible_ = s->full_ = s->length_;
    s->set_loop(loop);
    if (gapless && !s->has_loop_) {
        s->trim_silence();
    }
    s->seek(0);
    return s;
}

std::shared_ptr<Mp3Stream> Mp3Stream::open_pcm(std::vector<std::int16_t> samples, u32 rate,
                                               u32 channels, const Loop *loop, bool gapless) {
    if (rate == 0 || channels < 1 || channels > 2 || samples.size() < channels) {
        return nullptr;
    }
    std::shared_ptr<Mp3Stream> s(new Mp3Stream());
    auto dec = std::make_unique<PcmDecoder>();
    dec->channels = channels;
    dec->frames = samples.size() / channels;
    dec->samples = std::move(samples);
    s->rate_ = rate;
    s->channels_ = channels;
    s->priming_ = 0;
    s->length_ = s->audible_ = s->full_ = dec->frames;
    s->dec_ = std::move(dec);
    s->set_loop(loop);
    if (gapless && !s->has_loop_) {
        s->trim_silence();
    }
    s->seek(0);
    return s;
}

void Mp3Stream::set_loop(const Loop *loop) {
    if (loop == nullptr) {
        return;
    }
    const u64 end = std::min(loop->end, length_);
    if (loop->start >= end) {
        return; // empty once clamped: no loop
    }
    has_loop_ = true;
    loop_start_ = loop->start;
    length_ = end;
    audible_ = std::min(audible_, length_);
}

bool decode_mp3(const std::vector<std::uint8_t> &bytes, u32 max_seconds,
                std::vector<std::int16_t> *samples, u32 *rate, u32 *channels) {
    drmp3 m{};
    if (!init_mp3(&m, bytes)) {
        return false;
    }
    bool ok = false;
    const u64 frames = drmp3_get_pcm_frame_count(&m);
    if (frames != 0 && frames <= u64{m.sampleRate} * max_seconds &&
        drmp3_seek_to_pcm_frame(&m, 0)) {
        samples->assign(static_cast<std::size_t>(frames) * m.channels, 0);
        const u64 got = drmp3_read_pcm_frames_s16(&m, frames, samples->data());
        samples->resize(static_cast<std::size_t>(got) * m.channels);
        *rate = m.sampleRate;
        *channels = m.channels;
        ok = got != 0;
    }
    drmp3_uninit(&m);
    return ok;
}

// Decodes the pass once (from Apple frame 0, as read() would) and keeps the frames from the
// first to the last one with a sample other than 0. The threshold is exact zeros: digital
// silence, never quiet music (ADR 0062). The 529 silent tail frames are always dropped with
// the rest of the trailing silence.
void Mp3Stream::trim_silence() {
    seek(0);
    constexpr u32 kBlock = 4096;
    std::vector<std::int16_t> buf(std::size_t{kBlock} * channels_);
    bool any = false;
    u64 first = 0, last = 0, p = 0;
    while (p < audible_) {
        const u32 n = static_cast<u32>(std::min<u64>(kBlock, audible_ - p));
        const u32 got = static_cast<u32>(dec_->read(buf.data(), n));
        for (u32 i = 0; i < got; i++) {
            bool sound = false;
            for (u32 c = 0; c < channels_; c++) {
                sound = sound || buf[std::size_t{i} * channels_ + c] != 0;
            }
            if (sound) {
                first = any ? first : p + i;
                last = p + i;
                any = true;
            }
        }
        p += got;
        if (got < n) {
            break; // fewer frames than counted (never seen): the rest is silence
        }
    }
    if (!any) {
        return; // silent throughout: the whole pass, as without the option
    }
    start_ = first;
    length_ = last + 1 - first;
    audible_ = length_;
}

void Mp3Stream::seek(u64 frame) {
    pos_ = std::min(frame, length_);
    ended_.store(false); // the rest of this pass plays (then the stream ends again, or loops)
    // Decoder frame start_ + pos_ + priming_ (dr_mp3 goes back to the start and decodes up to
    // it; for frame 0 without the gapless trim that is one MP3 frame). In the silent tail there
    // is nothing to decode.
    if (pos_ < audible_ && !dec_->seek(start_ + pos_ + priming_)) {
        audible_ = pos_; // a stream that cannot seek plays silence from here (never seen)
    }
}

void Mp3Stream::restart() {
    repeated_ = 0;
    seek(0);
}

u32 Mp3Stream::read(std::int16_t *out, u32 frames) {
    u32 done = 0;
    while (done < frames) {
        if (pos_ >= length_) {
            const std::int32_t loops = loops_.load();
            if (loops >= 0 && repeated_ >= static_cast<u32>(loops)) {
                ended_.store(true);
                break;
            }
            repeated_++;
            seek(loop_start_); // 0 unless a pack's loop says otherwise
            continue;
        }
        std::int16_t *dst = out + std::size_t{done} * channels_;
        u32 n = static_cast<u32>(std::min<u64>(frames - done, length_ - pos_));
        if (pos_ < audible_) {
            n = static_cast<u32>(std::min<u64>(n, audible_ - pos_));
            const u32 got = static_cast<u32>(dec_->read(dst, n));
            if (got < n) { // fewer frames than counted (never seen): silence for the rest
                std::memset(dst + std::size_t{got} * channels_, 0,
                            std::size_t{n - got} * channels_ * sizeof(std::int16_t));
                audible_ = pos_ + got;
            }
        } else {
            std::memset(dst, 0, std::size_t{n} * channels_ * sizeof(std::int16_t));
        }
        pos_ += n;
        done += n;
    }
    return done;
}

} // namespace mt_audio
