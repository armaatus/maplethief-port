# 45. Audio: a deterministic mixer in the runtime, pulled by one SDL3 audio stream

- **Status:** Proposed
- **Date:** 2026-10-29
- **Issue:** #61
- **Builds on:** [ADR 0003](0003-dependencies.md) (our own OpenAL layer, SDL3, dr_mp3), [ADR 0033](0033-gcd-worker-threads-and-main-queue.md) (one guest thread), [ADR 0038](0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md) (silent audio: handles, names, error rules), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (the same split for graphics: deterministic core in `runtime/`, SDL only in `platform/`), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the virtual clock)

## Context

The game's sound is small and simple ([re/audio.md](../re/audio.md)):

- **Effects:** 26 WAVs, 16-bit PCM, mono 8 kHz (13), mono 22.05 kHz (11), stereo 44.1 kHz (2). All are decoded at start-up into one OpenAL buffer and one source per id. A source is only ever played (`alSourcePlay`): never looped, stopped, positioned or given a gain. Replaying a playing source restarts it. The only other AL state is the listener gain (0 or 1: the effects switch).
- **Music:** 18 MP3s (44.1 kHz and 22.05 kHz stereo), one `AVAudioPlayer` per track, looping for ever at volume 0.7 or 0. In practice one track plays at a time. `-stop` keeps the position, `setCurrentTime:0` rewinds.
- **No 3D audio, no streaming AL buffers, no queueing.** The game asks `alcGetProcAddress` only for `alBufferDataStatic`, in a branch that can't run.

ADR 0003 decided on our own OpenAL layer "onto SDL3 audio streams". ADR 0038 then built the silent version in `runtime/src/audio/`, and `runtime/` does not link SDL3 (graphics made the same choice in ADR 0039: the core is deterministic and SDL-free, the platform presents). What is open is where mixing and resampling happen and what crosses the runtime/platform line.

M6's exit criteria: all 44 sounds play when triggered, the music loops and switches per map, no dropouts over 30 minutes.

## Options considered

