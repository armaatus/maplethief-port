/* Audio (ADR 0038, ADR 0045): the OpenAL, AudioToolbox (AudioSession, system sounds,
 * ExtAudioFile) imports and the AVFoundation classes (AVAudioSession, AVAudioPlayer) the game
 * reaches, and the mixer they play through, which the platform pulls.
 *
 *   OpenAL          real (#61): alcOpenDevice/alcCreateContext return handles on the guest
 *                   heap, alGen* names, alGetError the first error since the last call
 *                   (AL_NO_ERROR for the game's calls; AL_INVALID_OPERATION without a current
 *                   context), alBufferData copies the guest's PCM (MONO8/16, STEREO8/16) into
 *                   host memory, alSourcePlay (re)starts the source's voice at frame 0,
 *                   alListenerf(AL_GAIN) scales every effect, alcSuspendContext and
 *                   alcMakeContextCurrent(NULL) pause the effects, alcProcessContext resumes
 *                   them. alcGetProcAddress answers NULL (no extension functions; the game's
 *                   only request, alBufferDataStatic, is in a branch that cannot run).
 *   AudioSession    initialised once (again: 'init'), active, the category kept; calls before
 *                   AudioSessionInitialize answer '!ini', a category of the wrong size '!siz',
 *                   as on the device. The interruption listener is kept and never called: a
 *                   desktop has no phone calls or alarms (#64).
 *   system sounds   AudioServicesPlaySystemSound(kSystemSoundID_Vibrate = 0xFFF) calls the
 *                   host's vibrate hook (mt_audio_set_vibrate_hook; #72 rumbles a gamepad);
 *                   every other id is counted and does nothing.
 *   ExtAudioFile    opens a bundle WAV (RIFF chunk walk: format, length, the data chunk),
 *                   answers the file format and frame count, accepts a client format of
 *                   16-bit signed PCM (or the file's own) at the file's rate and channels,
 *                   and reads the file's samples in it (#62).
 *   AVAudioPlayer   real (#63, ADR 0046): an MP3 file streamed by dr_mp3 as a music voice
 *                   of the mixer, on Apple's decoder timeline (the 529-frame decoder delay
 *                   dropped, 529 silent frames at the end of a pass), looped whole as
 *                   numberOfLoops says; -play / -stop (the exact frame kept) / -isPlaying,
 *                   -setVolume:, -setCurrentTime:. nil for a file without MP3 audio.
 *                   Optionally gapless (mt_audio_set_gapless_music, #125, ADR 0062).
 *
 * The mixer (runtime/src/audio/mixer.h) renders MT_AUDIO_RATE Hz, MT_AUDIO_CHANNELS channels,
 * interleaved float in [-1, 1], deterministically: the same calls give the same samples on
 * every OS and CPU. It is pulled with mt_audio_render from any host thread (the platform's
 * device stream, a WAV capture, a test); the host's buses and mute multiply the game's own
 * gains and never change what the game reads back.
 *
 * Trace: MAPLETHIEF_TRACE=audio logs every call. Per process: a new current ObjC runtime
 * starts from scratch (every voice stops; the buses, mute and output stats stay). The guest
 * side runs on the single guest thread (ADR 0033); the pull API is thread-safe. C17 that also
 * compiles as C++20. */
#ifndef MAPLETHIEF_AUDIO_H
#define MAPLETHIEF_AUDIO_H

#include <stdbool.h>
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

/* --- the mixer's output (ADR 0045) ------------------------------------------------------- */

#define MT_AUDIO_RATE 44100u /* Hz */
#define MT_AUDIO_CHANNELS 2u /* interleaved left, right */

/* Mixes the next `frames` frames into `out` (MT_AUDIO_CHANNELS * frames floats, overwritten):
 * silence when nothing plays. Never waits for the guest; any host thread. */
void mt_audio_render(float *out, uint32_t frames) MT_NOEXCEPT;

