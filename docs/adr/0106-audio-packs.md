# 106. Audio packs: the game's music and effects replaced from a folder

- **Status:** Accepted
- **Date:** 2026-11-05
- **Issue:** #188
- **Builds on:** [ADR 0003](0003-dependencies.md) (dependencies: dr_flac joins dr_mp3), [ADR 0045](0045-audio-mixer-and-output.md) (the mixer, ExtAudioFile), [ADR 0046](0046-avaudioplayer-mp3-streams-and-loop-points.md) (the music streams), [ADR 0062](0062-optional-gapless-music-loops.md) (gapless loops), [ADR 0070](0070-settings-registry-and-presets.md) (the registry), [ADR 0081](0081-texture-packs-and-nearest-texture-filter.md) (texture packs: the pattern)

## Context

#188 asks for audio packs: a folder of the player's own files in place of the game's 18 MP3 tracks and 26 WAV effects, for remastered or higher-rate music (seven tracks are 22.05 kHz, 80 kbit/s rips) and fan sound packs, without touching the game's files.

- **Two doors.** The game opens every sound through one of two calls (`docs/re/audio.md`): `ExtAudioFileOpenURL` for the effects (Apple's `MyGetOpenALAudioData` at start-up: the file format, a 16-bit client format at the file's own rate and channels, one read of the whole file, then `alBufferData` at that rate), and `-[AVAudioPlayer initWithContentsOfURL:error:]` for each track (a stream per player, ADR 0046). Both get the bundle path `sound/<id>.<ext>`.
- **The mixer resamples any rate.** Effect voices and music voices step through their source at `rate / 44,100` (ADR 0045), so a replacement at any rate and 1 or 2 channels plays without a converter.
- **Apple's MP3 timeline is the game's files'.** ADR 0046 drops 529 decoder frames and adds 529 silent ones because AVAudioPlayer did so for files without a LAME/Xing header. A player's file usually has such a header, which dr_mp3 already honours (it trims the encoder's delay and padding).
- **Pack files are untrusted**: downloaded, or made with tools we don't control (ADR 0081's rule).
- **Loops.** The game loops every track whole (`numberOfLoops = -1`). A remaster often wants an intro and a loop section.

## Options considered

**Where the pack plugs in.**

1. **At the two doors, by the bundle file's name.** The pack is asked first; with no file (or a rejected one) the door opens the game's file as before. Chosen: the game, the mixer and the capture path see nothing new without a pack.
2. A virtual file system under the bundle. It would also need to fake WAV/MP3 containers for files of another format, and the game would decode an MP3 in place of a WAV it reads as PCM.

**The timeline of a pack's music.**

1. **The file's own samples: the decoder's frames from the first.** Chosen. Sample for sample what the player put in, gapless where their encoder said so.
2. ADR 0046's Apple timeline. It would cut the first 529 frames of a file that has a LAME header (dr_mp3 already trims it) and add a silence nobody asked for.

**Loop points.**

1. **`<id>.loop` (text: start and end frame, end excluded), else a WAV's first `smpl` loop.** The first pass plays 0 to end, each repeat start to end. A text file works for every format and every editor; `smpl` is the WAV tag that sample editors write. Chosen. MP3 and FLAC have no standard loop tag (Vorbis-comment `LOOPSTART` is a convention of some games); a `.loop` file covers them.
2. ID3 or Vorbis-comment tags only. Not standard, and a player could not set them without a tag editor.

Without a loop, ADR 0062 applies: `gapless_music` trims the file's exact-zero ends as it does the game's.

**Formats: an ADR 0003 dependency review.**

| Candidate | Licence | Fit | Untrusted input | Decision |
|---|---|---|---|---|
| WAV (our RIFF walk, now `wav.cpp`, shared with ExtAudioFile) | ours | 8/16/24/32-bit PCM | bounds-checked since #62 | **yes** |
| MP3, dr_mp3 0.7.3 | Unlicense / MIT-0, already pinned | the decoder the game's tracks use | fuzzed upstream, already trusted with the bundle | **yes** |
| FLAC, dr_flac 0.13.3, the same dr_libs commit as dr_mp3 | Unlicense / MIT-0 | a single header from the same author and commit; lossless, so remasters need not be WAVs in memory | maintained with dr_mp3, its CRC and bounds checks; our mutation test (below) | **yes**, pinned by SHA-256 like dr_mp3 |
| Ogg Vorbis, stb_vorbis (stb at our pinned commit) | MIT / Unlicense | a single file we already fetch from | a long list of memory-safety bugs found by fuzzing malformed files (CVE-2019-13217 to -13223, and further reports since) | **no**: untrusted downloads are exactly its weak spot |