1. **One SDL_AudioStream per AL source and per player**, all bound to one device. SDL mixes and resamples.
   - Pro: least code of ours.
   - Con: `runtime/` would link SDL3, and every test would need an audio device (or SDL's dummy driver) to hear anything. The mix is SDL's, so output is not byte-stable across SDL versions, and can't be checked in a test except by ear.
   - Con: "restart a playing source" becomes clear-and-refill a stream from the guest thread while the device thread reads it.
2. **Our own mixer in `runtime/`, pulled by one SDL3 audio stream in `platform/`.** The runtime renders a fixed output format on request; the platform opens one device stream whose callback calls it.
   - Pro: deterministic. The same calls give the same samples on every OS and in the Linux container, so tests compare mixed output exactly (synthetic tones, a capture of a scripted boot). The mixer is about 300 lines: 44 one-shot voices and one or two music streams.
   - Pro: the runtime stays SDL-free, as for graphics.
   - Con: we write the resampler (linear interpolation is enough for 8 kHz effects and is what Apple's OpenAL did at its default quality).
3. **OpenAL Soft.** Rejected in ADR 0003 (size, LGPL, still needs every thunk).

## Decision

- **Option 2.** The mixer lives in `runtime/src/audio/mixer.{h,cpp}` (internal) behind a pull API in `maplethief/audio.h`. `platform/` owns the device.
- **Output format:** 44,100 Hz, stereo, interleaved `float` in [−1, 1] (`MT_AUDIO_RATE`, `MT_AUDIO_CHANNELS`). It is the rate of most of the content (the 22.05 kHz files are an exact 2×). SDL's stream converts to the device's own rate and format.
- **Pull API** (C, callable from any host thread):
  - `void mt_audio_render(float *out, uint32_t frames)`: mixes the next `frames` frames into `out` (overwrites it). Always produces output, silence when nothing plays; never blocks on the guest.
  - `mt_audio_set_gain(mt_audio_bus bus, float gain)` for the buses `MT_AUDIO_BUS_MASTER`, `MT_AUDIO_BUS_MUSIC`, `MT_AUDIO_BUS_EFFECTS` (user settings, #64), and `mt_audio_set_muted(bool)` (unfocused window). These multiply the game's own gains; they never change what the game reads back.
  - `mt_audio_output_stats()`: frames rendered, render calls, the longest render in microseconds and the number of late callbacks reported by the platform (`mt_audio_note_late`), for the soak test.
- **Voices.**
  - **Effects:** an AL source with a filled buffer is a voice. `alSourcePlay` (re)starts it at frame 0. Each voice keeps a 32.32 fixed-point read position and step (`buffer_rate / 44100`), interpolates linearly between 16-bit samples, and is mixed at `listener_gain × effects_bus`. Mono goes to both channels at unit gain. Fixed-point stepping keeps the result identical on every host.
  - **`alBufferData` copies** the guest's bytes into host memory (the game frees its buffer right after). If `alBufferDataStatic` is ever requested and exists, it keeps a guest pointer, which the mixer reads only under the lock described below; it is not needed for the shipped game and #61 decides whether to provide it (`alcGetProcAddress` returning NULL is the device-compatible answer for anything not provided).
  - **Music** (#63): a player that is playing is a music voice that pulls decoded PCM from its decoder (dr_mp3, streamed in chunks inside `mt_audio_render`, not decoded whole: the game never frees a player). Looping, position, volume (`setVolume:`) and stop/play follow `AVAudioPlayer`. The music voice is mixed at `player_volume × music_bus`.
  - The sum is multiplied by the master bus (0 when muted) and clamped to [−1, 1].
- **Threads and the lock.** The guest thread changes voices (inside HLE calls), the device thread renders. One mutex guards the mixer's voice table; it is held only for short host-side work (start a voice, copy a buffer, render a block), never across guest code. The guest never waits on the audio thread. Music decoding happens under the same lock in blocks of at most 4,096 frames; if that proves too slow for the callback, decoding moves to a ring buffer filled outside the lock (the soak test shows which).
- **The platform** (`platform/src/audio_out.{h,cpp}`): `SDL_Init(SDL_INIT_AUDIO)`, `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, {SDL_AUDIO_F32, 2, 44100}, callback)`, resumed once the game is running. The callback renders exactly the frames SDL asks for and reports a late callback when the time since the previous one exceeds 1.5 × the duration of the audio it delivered. No audio device (CI, `SDL_AUDIO_DRIVER=dummy`, `--no-audio`) is not an error: the game runs and is silent.
- **Deterministic capture:** `MapleThief --audio-capture FILE.wav` writes the mixed output as a 16-bit stereo WAV instead of (or besides) playing it. Under the virtual clock (`--fixed-step MS`, ADR 0044) the platform renders the audio frames each step covers (44,100 × MS / 1,000, with the remainder carried, so 735 per 16.667 ms on average) into the capture after each run-loop turn, so a scripted run gives the same file every time and on every OS. Tests read captures; they never need a device.
- **The silent state rules stay** (ADR 0038): handles, names, `alGetError`'s first-error rule, `AL_INVALID_OPERATION` without a current context, and the game's own `alSourcePlay(-1)` from `ResumeBGM` before any music. `alcSuspendContext` / `alcMakeContextCurrent(NULL)` pause effects mixing for that context; `alcProcessContext` resumes it.

## Consequences

- **#61** writes the mixer, the effect voices, `alBufferData`'s copy, the pull API, the platform stream and `--audio-capture`, plus a synthetic tone test (generated PCM through `alBufferData` → `alSourcePlay` → `mt_audio_render`, compared sample-exactly), and marks this ADR Accepted.
- **#62** fills `ExtAudioFileRead` with real samples (the WAV path already parses headers); effects then sound.
- **#63** adds the music voice and dr_mp3 streaming behind the existing `AVAudioPlayer` state.
- **#64** stores master/music/effects volume and mute-on-unfocus in `settings.ini` (ADR 0043) and applies them through the buses; the vibrate system sound goes to a rumble hook for #72.
- **CI** checks the mixer exactly with synthetic data, and real-asset tests check the decoded files and a scripted capture locally. Listening is a manual check (`needs-human` where an issue asks for it).
- **How we'd know this was wrong:**
  - **Dropouts in the soak test** (late callbacks, a long render): move music decoding out of the lock into a ring buffer, or raise the device buffer.
  - **Linear interpolation sounds wrong** on the 8 kHz effects next to device recordings: swap in a better interpolator behind the same voice step; the tests' expected samples change with it.
  - **A device's own rate makes SDL's conversion audible**: render at the device rate instead (the mixer's rate becomes a parameter; captures stay at 44,100).
