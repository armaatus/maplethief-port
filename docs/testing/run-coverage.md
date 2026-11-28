# Run coverage

How much of the game's code and data the automated runs execute ([#295](https://github.com/armaatus/maplethief-port/issues/295), [ADR 0160](../adr/0160-run-coverage-entry-counters-and-hit-recorders.md)). The report names guest functions by address and the scripts, natives, maps, imports and selectors by their keys; it is derived from your own copy of the game and stays in git-ignored `work/coverage/`. This page keeps only numbers and addresses.

## Making the report

Needs `work/` (`scripts/build bootstrap`, `scripts/build recomp`) and the function tags (`work/re/function-tags.json`, from `scripts/re-analyze`).

```sh
# Build mac-coverage, run the baseline tests one by one, merge, report:
PYTHONPATH=tools python3 -m mtport.runcoverage run
# Other tests (CTest name regexes; their fixture setups run first):
PYTHONPATH=tools python3 -m mtport.runcoverage run --tests 'play\.first_map' --out work/coverage/first-map
# Compare with an earlier report: what a change's runs added.
PYTHONPATH=tools python3 -m mtport.runcoverage report work/coverage/runs --baseline OLD/run-coverage.json
# Sum coverage files from anywhere (each MAPLETHIEF_COVERAGE file, or directories of them):
PYTHONPATH=tools python3 -m mtport.runcoverage merge work/coverage/all.txt work/coverage/runs other.txt
```

By hand: configure and build `mac-coverage` (`cmake --preset mac-coverage && cmake --build --preset mac-coverage`), run anything with `MAPLETHIEF_COVERAGE=<dir>/coverage-%p.txt` (and `MAPLETHIEF_SYMBOLS=build/mac-coverage/maplethief.sym` for names), then `report <dir>`. Without the counters build the file still has the imports, selectors and scripts, but no functions, natives or maps.

`run` writes:

- `work/coverage/runs/<test>/coverage-<pid>.txt`: each process's file;
- `work/coverage/run-coverage.txt`: the merged file (`# run` lines, functions, hits);
- `work/coverage/run-coverage.md` and `.json`: the report. Game code is every tagged function but Chartboost's 675 (`lib:chartboost`, never run by design: [re/libraries.md](../re/libraries.md)).

## The coverage gaps

[re/coverage-gaps.md](../re/coverage-gaps.md) gives every game function no run enters a class and the evidence (ADR 0232). To make it, collect the coverage of the whole real-asset suite into one directory (`MAPLETHIEF_COVERAGE=<dir>/coverage-%p.txt`; the suite takes about 1.5 h on the coverage build split into three fixture-connected partitions run in parallel, `ctest -R <partition> -FA '.*'`), then `python3 -m mtport.coveragegaps generate --coverage <dir>`. `python3 -m mtport.coveragegaps check` (and `tests/test_coveragegaps_real.py`) regenerates the doc from its own list and the current `work/`, so it fails when a re-analysis or a difftest change moved a class.

### Host-driven tests

The functions no play can enter (teardown, the system's events, a coupon) are entered by `play.host_events` ([#432](https://github.com/armaatus/maplethief-port/issues/432), [ADR 0242](../adr/0242-host-driven-tests-for-unreachable-functions.md)), about 30 s. To add its coverage (or any one run's) to the doc without the whole suite:

```sh
cmake --preset mac-coverage && cmake --build --preset mac-coverage --target play_real_test
MAPLETHIEF_COVERAGE=<dir>/coverage-%p.txt SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  build/mac-coverage/tests/play_real_test host_events <out dir> 1
PYTHONPATH=tools python3 -m mtport.coveragegaps update --coverage <dir>
```

On 2026-11-27 it entered 113 of the 124 `unreachable` functions; the other 11 have no sender and are `dead` rules. The teardown ran 86 destructors: 46 on live objects, 1 on an object made by its constructor (`CAniManager`) and 39 on zeroed objects with the class's vtables. It also ran 14 `dealloc`s, one on a new object (`TermsView`).

### Run steps