So packs take WAV, MP3 and FLAC. A Vorbis decoder can follow behind the same codec switch if a safer one is pinned.

**Limits.** At most 128 MiB a file (read only after its size is checked), 1 or 2 channels (OpenAL's formats, the mixer's voices), 8,000 to 192,000 Hz, an effect at most 60 s and a track at most 30 min (counted before anything is decoded: a WAV from its data size, an MP3 from its frame walk, a FLAC from its STREAMINFO; a FLAC that does not state its length is refused). An effect is decoded whole because the game reads it whole into guest memory; 60 s at 192 kHz stereo is 46 MB, inside the guest heap's 480 MB and ADR 0045's 64 MiB buffer limit.

## Decision

- **Runtime** (`maplethief/audio.h`, `runtime/src/audio/audio_pack.{h,cpp}`): `mt_audio_pack_open(DIR)` indexes the folder's `.wav`, `.mp3`, `.flac` and `.loop` names (any case); `mt_audio_pack_close`; `mt_audio_pack_get_stats` (effects and tracks asked and replaced, files rejected). The pack's file for the bundle's `<id>.<ext>` is `<id>.<ext>`, else `<id>.wav`, `.mp3`, `.flac`, whatever the role.
  - **Effects**: `ExtAudioFileOpenURL` asks `pack::effect`; a pack file is decoded whole to 16-bit PCM and becomes the opened file (16-bit, its rate and channels), which the game reads with its own client format.
  - **Music**: `-initWithContentsOfURL:error:` asks `pack::music`; a pack file becomes the player's stream (`Mp3Stream::open_pack` for MP3 and FLAC, `open_pcm` for a WAV's samples) on its own timeline, with its loop. `-play`, `-stop`, `-setCurrentTime:`, the volume and `numberOfLoops` behave as for the game's tracks.
  - A rejected file is logged once (`audiopack: music DIR/31.mp3 holds no MP3 audio ...: rejected, the game's 31.mp3 kept`) and never read again; the game's file plays. A bad `.loop` is logged and ignored.
  - The trace (`MAPLETHIEF_TRACE=audio`) adds `, from the audio pack: FILE` (and the loop) to the open lines; without a pack it is unchanged.
