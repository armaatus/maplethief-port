# 228. Text_Wrap's runaway on a word wider than the box is cut in every mode

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#419](https://github.com/armaatus/maplethief-port/issues/419)
- **Builds on:** [ADR 0069](0069-guest-function-hooks.md) (guest function hooks, wrappers, `fix_original_bugs`), [ADR 0108](0108-text-packs.md) (the text pack's `Text_SubChars` step)

## Context

A Korean game with `fix_original_bugs` off shows the key `Chapter3_Quest_Zenumist_Chief.dialog.1` as the Zenumist Chief's line, because `ko.lproj` lacks it ([original bug 15](../re/original-bugs.md#15-a-dialogue-key-missing-in-korean)). The port died there (SIGBUS, exit 138, no log line). #419 traced why:

- `Text_Wrap` `0x22EC5` keeps the last space's character (`[r7-48]`, 0 at first). When the piece `[start, i)` is wider than the box it stores `Text_SubChars(start, lastSpace + 1)` as a line, sets `start = lastSpace + 1` and tries `i` again.
- With no space between `start` and `i`, the stored line is empty and `start` does not move: the loop stores empty lines without end. The first line of an unbroken word is its first character (`lastSpace` is 0); every later one is empty.
- The line array is 128 words on `Text_Wrap`'s own stack (`sp + 0x60`). The 515th line lands on `lastSpace`, so `start` becomes a heap pointer + 1, and the next `Text_SubChars` `0x22A1D`, which skips `from` characters without looking at the NUL, walks off the end of guest memory (pc `0x22A60`).

The device runs the same code on the same frame layout. Its `lastSpace` gets the same pointer, and the skip then walks hundreds of megabytes past the text; if that does not fault, the next lines overwrite the line count and the loop's own counter. The device most likely dies there too: there is no device "box shows the key" to reproduce. The setting-off rule ("the game does exactly what the device does") would keep a crash whose exact form cannot be known.

The fault is general: any word wider than its box, after any space or none, makes the loop run away. Only this key is known to trigger it in the shipped data, but a text pack can bring long words (a German compound in a 168-point box).

## Options considered

1. **Keep it (the device's crash).** Faithful in principle, but the crash's form is unknowable and the port's (SIGBUS, no log) is not the device's. A Korean Original or Classic game could not get past chapter 3's first map. Rejected.
2. **Pad guest memory past its top.** The skip then reads garbage instead of faulting, but the loop still overwrites its own frame: the result is undefined, not survivable. Rejected.
3. **A hook on `Text_SubChars` that stops at the NUL.** Ends the out-of-bounds read, but the loop still stores empty lines past its array and over its frame. Rejected.
4. **Replace `Text_Wrap`.** Re-implementing the layout risks every wrap the game does (ADR 0108 rejected it for the same reason), and the function already carries widescreen's handler.
5. **A wrapper on `Text_SubChars` at `Text_Wrap`'s too-wide call (return `0x22FFB`) that acts only on an empty line (`to <= from`).** The line becomes `[from, i - 1)`, the characters that fitted on the last try (at least one), and `lastSpace` in `Text_Wrap`'s frame becomes its end − 1, so `start` moves on to the character that did not fit. Chosen.

## Decision

- **`mt_hooks_install_text_wrap_guard(1)`** (`maplethief/hooks.h`, `runtime/src/hooks/text_wrap.cpp`): a wrapper (`mt_hook_wrap`) on `Text_SubChars` that changes the call's `to` and the frame's `lastSpace` and passes, so a text pack's handler (its UTF-8 step) or the original does the call. `mt_hooks_text_wrap_guarded()` counts the lines it cut.
- **Every mode.** `platform/src/game.cpp` and the test driver install it with 1, outside `fix_original_bugs` (decision "Neutralised in every mode" in re/original-bugs.md), as #167's quest page fix. It changes nothing the device survives: the guard acts only where the original's next step is the runaway, and every other call, the first line of an unbroken word included (cut after its first character), is the original's.
- **What the box shows** with the setting off: the key cut across lines at the box's width, the first line its "C" as the device's wrap cuts it: `C` / `hapter3_Quest_Zenumist` / `_Chief.dialog.1`. With the setting on, bug 15's English fallback still shows the English line.

## Evidence

- `tests/cpp/text_wrap_test.cpp` (`runtime_test`, CI): `Text_Wrap`'s loop over a synthetic guest (its frame in guest memory, `Text_SubChars` through `MT_FN_HOOK` with the game's return addresses, a stand-in that steps as the original and counts reads outside the string): without the guard the unbroken key runs away ("C", then empty lines); with it the lines join back to the key, each within the width, and no read leaves the string; a long word after a space and 30 Hangul syllables (never cut inside one) likewise; text that breaks at spaces wraps the same with the guard and without; a handler inside the wrapper gets the guard's arguments.
- `play.korean_missing_key_original_synthetic` (real assets, local): a Korean game, the setting off, on the Chief's line: exits 0, the box's lines `C|hapter3_Quest_Zenumist|_Chief.dialog.1|`, the guard counted. The same run with the guard not installed dies with a bus error.

## Consequences

- A Korean Original or Classic game passes the Chief's line, and the box shows the key, the closest result to the device's that does not end the game.
- A text pack's word wider than its box is cut there instead of crashing the game.
- The guard knows `Text_Wrap`'s frame (`[r7-36]` the character, `[r7-48]` the last space) and one return address; a different binary would need them found again (the same is true of every hook).
- How we'd know this is wrong: a Korean device that shows something else on that line (then the box's cut is a guess worth correcting), or a shipped text that reaches the guard with the setting on (`mt_hooks_text_wrap_guarded` counts it).
