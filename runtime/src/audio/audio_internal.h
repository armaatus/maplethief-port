// Audio, silent (#117, ADR 0038, maplethief/audio.h): what audio.cpp (OpenAL, AudioToolbox,
// ExtAudioFile) and avfoundation.cpp (AVAudioSession, AVAudioPlayer) share. The HLE functions
// are listed by import name in runtime/src/hle/registry.cpp, the classes in
// runtime/src/objc/classes.cpp.
#ifndef MAPLETHIEF_RUNTIME_AUDIO_AUDIO_INTERNAL_H
#define MAPLETHIEF_RUNTIME_AUDIO_AUDIO_INTERNAL_H

#include <cstdint>
#include <string>

#include "maplethief/audio.h"
#include "maplethief/objc/host.h"

extern "C" {
// OpenAL (OpenAL.framework)
void mt_hle_alcOpenDevice(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcCloseDevice(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcCreateContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcDestroyContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcMakeContextCurrent(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcGetCurrentContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcGetContextsDevice(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcProcessContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcSuspendContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alcGetProcAddress(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alGetError(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alGenBuffers(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alDeleteBuffers(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alGenSources(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alDeleteSources(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alBufferData(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alSourcei(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alSourcePlay(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_alListenerf(mt_cpu *cpu, std::uint32_t id) noexcept;
// AudioToolbox: the audio session and system sounds
void mt_hle_AudioSessionInitialize(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_AudioSessionSetActive(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_AudioSessionSetProperty(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_AudioServicesPlaySystemSound(mt_cpu *cpu, std::uint32_t id) noexcept;
// AudioToolbox: ExtAudioFile
void mt_hle_ExtAudioFileOpenURL(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_ExtAudioFileGetProperty(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_ExtAudioFileSetProperty(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_ExtAudioFileRead(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_ExtAudioFileDispose(mt_cpu *cpu, std::uint32_t id) noexcept;
}

namespace mt_audio {

using u32 = std::uint32_t;

// The counters of the current process (a new current ObjC runtime starts from zero).
mt_audio_stats &stats();

// The host path of a file URL (an NSURL / CFURL) that the guest may read (the bundle or the
// home directory); "" if `url` is none or not such a file.
std::string url_host_path(mt_cpu *cpu, u32 url);

// AVFoundation (avfoundation.cpp)
extern const mt_objc_host_class k_avaudiosession;
extern const mt_objc_host_class k_avaudioplayer;

} // namespace mt_audio

#endif // MAPLETHIEF_RUNTIME_AUDIO_AUDIO_INTERNAL_H