- **dr_flac** 0.13.3 is pinned beside dr_mp3 (`maplethief_use_dr_flac`, THIRD_PARTY.md), implemented in `mp3_stream.cpp` with `DR_FLAC_NO_OGG` (one container less to parse), `DR_FLAC_NO_SIMD` and `DR_FLAC_NO_CRC` (a damaged frame decodes as it is, bounds-checked, as a damaged MP3 frame does).
- **Setting** `audio_pack = DIR` (`--audio-pack DIR`; Audio, not in the presets: a path; from the next start). The frontend opens it before the boot, beside `texture_pack`, and logs the counts at the end: `audiopack: pack: 26 effects, 1 replaced; 2 tracks, 1 replaced; 0 rejected`.
- **`python3 -m mtport.audiopack init DIR [--bundle APP]`** writes `DIR/audiopack.txt`: the 44 names with their role (BGM or SFX, `Sound_IsBGM`'s list), length in seconds and frames, format, and the ID3 title where the file has one. It copies no sound.
- The repository ships no pack. The tests' sounds are generated (C++ and Python tones written at run time) or our own synthetic tones (`tests/cpp/data/audio/`, `make_tones.py`).

## Evidence

- **CI (synthetic)**, `tests/cpp/audio_pack_test.cpp` (`objc_test`):
  - `audio.pack: a generated tone in place of an effect plays sample for sample`: a 22.05 kHz stereo tone named `12.WAV` replaces an 8 kHz mono `12.wav`; the game's ExtAudioFile calls read the tone, `alBufferData` and the mixer give ADR 0045's interpolation of it in every frame; a name without a file and a closed pack give the game's file.
  - `... an MP3 in place of an effect ...`: `tone22.mp3` as `20.mp3` for `20.wav`: every dr_mp3 frame, none dropped.
  - `... a generated tone in place of a track plays sample for sample, looped`, `... an MP3 in place of a track ...`, `... a FLAC in place of a track or an effect decodes losslessly`: through AVAudioPlayer and the mixer, looped whole, `stop`/`play` on from the frame; the FLACs' decode equals the triangles they were made from, 16-bit stereo and 24-bit mono (its top 16 bits).
  - `... loop points from a .loop file or a WAV's smpl chunk`: 0..3000 then 1000..3000 again; smpl 500..1499 inclusive; `.loop` over smpl; an MP3 by its `.loop`; a reversed `.loop` and one past the end logged and ignored.
  - `... gapless_music trims a pack file without a loop as the game's`.
  - `... corrupt, oversize and out-of-limit files fall back to the game's, logged once`: random bytes, a cut fmt chunk, 3 channels, 4,000 Hz, 60 s + 1 frame, empty, an MP3 of random bytes and one cut after 3 bytes, a FLAC of random bytes, a WAV without samples, and a sparse 128 MiB + 1 file (never read): each opened twice, the game's samples both times, one log line each; a corrupt track plays the game's MP3.
  - `... random mutations of a WAV, an MP3 and a FLAC never read out of bounds`: 450 files cut anywhere or with 8 random bytes changed, each through `pack::effect` and a `pack::music` stream read through its loop from its middle.
  - `platform.presets`: the row's columns.
  - `tests/test_audiopack.py`: the list on a synthetic bundle (roles, frames, seconds, formats, the title), the player's files kept, a missing or wrong bundle refused; locally, the real bundle's 44 names.
- **Local, real assets**: `platform.audio_pack` (four `--audio-capture` runs of `platform.audio_capture`'s 4.7 s, tap on Play at 3 s): no pack, a pack whose only file replaces `0.mp3` (which this run never plays) and a pack whose `31.mp3` is text give the same capture byte for byte (SHA-256 `003a566f...`, also the capture of the epic's tip before #188); a pack whose `31.wav` is a generated tone changes it, and from the menu track's first frame (11,025) the capture is the tone at the game's volume 0.7 within 1 LSB for 1 s. `audio.real_wavs`, `audio.real_mp3`, `audio.game_sounds`, `audio.game_map_music`, `audio.game_gapless_music` and `platform.audio_capture` pass unchanged.

## Consequences

- A player can swap the 22 kHz rips for full-rate remasters with intro-and-loop points, or swap single effects, without touching the bundle; any mix of WAV, MP3 and FLAC.
- The title screen plays no music (`docs/re/audio.md`): the music heard before Play is the main menu's `31.mp3`, and `0.mp3` is the Henesys ending's, so a pack for the title's music replaces `31`.
- A track's WAV sits in memory as 16-bit PCM for the player's life (the game keeps every player it made); a 10-minute stereo 44.1 kHz WAV is 106 MB. MP3 and FLAC stream from their file's bytes, so they are the better choice for long tracks (`docs/play/settings.md` says so).
- An effect decodes when the game starts (all 26 at once): a pack of long FLAC effects adds their decode time to the start-up.
- How we'd know this is wrong: a pack file that plays with a gap, a click or the wrong speed at its loop (the loop or the resampler), a capture without a pack that differs from one before #188, or a crash or hang on a file from the wild (the limits and the decoders' checks).

## Addendum (#413)

ADR 0154's threat model found that an MP3 or FLAC effect was decoded whole before its rate and channels were checked, against the limits above. `pack::effect` now opens the stream (`Mp3Stream::open_pack`: the header, the MP3's frame walk, the FLAC's STREAMINFO), checks its rate and channels, and only then decodes it whole (`decode_whole`, which replaces `decode_file`). A pack indexes at most 65,536 sound and `.loop` files (`MT_AUDIO_PACK_MAX_FILES`); the rest are skipped, in one log line. A symbolic link in the pack is skipped and never followed, in one log line. Tests: `audio.pack: an MP3 or FLAC effect's rate and channels are checked before it is decoded (#413)`, `audio.pack: a symbolic link in a pack is not followed, logged once (#413)`, `audio.pack: a pack indexes at most MT_AUDIO_PACK_MAX_FILES files, the rest skipped and logged once (#413)`; the limits that had none: `audio.pack: up to 192 kHz; a track up to 1,800 s; a FLAC without its length is refused (#413)`, `audio.pack: a .loop file is at most 256 bytes (#413)`.
