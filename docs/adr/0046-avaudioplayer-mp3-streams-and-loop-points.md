# 46. AVAudioPlayer: dr_mp3 streams on Apple's decoder timeline, looped whole

- **Status:** Accepted
- **Date:** 2026-10-29
- **Issue:** #63
- **Builds on:** [ADR 0003](0003-dependencies.md) (dr_mp3 0.7.3), [ADR 0038](0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md) (the silent `AVAudioPlayer`'s selectors and state), [ADR 0045](0045-audio-mixer-and-output.md) (music voices pulling a `mix::MusicStream`)

## Context

The game's music is 18 MP3s, one `AVAudioPlayer` per track ([re/audio.md](../re/audio.md)): `CMSoundPlayer::LoadBGM` sends `initWithContentsOfURL:error:`, `setVolume:` 0.7 (0 while music is off), `setNumberOfLoops:-1` and `prepareToPlay`; `Play` sends `isPlaying`/`stop`, `setCurrentTime:0` unless it resumes, `setVolume:` and `play`; `StopAllBGM` stops every player that plays; `MusicOff`/`MusicOn` set the volume of every player to 0/0.7. Those are all the selectors ([re/host-surface.md](../re/host-surface.md)): no `pause`, `currentTime`, `duration` or delegate. The players are never released.

ADR 0045 left #63 the stream: what a pass of a track is, where it loops, and what the player's messages do to it. The files have **no Xing/Info, VBRI or LAME frame**, so nothing says how many samples at the start (encoder and decoder delay) and at the end (padding) are not music. ADR 0003 expected "a fixed encoder delay or measured loop points". The device's behaviour is what we match: the player looped whatever Apple's decoder gave, padding and all.

Measured on this Mac (macOS AudioToolbox, the codec family AVAudioPlayer plays through; scratch tools, numbers in Evidence):

- `AudioFile` has **no packet table** for these files (`kAudioFilePropertyPacketTableInfo` fails), so Apple knows no more than we do; its duration is MPEG frames × samples per frame (`31.mp3`: 2,371 × 1,152 / 44,100 = 61.936 s).
- **ExtAudioFile's PCM has exactly that many frames, and is dr_mp3's output shifted by 529 frames**: Apple frame *i* = dr_mp3 frame *i* + 529 (within 2 LSB), and Apple's last 529 frames are exact zeros. 529 is the MP3 decoder's delay (528 + 1, as LAME counts it); Apple's converter drops it and keeps the length. The same holds for all 18 files, MPEG-1 and MPEG-2 alike, and for our synthetic LAME files.
- dr_mp3 without an info frame trims nothing: it gives MPEG frames × samples per frame, the decoder delay included.

## Options considered

1. **Loop dr_mp3's raw output** (frames 0 … N − 1): the same length as the device, but every pass starts 529 frames early (the decoder's warm-up instead of the music) and the timeline is 529 frames off Apple's, so `setCurrentTime:` lands elsewhere.
2. **Trim to the music** (guess the encoder delay, e.g. LAME's 576 + 529, and the padding from the length): gapless, but a guess per file with no data, and not what the device did.
3. **Apple's timeline:** a pass is N frames, dr_mp3's frames 529 … N − 1 followed by 529 frames of silence; loop the whole pass.

## Decision

- **Option 3.** `mt_audio::Mp3Stream` (`runtime/src/audio/mp3_stream.{h,cpp}`) is a `mix::MusicStream` with `length()` = MPEG frames × samples per frame and `kPriming` = 529. Frame *p* of a pass is dr_mp3 frame *p* + 529 for *p* < N − 529, else 0. At the end of a pass the stream seeks dr_mp3 back to the start (a fresh decoder, as a new pass of the device's player would be) and goes on, inside `read()`, if loops remain; otherwise it has ended. The encoder's delay and padding stay in the loop, as on the device.
- **Decoding** is streamed: the file's bytes (under 1 MB each) are read into host memory at `initWithContentsOfURL:error:` and dr_mp3 decodes from them as the mixer pulls blocks of up to 4,096 frames, under the mixer's lock (ADR 0045). A seek is dr_mp3's: back to the start and decode forward (the game only ever seeks to 0, which is one MP3 frame).
- **Determinism:** `mp3_stream.cpp` is the one file that defines `DR_MP3_IMPLEMENTATION`, with `DR_MP3_NO_SIMD` and without floating-point contraction (`maplethief_strict_fp`). minimp3's scalar path calls no libm, so the decoded samples are the same bytes on every host, and so is the mix (ADR 0045). CI pins a hash of the synthetic fixtures' decode.
- **The player** (`runtime/src/audio/avfoundation.cpp`), keyed by its guest address as the music voice:
  - `initWithContentsOfURL:error:` reads the file and opens the stream; **nil** if it can't be read or holds no MP3 audio, as AVFoundation fails. `*outError` is not written (the game passes nil).
  - `play` starts or resumes the voice from the stream's position at the player's volume; after a natural end (finite loops) it rewinds first, as AVAudioPlayer resets `currentTime` when it finishes. `stop` stops the voice and keeps the exact frame (the voice keeps what it had buffered, ADR 0045). `isPlaying` is YES from `play` until `stop` or until the voice has played out the last pass (the mixer's `music_playing`).
  - `setVolume:` clamps to [0, 1] (NaN is 0) and sets the voice's volume at once; music off is volume 0 and keeps playing, as on the device. `setNumberOfLoops:` *n* < 0 loops for ever, *n* ≥ 0 plays *n* more passes; it may change during play (atomic). `setCurrentTime:` *t* seeks to frame ⌊*t* × rate⌋ of the pass, clamped to [0, N], through `mix::music_edit` (dropping the voice's buffer) or directly when no voice has the stream yet. `prepareToPlay` answers YES and does nothing more (the stream is open from `init`).
  - Dealloc removes the voice (`music_remove`) and the stream. A new process forgets every player's stream, as the mixer forgets their voices.
  - No delegate is called (the game sets none).
- `AVAudioSession` stays the stub of ADR 0038 (#64 owns the AudioSession C API).

## Evidence

- **CI (synthetic):** `tests/cpp/mp3_test.cpp` (in `objc_test`, strict FP) on `tests/cpp/data/audio/tone44.mp3` and `tone22.mp3`, 0.3 s stereo tones (440 Hz left, 660 Hz right) that `make_tones.py` there encoded with LAME 4.0 as the game's two batches were made (CBR joint stereo, CRC, ID3v2 + ID3v1, no LAME tag): rate, channels and length (13 × 1,152 and 14 × 576, mtport.audio's frame walk); the pinned FNV-1a of dr_mp3's decode; a pass = dr_mp3 from frame 529 plus 529 zeros; the tone's zero crossings; numberOfLoops 0, 1, 2 and −1 across blocks of 1,000 frames, changed during play; seek, seek past the end, restart; bytes that hold no MP3; and the player through the runtime and the mixer: LoadBGM and Play (resume 0 and 1) sample for sample at 0.7, past the loop point, `stop` keeping the exact frame, music off and on, `setCurrentTime:` 0.1 s and a negative time, the music bus, a volume above 1, 22,050 Hz resampled across two loop points exactly, a player with numberOfLoops 0 that finishes (and plays again from the start, or plays out its rest after a `stop` near the end), two players switched as maps do, release removing the voice.
- **Local (real assets):** CTest `audio.real_mp3` (`tests/cpp/mp3_real_test.cpp`): all 18 MP3s open stereo at 44,100 or 22,050 Hz with a pass of exactly their MPEG frames × samples per frame (the same 18 numbers as `mtport.audio`'s `read_mp3`, e.g. 31.mp3: 2,371 frames, 2,731,392 samples), stream to their end and end, end with 529 zero frames, and loop past their end onto their first frames; 31.mp3 through the mixer across its loop point, 60,000 frames exact; on macOS, every file and both fixtures equal ExtAudioFile's decode in length and within **2 LSB** at frame 0. All 807.9 s of music decode in 1.5 s in a Debug build (534 × real time; a 4,096-frame block is about 0.2 ms under the lock). `platform.audio_capture` (two scripted fixed-step runs) still writes byte-identical WAVs, now with track 31 in them.
- **Manual:** the game on the Mac mini's speakers (`MapleThief --frames 900`, SDL dummy video): `audio: driver coreaudio, device "Mac mini Speakers": 44100 Hz, 2 channels, format 0x8120, 1024-frame buffer`; the trace shows LoadBGM's `setVolume:0.7`, `setNumberOfLoops:-1`, then Play's `setCurrentTime:0`, `setVolume:0.7`, `play` on `31.mp3` (2,731,392 frames); 5,199,872 frames rendered in 5,078 calls over 118 s (track 31 played and looped), longest render 325 µs, 0 late callbacks. Listening is Joris's ([#124](https://github.com/armaatus/maplethief-port/issues/124)).

## Consequences

- The music sounds and loops like the device's, including the short silence (encoder padding, the decoder tail) at each loop point that the original had. The port's players match Apple's timeline to the frame, so a later reference recording lines up without an offset.
- `kPriming` = 529 is a measured constant of Apple's decoder, not of the files; a different MP3 would get the same treatment.
- **How we'd know this was wrong:** a recording of the device (#124) whose loop point or start differs from ours by more than a few ms would mean iPhone OS 3/4's AudioQueue treated priming differently from today's macOS ExtAudioFile; `kPriming` (or the silent tail) then changes, and the expected passes in the tests with it. If a decode ever differs between hosts (the Windows MSVC build is not checked locally), the pinned hash in `mp3_test.cpp` fails in CI.

## Addendum: the loop points through the game (#124, 2026-10-30)

*Consequences* above says each loop point has "the short silence (encoder padding, the decoder tail)". Measured through the game's own `CMSoundPlayer::Play` (CTest `audio.game_sounds`, `tests/cpp/game_audio_real_test.cpp`), the silence is mostly the files' own: 13 of the 18 tracks end in 0.25–3.3 s of digital silence that is in the MP3 (Apple's ExtAudioFile decode has the same zero frames, within one MPEG frame), so the device's `AVAudioPlayer` paused as long at each loop. Track 31, the main menu's, has 140,985 zero frames at its end (the 529-frame tail included) and 5,466 at its start: **3.32 s**. Only `0`, `2`, `30`, `34` and `38` loop within 32–120 ms. For every track the test checks that the silence across the loop point is the pass's own trailing silence (at least the 529 tail frames) plus its own leading silence, equal to Apple's zeros within one MPEG frame, and that the second pass's first second equals the first pass's sample for sample, so the port adds nothing at the loop. The decision is unchanged: trimming that silence would be a change to the game's music, not a fix (a later option for #124's listening check or a setting, not a default). That option is [ADR 0062](0062-optional-gapless-music-loops.md) (#125): `gapless_music`, off by default.
