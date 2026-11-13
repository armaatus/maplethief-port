// A WAV of an audio pack or of the game's effects (runtime/src/audio/wav.cpp, #62, #188): the
// RIFF chunk walk, then the samples as 16-bit, one per channel per frame.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/wav.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);
    mt_audio::wav::Info info;
    if (!mt_audio::wav::parse(bytes, &info)) {
        return 0;
    }
    if (info.has_loop && (info.loop_start >= info.loop_end || info.loop_end > info.frames)) {
        __builtin_trap();
    }
    const std::vector<std::int16_t> samples = mt_audio::wav::to_s16(bytes, info);
    if (samples.size() != info.frames * info.channels) {
        __builtin_trap();
    }
    return 0;
}
