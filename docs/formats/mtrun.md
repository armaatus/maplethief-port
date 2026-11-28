# Run files (`.mtrun`)

A run file is a session of `MapleThief` played in real time, kept so that it plays back exactly ([#405](https://github.com/armaatus/maplethief-port/issues/405), [ADR 0156](../adr/0156-speedrun-kit-splits-clean-runs-and-verifiable-replays.md) and its addendum). It is our format, not the game's. It holds no game content: the clock values the guest read, the input the port gave it, the player's settings and the player's own save files.

- **Written by**: `record_runs = on` (Other, off by default): `<data dir>/runs/<UTC YYYYMMDD-HHMMSS>.mtrun` at the end of every session (`platform/src/run_recorder.cpp`).
- **Read by**: `MapleThief --play-run FILE` and `MapleThief --verify-run FILE` (`platform/src/main.cpp`, `platform/src/run_verify.h`, [#408](https://github.com/armaatus/maplethief-port/issues/408); the player's page is [Speedrun timer](../play/speedrun.md#recording-and-verifying-a-run)).
- **Parser and writer**: `platform/src/run_file.{h,cpp}` (`read_run_file`, `write_run_file`). The tape it carries is `runtime/include/maplethief/runtape.h`'s.
- **Validation**: synthetic files in CI (`tests/cpp/run_file_test.cpp`: the round trip, each malformed header and body field refused with its byte, every byte of a good body flipped, every cut, random bodies; `tests/cpp/run_verify_test.cpp`: the verifier's refusals and report); locally, recorded sessions replayed with equal checkpoints (ADR 0156's addendum). There is no shipped file to parse: the IPA has none.

## What it is bound to, and what it holds

| Bound to | Where |
|---|---|
| the armv7 slice's SHA-256 | `BIND` |
| the port's version (`MT_VERSION`) | `BIND` |
| the format version | the header |
| the settings at the start, every change at its turn | `SETS`, `CHGS` |
| the game's files the guest reads at boot | `FILE` |

| Holds | Where |
|---|---|
| every value the guest's clock hooks returned, run-length coded | `TAPE` |
| the host's calls at the guest's boundary, at their run-loop turn | `EVTS` |
| the run categories the session timed, and the splits the live timer reached ([#408](https://github.com/armaatus/maplethief-port/issues/408)) | `SPLT` |
| checkpoints: every 1,000 game ticks, at every split's turn, and the last whole turn when the session ended on one; at most one a turn | `CHKS` |
| why the recording ended, and the turns it covers | `END_` |

A replay is valid on the binary and port version that recorded it: `--play-run` and `--verify-run` refuse another. `--verify-run` also refuses a file with a checkpoint missing (one every 1,000 game ticks up to the last checkpoint's tick, and one at every split's turn) or with checkpoints out of turn order, before it plays.

## Layout

All numbers are little-endian. *var* is an unsigned LEB128 number (7 bits a byte, low first, at most 10 bytes and 64 bits); *svar* is a signed one, zig-zag coded (`(v << 1) ^ (v >> 63)`) as a *var*. A *string* is a *var* length and that many bytes (UTF-8).

### Header (36 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `4D 54 52 55 4E 00 0D 0A` (`MTRUN\0\r\n`) |
| 8 | 4 | format, `1` |
| 12 | 4 | flags, `0` |
| 16 | 8 | the body's length, inflated (at most 512 MiB) |
| 24 | 8 | the packed body's length: exactly the rest of the file |
| 32 | 4 | CRC-32 (IEEE) of the packed body |
| 36 | … | the body, deflated as a zlib stream (RFC 1950) |

### Body

Nine sections, each `char tag[4]`, `u32 length` and `length` bytes, in this order and no other. A section's fields must use its bytes exactly.

| Tag | Fields |
|---|---|
| `BIND` | 32 bytes SHA-256; *string* port version (≤ 256 bytes) |
| `SETS` | *string* the settings as `settings.ini` writes them (`serialise_settings`, ≤ 1 MiB) |
| `CHGS` | *var* n; n × (*var* turn − the previous change's turn, *string* key ≤ 64, *string* value ≤ 4,096) |
| `FILE` | *var* n (≤ 16); n × (*string* name ≤ 64, a plain file name; *var* size ≤ 4 MiB; the bytes) |
| `TAPE` | three streams, `clock`, `gcd`, `music`, each: *var* n; n × (*svar* value − the previous run's value, *var* count ≥ 1) |
| `EVTS` | *var* n; n × (*var* turn − the previous event's turn, `u8` kind, the kind's fields) |
| `SPLT` | *var* m (≤ 16); m × (*string* category id ≤ 64, a plain name; *string* the category file's text ≤ 16 KiB); *var* n; n × (*string* name ≤ 256, *var* turn, *var* real time µs, *var* game ticks) |
| `CHKS` | *var* n; n × (*var* turn, *var* game ticks, `u64` guest memory digest, *string* state line ≤ 1,024) |
| `END_` | *var* end (0 quit, 1 refused, 2 failed); *string* detail ≤ 1,024; *var* the turns recorded |

The streams (`runtape.h`): `clock` is `mt_libc_config.clock`'s Unix microseconds (gettimeofday, CFAbsoluteTimeGetCurrent, UIKit's clock), `gcd` is `mt_gcd_config.clock`'s nanoseconds, `music` is `-[AVAudioPlayer isPlaying]`'s 0 or 1 (a player plays out on the audio thread, in real time). A value can step back (a negative delta): the guest's clock is whatever the platform's was.

Event kinds and their fields:

| Kind | Name | Fields | The call it replays |
|---|---|---|---|
| 1 | press | *var* action | `mt_input_press` |
| 2 | release | *var* action | `mt_input_release` |
| 3 | release all | none | `mt_input_release_all` |
| 4 | touch | *var* finger, `u8` phase (0–3), `f32` x, `f32` y (screen points, the bits as given) | `mt_uikit_touch` |
| 5 | lifecycle | *var* event | `mt_uikit_lifecycle_event` |
| 6 | fire tolerance | `f64` seconds | `mt_runloop_set_fire_tolerance` |
| 7 | memory write | `u32` guest address, `u8` length (1–8), the bytes | `mt_runtape_poke` |

Actions are numbered as `mt_input_action` in the version that recorded the file, which is why a file plays only on that version.

The turn is the run loop's: UIApplicationMain's loop counts them from 1, 0 is before the first. A replay delivers a turn's events after that turn's run-loop work and before the turn hook. The recorded turns stop at the last whole one: a quit ends inside its turn (the delegate hears it terminates), and a refusal ends the recording before the refused call.

A split (`SPLT`) is the live timer's ([#406](https://github.com/armaatus/maplethief-port/issues/406)): its title in the category, the turn it fired in, and its two times since the run's start: the real time in the timer's whole milliseconds (× 1,000) and the game ticks. While a tape records or plays, the timer's real time is the guest's clock as the guest last read it (`mt_runtape_guest_clock_us`), so a replay gives the same times. The categories are written as `run_category_text` writes them, once per distinct id and text; `--play-run` puts them in its scratch data folder's `categories/`, where they replace a shipped category of the same name as a player's own would.

The guest memory digest is FNV-1a 64 over each writable image segment and the heap up to its committed end, each preceded by its `u32` address and `u32` length (`mt_snapshot_memory_digest`). The state line is `ticks=N mode=M map=A.B popup=P panel=K`.

## Reading it as untrusted input

The reader checks every length against what is left before it reads or allocates: a count of items is refused when it is more than the section's remaining bytes could hold. A refusal names the byte, `byte N` in the file for the header and `body byte N` in the inflated body for a section, with what is wrong there: `byte 32: the packed body's CRC-32 does not match`, `body byte 41: section SETS expected`, `body byte N: an event of unknown kind 9`, `body byte N: a number longer than 64 bits`. A boot file's name with `/`, `\` or `:` (or `.` or `..`) is refused: `--play-run` writes them into a folder of its own.

## Size

Display pacing makes the tape small: the guest's time moves only at a refresh, so the clock's value repeats for every read in between and the steps repeat too. Measured sizes per hour are in ADR 0156's addendum: about 0.2 MB locked to a 60 Hz display, 0.6 MB on the 1 ms grid, 14 MB with `pacing = wall` (every read sees a new microsecond).
