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

struct Mp3Stream::Decoder {
    drmp3 mp3{};
    bool open = false;
    ~Decoder() {
        if (open) {
            drmp3_uninit(&mp3);
        }
    }
};

Mp3Stream::Mp3Stream() : dec_(std::make_unique<Decoder>()) {}
Mp3Stream::~Mp3Stream() = default;

std::shared_ptr<Mp3Stream> Mp3Stream::open(std::vector<std::uint8_t> bytes, bool gapless) {
    std::shared_ptr<Mp3Stream> s(new Mp3Stream());
    s->bytes_ = std::move(bytes); // dr_mp3 keeps a pointer to these bytes: never moved again
    if (s->bytes_.empty() ||
        !drmp3_init_memory(&s->dec_->mp3, s->bytes_.data(), s->bytes_.size(), nullptr)) {
        return nullptr;
    }
    s->dec_->open = true;
    drmp3 &m = s->dec_->mp3;
    if (m.sampleRate == 0 || m.channels < 1 || m.channels > 2) {
        return nullptr;
    }
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
        const u32 got = static_cast<u32>(drmp3_read_pcm_frames_s16(&dec_->mp3, n, buf.data()));
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
    // Decoder frame start_ + pos_ + kPriming (dr_mp3 goes back to the start and decodes up to
    // it; for frame 0 without the gapless trim that is one MP3 frame). In the silent tail there
    // is nothing to decode.
    if (pos_ < audible_ && !drmp3_seek_to_pcm_frame(&dec_->mp3, start_ + pos_ + kPriming)) {
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
            seek(0);
            continue;
        }
        std::int16_t *dst = out + std::size_t{done} * channels_;
        u32 n = static_cast<u32>(std::min<u64>(frames - done, length_ - pos_));
        if (pos_ < audible_) {
            n = static_cast<u32>(std::min<u64>(n, audible_ - pos_));
            const u32 got = static_cast<u32>(drmp3_read_pcm_frames_s16(&dec_->mp3, n, dst));
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
