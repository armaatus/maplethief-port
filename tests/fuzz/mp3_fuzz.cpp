// An MP3 through dr_mp3 (runtime/src/audio/mp3_stream.cpp, #63, #188): the game's music path
// (Mp3Stream::open, then read as the mixer pulls it, gapless or not) and an audio pack's
// (open_pack, its length checked first, then decode_whole).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/mp3_stream.h"
#include "fuzz_check.h"

namespace {

// Pulls at most this many frames from a stream, as the mixer would, a block at a time.
constexpr std::uint32_t kReadFrames = 1u << 15;
constexpr std::uint32_t kBlock = 1024;
// An audio pack's limit, kept short so that a long file is refused rather than decoded.
constexpr std::uint32_t kMaxSeconds = 20;

void pull(mt_audio::Mp3Stream &s) {
    std::vector<std::int16_t> out(static_cast<std::size_t>(kBlock) * 2);
    for (std::uint32_t done = 0; done < kReadFrames && !s.ended();) {
        const std::uint32_t n = s.read(out.data(), kBlock);
        if (n > kBlock) {
            FUZZ_FAIL();
        }
        done += n == 0 ? kBlock : n;
    }
    s.seek(s.length() / 2);
    (void)s.read(out.data(), kBlock);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);
    const bool gapless = size % 2 == 1;
    if (const std::shared_ptr<mt_audio::Mp3Stream> s = mt_audio::Mp3Stream::open(bytes, gapless)) {
        if (s->channels() < 1 || s->channels() > 2) {
            FUZZ_FAIL();
        }
        pull(*s);
    }
    // An effect: the stream opened without a loop, then decoded whole (decode_whole, #413).
    if (const std::shared_ptr<mt_audio::Mp3Stream> e = mt_audio::Mp3Stream::open_pack(
            bytes, mt_audio::Mp3Stream::Codec::kMp3, nullptr, false, kMaxSeconds)) {
        std::vector<std::int16_t> samples;
        if (mt_audio::decode_whole(*e, &samples) &&
            (e->channels() < 1 || e->channels() > 2 || samples.size() % e->channels() != 0)) {
            FUZZ_FAIL();
        }
    }
    return 0;
}
