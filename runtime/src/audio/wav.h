// A PCM WAV file's format, data and loop (#62, #188): the RIFF chunk walk that ExtAudioFile
// (audio.cpp, the game's effects) and audio packs (audio_pack.cpp) share. The file is untrusted
// data: every read is bounds-checked.
//
//   accepted   RIFF/WAVE with a "fmt " chunk of WAVE_FORMAT_PCM (tag 1), 1 to 8 channels,
//              1 to 384,000 Hz, 8 (unsigned), 16, 24 or 32-bit integer samples, a block align of
//              channels x bytes per sample, and a "data" chunk (the first one; a truncated last
//              data chunk counts up to the end of the file). Chunks are word-aligned; other
//              chunks are skipped.
//   the loop   (packs, #188) the first loop of a "smpl" chunk (its start and its inclusive end,
//              in frames), if it is inside the data and not empty.
#ifndef MAPLETHIEF_RUNTIME_AUDIO_WAV_H
#define MAPLETHIEF_RUNTIME_AUDIO_WAV_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mt_audio::wav {

struct Info {
    std::uint32_t channels = 0, rate = 0, bits = 0, block_align = 0;
    std::uint64_t frames = 0; // whole frames in the data chunk
    std::size_t data_at = 0;  // the data chunk's first byte in the file
    bool has_loop = false;    // a "smpl" loop, frames [loop_start, loop_end)
    std::uint64_t loop_start = 0, loop_end = 0;
};

// False if `b` is no PCM WAV as above.
bool parse(const std::vector<std::uint8_t> &b, Info *out);

// The frames of `b` (parsed into `w`) as 16-bit signed samples: the top 16 bits of each sample
// (8-bit is unsigned, 128 the zero), interleaved as in the file.
std::vector<std::int16_t> to_s16(const std::vector<std::uint8_t> &b, const Info &w);

} // namespace mt_audio::wav

#endif // MAPLETHIEF_RUNTIME_AUDIO_WAV_H
