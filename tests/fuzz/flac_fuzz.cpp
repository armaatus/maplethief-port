// A FLAC of an audio pack through dr_flac (runtime/src/audio/mp3_stream.cpp, #188): the music
// path (Mp3Stream::open_pack with a loop, then read as the mixer pulls it) and the effects'
// (decode_file), both with the length checked before anything is decoded.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/mp3_stream.h"

namespace {

constexpr std::uint32_t kReadFrames = 1u << 15;
constexpr std::uint32_t kBlock = 1024;
constexpr std::uint32_t kMaxSeconds = 20;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);
    // A loop from the input's own bytes, so that clamping it to the file is exercised too.
    mt_audio::Mp3Stream::Loop loop;
    loop.start = size >= 2 ? static_cast<std::uint64_t>(data[size - 2]) << 8 : 0;
    loop.end = loop.start + (size >= 1 ? 1 + static_cast<std::uint64_t>(data[size - 1]) * 64 : 1);
    const bool with_loop = size % 3 == 0;
    if (const std::shared_ptr<mt_audio::Mp3Stream> s = mt_audio::Mp3Stream::open_pack(
            bytes, mt_audio::Mp3Stream::Codec::kFlac, with_loop ? &loop : nullptr, size % 2 == 1,
            kMaxSeconds)) {
        if (s->channels() < 1 || s->channels() > 2) {
            __builtin_trap();
        }
        s->set_loops(1);
        std::vector<std::int16_t> out(static_cast<std::size_t>(kBlock) * 2);
        for (std::uint32_t done = 0; done < kReadFrames && !s->ended();) {
            const std::uint32_t n = s->read(out.data(), kBlock);
            done += n == 0 ? kBlock : n;
        }
    }
    std::vector<std::int16_t> samples;
    std::uint32_t rate = 0, channels = 0;
    if (mt_audio::decode_file(bytes, mt_audio::Mp3Stream::Codec::kFlac, kMaxSeconds, &samples,
                              &rate, &channels) &&
        (channels < 1 || channels > 2 || samples.size() % channels != 0)) {
        __builtin_trap();
    }
    return 0;
}