/* The host's gain stages (user settings, #64). Effects are mixed at listener gain x EFFECTS,
 * music at the player's volume x MUSIC, and the sum at MASTER (0 while muted). */
typedef enum mt_audio_bus {
    MT_AUDIO_BUS_MASTER = 0,
    MT_AUDIO_BUS_MUSIC = 1,
    MT_AUDIO_BUS_EFFECTS = 2,
} mt_audio_bus;
#define MT_AUDIO_BUS_COUNT 3u

/* Sets a bus's gain, clamped to [0, 1] (NaN is ignored); all are 1 at start. Takes effect at
 * the next render. */
void mt_audio_set_gain(mt_audio_bus bus, float gain) MT_NOEXCEPT;
float mt_audio_get_gain(mt_audio_bus bus) MT_NOEXCEPT;
/* Mutes or unmutes the whole output (e.g. while the window is unfocused). */
void mt_audio_set_muted(bool muted) MT_NOEXCEPT;
bool mt_audio_get_muted(void) MT_NOEXCEPT;
/* Mono (#195, off at start): each frame of the mix becomes (L + R) / 2 in both channels, after
 * the voices are summed and before the master bus and the clamp (a clamp that did not bite
 * before does not bite now). Off leaves the mix exactly as without it. Kept across processes. */
void mt_audio_set_mono(bool mono) MT_NOEXCEPT;
bool mt_audio_get_mono(void) MT_NOEXCEPT;

typedef struct mt_audio_output_stats {
    uint64_t frames;            /* frames rendered by mt_audio_render */
    uint64_t renders;           /* mt_audio_render calls */
    uint64_t late;              /* late device callbacks reported by mt_audio_note_late */
    uint32_t longest_render_us; /* the longest mt_audio_render call, in microseconds */
    uint32_t effects_playing;   /* effect voices playing now */
    uint32_t music_playing;     /* music voices playing now */
} mt_audio_output_stats;

/* The output's numbers since the start of the host process (or the last reset). */
mt_audio_output_stats mt_audio_get_output_stats(void) MT_NOEXCEPT;
void mt_audio_reset_output_stats(void) MT_NOEXCEPT;
/* The platform's device callback came late (ADR 0045: more than 1.5 x the duration of the
 * audio its previous call delivered). Any thread. */
void mt_audio_note_late(void) MT_NOEXCEPT;

/* Plays a test tone (a sine of `hz` Hz at half amplitude for `ms` milliseconds, mono 16-bit
 * at 22,050 Hz) as an effect voice outside any OpenAL state, for the manual device check
 * (MapleThief --audio-test-tone). */
void mt_audio_test_tone(float hz, uint32_t ms) MT_NOEXCEPT;

/* --- gapless music loops (#125, ADR 0062) ------------------------------------------------- */

/* Off (the default, the device's behaviour): a player loops its file's whole pass, with the
 * digital silence the game's MP3s carry at their start and end (up to 3.3 s per loop). On: a
 * player opened afterwards (-initWithContentsOfURL:error:; the game opens all of its players at
 * start-up) keeps only the frames from its file's first to its last one that is not exactly 0,
 * so it loops from the last sound straight to the first; -setCurrentTime: and -stop's position
 * are on that shorter pass. Players already open keep what they have. Kept across processes. */
void mt_audio_set_gapless_music(bool on) MT_NOEXCEPT;
bool mt_audio_get_gapless_music(void) MT_NOEXCEPT;

/* --- audio packs (#188, ADR 0106) ----------------------------------------------------------- */