Every function play can reach is entered by a run (#433, #435): `input.menu_touches` and `input.menu_touches_more` touch the key pads' buttons and the GL touch widgets that the other flows drive by keys, and `runtime.scriptvm_corpus` runs every script function. To add them:

```sh
cmake --build --preset mac-coverage --target menu_nav_real_test scriptvm_real_test
cd build/mac-coverage && MAPLETHIEF_COVERAGE=<dir>/coverage-%p.txt \
  ctest -R 'input\.menu_touches|runtime\.scriptvm_corpus' --output-on-failure
PYTHONPATH=tools python3 -m mtport.coveragegaps update --coverage <dir>
```

A function no run enters is a `dead` rule with its evidence (`RULES` in `tools/mtport/coveragegaps.py`) or a new run step: `tests/test_coveragegaps_real.py` fails on a `missed` row.

## The baseline

2026-11-26, branch `armaatus/295-run-coverage` at `3c6e8ad3` (on `dev` after #415), Apple M4, macOS 26.6.2, `mac-coverage` build, `python3 -m mtport.runcoverage run` with its default tests: `play.chapter_1_a` to `play.chapter_1_m`, `play.regress`, every `menus.*` and every `input.*` (53 tests, all real-asset, 0 failed). `play.regress` gave its usual digest `8a2f05733c8d4603` with the recorders on.

- **Game code: 1,291 of 1,980 functions executed (65.2%)**, Chartboost not counted.
- All functions: 1,291 of 2,655; 2,359,914,907 entries.
- Scripts read: 113 of 307 (36.8%).
- Natives called: 36 of 65 (55.4%).
- Maps entered: 30 of 90 (33.3%).
- Game imports called: 136 of 225 (60.4%).
- (class, selector) pairs called: 753.

| Tag | Executed | Total | % |
|---|---:|---:|---:|
| `app-objc` | 279 | 620 | 45.0% |
| `engine` | 1,008 | 1,353 | 74.5% |
| `lib:apple-openal-support` | 1 | 2 | 50.0% |
| `lib:chartboost` | 0 | 675 | 0.0% |
| `lib:crt1` | 0 | 1 | 0.0% |
| `lib:uidevice-identifier` | 3 | 4 | 75.0% |

Functions entered per run:

| Run | Functions entered |
|---|---:|
| `input.bindings_flags` | 313 |
| `input.carry_keys` | 838 |
| `input.carry_pad` | 815 |
| `input.chords` | 889 |
| `input.chords_mash` | 817 |
| `input.chords_mixed` | 808 |
| `input.gamepad_climb_game` | 811 |
| `input.gamepad_climb_last` | 812 |
| `input.gamepad_climb_stick` | 812 |
| `input.gamepad_disconnect` | 817 |
| `input.gamepad_hold` | 819 |
| `input.gamepad_play` | 834 |
| `input.keyboard_climb_game` | 811 |
| `input.keyboard_climb_last` | 812 |
| `input.keyboard_hold` | 816 |
| `input.keyboard_menus` | 611 |
| `input.keyboard_play` | 908 |
| `input.keypad_hide` | 819 |
| `input.keypad_layout` | 819 |
| `input.menu_characters` | 613 |
| `input.menu_early` | 577 |
| `input.menu_game` | 946 |
| `input.menu_main` | 593 |
| `input.menu_pointer` | 339 |
| `input.menu_popup_grids` | 804 |
| `input.menu_shop_scroll` | 805 |
| `input.menu_shops` | 865 |
| `input.menu_shops_save` | 1,013 |
| `input.menu_terms` | 337 |
| `input.remap_play` | 826 |
| `input.rumble` | 821 |
| `menus.create` | 615 |
| `menus.delete` | 615 |
| `menus.help_about` | 329 |
| `menus.new_game` | 577 |
| `menus.offline_ui` | 325 |
| `menus.online` | 335 |
| `menus.select` | 605 |
| `menus.terms` | 337 |
| `play.chapter_1_a` | 1,072 |
| `play.chapter_1_b` | 1,130 |
| `play.chapter_1_c` | 944 |
| `play.chapter_1_d` | 1,106 |
| `play.chapter_1_e` | 1,088 |
| `play.chapter_1_f` | 1,102 |
| `play.chapter_1_g` | 1,103 |
| `play.chapter_1_h` | 1,086 |
| `play.chapter_1_i` | 1,087 |
| `play.chapter_1_j` | 1,004 |
| `play.chapter_1_k` | 1,091 |
| `play.chapter_1_l` | 1,044 |
| `play.chapter_1_m` | 976 |
| `play.regress` | 920 |
