// Audio packs (#188, ADR 0106, maplethief/audio.h): what the two places that open the game's
// sound files ask before they read the bundle's file. ExtAudioFileOpenURL (audio.cpp) asks
// effect() for each WAV the game decodes at start-up; -[AVAudioPlayer initWithContentsOfURL:
// error:] (avfoundation.cpp) asks music() for each track. Both answer from the pack open now
// (mt_audio_pack_open), by the bundle file's name; false / null: no pack, no file for that name,
// or a file that was rejected (logged once), so the caller opens the game's own file.
#ifndef MAPLETHIEF_RUNTIME_AUDIO_AUDIO_PACK_H
#define MAPLETHIEF_RUNTIME_AUDIO_AUDIO_PACK_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mt_audio {

class Mp3Stream;

namespace pack {

// An effect decoded whole: 16-bit interleaved PCM.
struct Sound {
    std::vector<std::int16_t> samples;
    std::uint32_t rate = 0, channels = 0;
    std::string file; // the pack file's path (UTF-8), for the trace
};

// The pack's replacement for the effect at host path `path` (its file name is what counts).
bool effect(const std::string &path, Sound *out);

// The pack's replacement for the music track at host path `path`: a stream on the pack file's
// own timeline with its loop (Mp3Stream::open_pack / open_pcm), gapless as `gapless` says when
// it has no loop. *file: the pack file's path, for the trace.
std::shared_ptr<Mp3Stream> music(const std::string &path, bool gapless, std::string *file);

} // namespace pack
} // namespace mt_audio

#endif // MAPLETHIEF_RUNTIME_AUDIO_AUDIO_PACK_H
