# 62. Optional gapless music loops: trim the files' digital silence, on a trimmed timeline

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #125
- **Builds on:** [ADR 0046](0046-avaudioplayer-mp3-streams-and-loop-points.md) (`Mp3Stream` on Apple's decoder timeline, looped whole; its #124 addendum measured the silence), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`settings.ini` and its audio keys)

## Context

#124 found that 13 of the game's 18 MP3s end in 0.25–3.3 s of digital silence that is in the files themselves; Apple's decoder gives the same zero frames. So the device's `AVAudioPlayer` paused at every loop. The worst is the main menu's track 31, with 3.32 s per loop. ADR 0046 reproduces this on purpose, and that stays the default. #125 asks for an *option* that skips the silence so the music loops without a pause. Questions:

- What counts as silence: exact zeros, or a small threshold?
- Where pass frame 0 is with the option on: the device's timeline, or the trimmed one. This matters for `setCurrentTime:` and for the position `stop` keeps.
- How the option gets from the settings to the players, and what it costs.

Measured (CTest `audio.real_mp3`, every shipped file, dr_mp3 on Apple's timeline):

- The leading silence is 0–6,744 frames and the trailing silence 529–141,077 frames (the tail of ADR 0046 included). Both are runs of exact zeros, and they match Apple's own decode within one MPEG frame (`audio.game_gapless_music`).
- A 1-LSB threshold (|sample| ≤ 1) would cut a little more from most files: 0–302 frames at either end. From `5.mp3` it would cut **109,554 frames (2.5 s)** of quiet ±1 LSB signal at the end. That is the end of a fade, not silence.

## Options considered

1. **Silence = exact zeros** (every channel of a frame is 0 in dr_mp3's 16-bit output), against **a threshold** (e.g. 1 LSB). A threshold cuts into real, if very quiet, music (`5.mp3`'s 2.5 s fade-out tail) and makes the result depend on a tuning value. Exact zeros are what the files' silence is (digital silence from the encoder's input), and it is decoder-independent enough that Apple's decode agrees.
2. **Timeline: the device's** (positions stay Apple frames; only the loop skips from the last sound to the first). `setCurrentTime:0`, which the game sends on every restart, would then still play the leading silence once (up to 153 ms). Also, a stream would have two lengths (where it loops and how far `seek` goes).
3. **Timeline: the trimmed pass.** The pass is the frames from the first sound to the last; `length()`, `position()`, `seek()`, `setCurrentTime:` and the loop all work on it. The game only ever seeks to 0 (`setCurrentTime:0`) and resumes from where `stop` left off. Both mean the same thing on the trimmed pass: the start of the music, and the exact frame.
4. **How it reaches the players:** a per-player property, against a process-wide switch read when a player opens its file. The game makes all 18 players in `CMSoundPlayer::LoadBGM` at start-up and never again, so a switch set before the boot covers them all.

## Decision

- **Exact zeros, option 1.** `Mp3Stream::open(bytes, gapless)` with `gapless` on decodes the pass once at open (from Apple frame 0, exactly as `read()` would), and finds the first and last frames with any sample ≠ 0. The 529 tail frames are always among the trailing zeros. A pass that is silent throughout is kept whole (no zero-length pass).
- **The trimmed timeline, option 3.** Pass frame *p* is Apple frame `trim_start()` + *p*, which is dr_mp3 frame `trim_start()` + *p* + 529. `length()` is last − first + 1, and `full_length()` keeps Apple's pass. Every frame of the trimmed pass is decoded, and the loop is still a seek of dr_mp3 back to the start (ADR 0046). Now it decodes forward to `trim_start()` + 529, at most 7,273 frames (about 6 MPEG frames).
- **The switch, option 4.** `mt_audio_set_gapless_music(bool)` / `mt_audio_get_gapless_music()` (`maplethief/audio.h`) is atomic and kept across processes. `-[AVAudioPlayer initWithContentsOfURL:error:]` reads it, so a player keeps what it opened with. With it on, the trace line of the init adds `, gapless: frames A to B of N (...)`; with it off the line is unchanged.
- **The setting:** `gapless_music = on|off` in `settings.ini` (ADR 0043's on/off spellings; written `on`/`off`), default **off**. The flag `--gapless-music on|off` is checked and applied like the other audio flags, in every mode. `apply_audio_settings` sets the switch before the game boots. The start-up log line shows `gapless music on|off`.
- **Off is the device**, byte for byte: `trim_start()` is 0, the length and every decoded frame are unchanged, and nothing extra is decoded.

## Evidence

- **CI (synthetic):** `tests/cpp/data/audio/gap44.mp3` (0.1 s silence, 0.2 s of the tone, 0.25 s silence) and `silence44.mp3` (0.1 s of silence only), both from `make_tones.py` with LAME, as the other fixtures. `tests/cpp/mp3_test.cpp` checks:
  - off (the default and `false`): Apple's whole pass, as before;
  - on: exactly the frames from the first to the last non-zero frame, starting and ending on a sound; the tone fixtures trim too;
  - a silent file stays whole;
  - numberOfLoops 0/1/2/−1 across blocks of 1,000 frames, with no silent frame on either side of a loop point;
  - seeks, a seek past the end and `restart` on the trimmed pass;
  - the player through the runtime and the mixer: sample for sample over two passes, `stop` and resume at the exact frame, `setCurrentTime:0.1` = trimmed frame 4,410, and Play(resume 0) from the first sound. A player opened before the switch was set keeps the whole pass.

  `tests/cpp/scaling_test.cpp` covers the key (spellings, bad values, the new file's comment and line, round trip, appended to an older file). `tests/cpp/audio_out_test.cpp` checks that `apply_audio_settings` sets and clears the switch.
- **Local (real assets):** CTest `audio.game_gapless_music` (`game_audio_real_test gapless`) boots the recompiled game with the switch on and plays each of the 18 tracks through `CMSoundPlayer::Play`:
  - the silent run across every loop point is **0 frames** (40.mp3: 1 frame, an interpolated 22,050 Hz edge), against one MPEG frame allowed (1,152 output frames);
  - the second pass's first second is identical to the first;
  - the frames dropped are Apple's zero frames at both ends within one MPEG frame (31.mp3: 5,466 + 141,077 dropped, Apple 5,466 + 140,985).

  CTest `audio.real_mp3`: each file gapless is the untrimmed pass's frames first..last exactly (the per-file table and the 1-LSB comparison above). Opening all 18 gapless (one decode each) takes 2.8 s in a Debug build on a loaded Mac mini M4: a one-off at the game's start-up, only with the option on. With the option off, the existing CTests pass unchanged (the pinned decode hash, `audio.game_sounds`, `audio.game_map_music`, `platform.audio_capture`). Two fixed-step `--audio-capture` runs (Debug, `--fixed-step 16.667`) give the same SHA-256 from the base commit (`fleet-ready-now`) and from this branch, both by default and with `--gapless-music off`:
  - `--frames 4200`: 207.35 s of the main menu, `d7b3fe85…`. It has track 31's loop point three times, each a 146,543-frame (3.32 s) silent run.
  - `--frames 180 --tap 3,106,165`: `platform.audio_capture`'s run, `003a566f…`.

  With `--gapless-music on`, the same menu capture has no silent run longer than 62 frames (1.4 ms) anywhere after the first sound.

## Consequences

- With the option on, the music loops with no pause. This changes the game's music from what the device played; it is an enhancement (M10), off unless the player turns it on. Track 31's pass gets 3.32 s shorter, so the menu music repeats sooner.
- A resumed track continues on the trimmed pass. That is the same music position as on the device's timeline, shifted by `trim_start()`, so `resume` still means "from where it stopped".
- Changing the setting takes effect at the next start (the players are made at boot). A settings menu (#89) that toggles it live would have to reopen the players or say "after a restart".
- **How we'd know this was wrong:** a listener hears a click at a loop point. That would mean the last and first sounds don't join smoothly; it is unlikely, since both are next to digital silence in the file. The fix would be a short crossfade at the loop, a mixer change. Or a track whose last sound is real music cut short. Then the file has no silence to trim, and the option changes nothing for it.
