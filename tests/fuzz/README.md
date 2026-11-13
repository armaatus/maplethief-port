# Fuzz targets

libFuzzer targets for the parsers of files a player can supply ([ADR 0154](../../docs/adr/0154-threat-model-for-player-supplied-files.md)), built with clang, ASan and UBSan by the `linux-fuzz` preset ([ADR 0158](../../docs/adr/0158-libfuzzer-targets-and-the-fuzz-smoke.md)). Running them, short and long: [docs/build/testing.md](../../docs/build/testing.md#fuzzing).

## Adding a target

A target `NAME` is three files here and one line in `CMakeLists.txt`:

| File | What it is |
|---|---|
| `NAME_fuzz.cpp` | `extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)`: hands the bytes to the parser, as the game or the frontend would, and returns 0. Built as `fuzz_NAME`. |
| `seeds/NAME.py` | `seeds() -> dict[str, bytes]`: the seed corpus, file name to bytes. Built in code, or read from synthetic fixtures under `tests/` (`pngfixtures.py`, `tests/cpp/data/`): **never a game file**. Optional `MAX_LEN = <bytes>` caps an input (default 64 KiB). Shared helpers go in `seeds/_NAME.py` (a leading `_` is not a target). |
| `NAME.dict` | Optional: a [libFuzzer dictionary](https://llvm.org/docs/LibFuzzer.html#dictionaries) of the format's keywords and magic numbers. |

```cmake
maplethief_add_fuzz_target(NAME LIBRARIES runtime)   # plus maplethief_frontend for platform/src
```

That builds `fuzz_NAME`, writes its seeds to `build/linux-fuzz/tests/fuzz/seeds/NAME/` (`make_corpus.py`), and registers CTest `fuzz.NAME`, which runs it for `MAPLETHIEF_FUZZ_SMOKE_SECONDS` (30) from those seeds. `scripts/fuzz NAME` then runs it for hours. Headers under `runtime/src/` and (with `maplethief_frontend`) `platform/src/` are on the include path. Each target gets `no_guest.cpp`'s `mt_dispatch`, which aborts: nothing parsed runs guest code. A target that drives a test runtime with guest code passes `OWN_DISPATCH` and defines its own.

Rules for a harness:

- **Drive the real entry point**, the one the untrusted bytes reach (`zip_read`, `parse_settings`, `Mp3Stream::open`, …), with the limits the caller passes. Where the parser has a round trip (serialise and parse again, store and read again), check it and `__builtin_trap()` on a mismatch: a wrong answer is a finding too.
- **One input, no state carried over.** Reset anything global the parser keeps. Files the code under test writes go in a scratch folder of the harness's own, emptied every input.
- **No output.** No logging per input (configure `mt_diag` to drop lines); libFuzzer runs it millions of times.
- **Several entry points** of one parser can share a target: the first byte picks which (`ipa_parts_fuzz.cpp`).
- **Warnings are errors** as everywhere (`maplethief_target_defaults`).

## A finding

A run that finds something stops and leaves the input in `build/test-output/linux-fuzz/fuzz/NAME/` (the smoke) or `build/fuzz/NAME/` (`scripts/fuzz`), named `crash-`, `leak-`, `timeout-` or `oom-` and its SHA-1. Every finding becomes a regression test in `tests/cpp/` (a doctest case that feeds those bytes, written out in the test, to the parser), then the parser is fixed. Add the input to `seeds/NAME.py` too when it reaches a path the seeds don't.

## The targets

| Target | Parser | Entry points |
|---|---|---|
| `zip` | `platform/src/save_zip.cpp` | `zip_read` (both forms), `zip_list` + `zip_extract`, and the store round trip |
| `ipa` | `platform/src/ipa_import.cpp` | `check_ipa_bytes`, and `import_ipa_bytes` past the hash check into a scratch folder |
| `ipa_parts` | `platform/src/ipa_import.cpp` | `plist_string`, `macho_cryptid`, `ipa_entry_parts` |
| `png` | `runtime/src/cg/codec.cpp` (stb_image) | `png_info`, `png_decode` (PNG and CgBI) |
| `texpack` | `runtime/src/texpack/texpack.cpp` | a texture pack's PNG, through `mt_texpack_open` and an upload |
| `border` | `platform/src/border.cpp` | `decode_border_image` |
| `wav` | `runtime/src/audio/wav.cpp` | `wav::parse`, `wav::to_s16` |
| `mp3` | `runtime/src/audio/mp3_stream.cpp` (dr_mp3) | `Mp3Stream::open` and `read`, `decode_file` |
| `flac` | `runtime/src/audio/mp3_stream.cpp` (dr_flac) | `Mp3Stream::open_pack`, `decode_file` |
| `strings` | `runtime/src/textpack/strings_file.cpp` | `parse_strings` |
| `settings` | `platform/src/settings.cpp` | `parse_settings`, and the `serialise_settings` round trip |
| `bindings` | `platform/src/bindings.cpp` | `parse_bindings`, and the `serialise_bindings` round trip |
