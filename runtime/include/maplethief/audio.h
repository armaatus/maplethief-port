/* Audio, silent (#117, ADR 0038): the OpenAL, AudioToolbox (AudioSession, system sounds,
 * ExtAudioFile) imports and the AVFoundation classes (AVAudioSession, AVAudioPlayer) the game
 * reaches accept every call, keep the state the game reads back or passes again (device and
 * context handles, buffer and source names, the error state, players and their play state)
 * and make no sound. Real audio is M6: OpenAL onto SDL3 streams (#61), ExtAudioFile decoding
 * (#62), AVAudioPlayer MP3 playback (#63), the audio session and volume (#64). They replace
 * these files behind the same imports and classes (runtime/src/audio/).
 *
 *   OpenAL          alcOpenDevice/alcCreateContext return handles on the guest heap,
 *                   alGen* names, alGetError the error the last bad call set (AL_NO_ERROR
 *                   for the game's calls), alBufferData keeps the format, size and rate,
 *                   alSourcePlay counts.
 *   AudioSession    initialised once, active, the category kept; the interruption listener
 *                   is never called (no interruptions).
 *   ExtAudioFile    opens a bundle WAV (RIFF chunk walk: format and length), answers the
 *                   file format and frame count, accepts the client format, and reads
 *                   silence of the right length. #62 decodes the samples.
 *   AVAudioPlayer   created for a file that exists, -play / -stop / -isPlaying keep the
 *                   play state (a looping player plays until stopped), volume, loops and
 *                   the time are kept.
 *
 * Trace: MAPLETHIEF_TRACE=audio logs every call. Per process: a new current ObjC runtime
 * starts from scratch. Single guest thread (ADR 0033). C17 that also compiles as C++20. */
#ifndef MAPLETHIEF_AUDIO_H
#define MAPLETHIEF_AUDIO_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

typedef struct mt_audio_stats {
    uint32_t calls;          /* audio imports and AVFoundation messages */
    uint32_t buffers;        /* OpenAL buffer names alive */
    uint32_t buffers_filled; /* of them, with alBufferData */
    uint32_t sources;        /* OpenAL source names alive */
    uint32_t source_plays;   /* alSourcePlay calls */
    uint32_t files_opened;   /* ExtAudioFileOpenURL successes */
    uint32_t files_failed;   /* ExtAudioFileOpenURL failures */
    uint32_t players;        /* AVAudioPlayers created */
    uint32_t players_playing;
    uint32_t system_sounds; /* AudioServicesPlaySystemSound calls (0xFFF: vibrate) */
    uint32_t session_active;
} mt_audio_stats;

/* This process's numbers (zeros before the first audio call). */
mt_audio_stats mt_audio_get_stats(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_AUDIO_H */