/* A folder of the player's own sound files that replace the game's, by the bundle's file name:
 * for the game's `sound/35.mp3` the pack's `35.mp3`, `35.wav` or `35.flac` (the original's
 * extension first, then .wav, .mp3, .flac; names in any case), for `12.wav` its `12.wav`,
 * `12.mp3` or `12.flac`. A pack
 * is indexed when it opens (its file names only) and each file is read when the game opens its
 * sound: the effects at start-up (ExtAudioFile, decoded whole), each track when its
 * AVAudioPlayer is made.
 *
 *   formats   PCM WAV (8, 16, 24, 32-bit integer), MP3 (dr_mp3) and FLAC (dr_flac, its
 *             header giving its length; 24 and 32-bit as their top 16 bits), 1 or 2 channels,
 *             MT_AUDIO_PACK_MIN_RATE to MT_AUDIO_PACK_MAX_RATE Hz, at most
 *             MT_AUDIO_PACK_MAX_FILE bytes; an effect at most MT_AUDIO_PACK_MAX_EFFECT_SECONDS,
 *             a track at most MT_AUDIO_PACK_MAX_MUSIC_SECONDS long.
 *   music     the file's own samples (no Apple decoder delay emulated; ADR 0046's 529 frames
 *             are the game's MP3s'), looped as the game asks. A loop: `<stem>.loop` in the
 *             pack (text: the start and end frame, end excluded: "start end"), else a WAV's
 *             first "smpl" loop; the first pass plays 0 to end, each repeat start to end.
 *             Without one, gapless_music (ADR 0062) trims the file's silent ends as the game's.
 *   effects   decoded whole to 16-bit PCM at the file's rate and channels; the game reads them
 *             as it reads its WAVs and the mixer resamples them.
 *   untrusted every read is bounds-checked; a file that cannot be read, decoded or is out of
 *             the limits is logged once ("audiopack: ...: rejected, the game's ... kept") and
 *             never read again, and the game's own file plays. A bad .loop is logged and
 *             ignored.
 *
 * Kept across processes (the frontend opens it before the boot). Single guest thread. */
#define MT_AUDIO_PACK_MAX_FILE (128u << 20)   /* bytes */
#define MT_AUDIO_PACK_MIN_RATE 8000u          /* Hz */
#define MT_AUDIO_PACK_MAX_RATE 192000u        /* Hz */
#define MT_AUDIO_PACK_MAX_EFFECT_SECONDS 60u  /* an effect's length */
#define MT_AUDIO_PACK_MAX_MUSIC_SECONDS 1800u /* a track's length (30 minutes) */
#define MT_AUDIO_PACK_MAX_FILES 65536u        /* files indexed (#413); the rest skipped, logged */

typedef struct mt_audio_pack_stats {
    uint32_t files;            /* sound files indexed (.wav, .mp3 and .flac) */
    uint32_t loops;            /* .loop files indexed */
    uint32_t effects;          /* effects the pack was asked about */
    uint32_t effects_replaced; /* of them, replaced by a pack file */
    uint32_t music;            /* tracks the pack was asked about */
    uint32_t music_replaced;   /* of them, replaced by a pack file */
    uint32_t rejected;         /* asked about with a file that was rejected: the game's kept */
} mt_audio_pack_stats;

/* Opens the pack in the directory `dir` (UTF-8): indexes its file names. Returns the sound
 * files indexed, or -1 (logged) if `dir` is no directory. A pack open before is closed first. */
int mt_audio_pack_open(const char *dir) MT_NOEXCEPT;
void mt_audio_pack_close(void) MT_NOEXCEPT;
/* The counters since the pack opened (zeros without one: closing it clears them). */
mt_audio_pack_stats mt_audio_pack_get_stats(void) MT_NOEXCEPT;

/* --- the vibrate hook (#64) ------------------------------------------------------------------ */

/* Called on the guest thread for each AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
 * (0xFFF), the game's CMSoundPlayer::Vibrate when its vibration option is on. The device's
 * vibration has no duration (the game's milliseconds are ignored). It must not call back into
 * the guest. */
typedef void (*mt_audio_vibrate_fn)(void *user);

/* Installs the hook (NULL: none, the default: vibrations are only counted). Kept across
 * processes. */
void mt_audio_set_vibrate_hook(mt_audio_vibrate_fn hook, void *user) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_AUDIO_H */
