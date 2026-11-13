# Windows PC hand-offs

The hand-offs that need the Windows PC. The order builds each tree once: the test builds first (`win-debug`, `win-release`), then the game folder from `scripts\build.ps1 win`, then the player zip, then the clean-machine and fresh-clone checks. [How to report](README.md#how-to-report) says what to post.

Older comments name branches that are merged and gone (`epic-201`, `epic-259-data-folder`, `epic-269-builds-docs`, `113-demo-msvc-only-warning`) or `main`: use `dev` (or the epic branch `epic-106-itch-handoffs` until it is merged). The v0.1.0 release (2026-11-07) predates the data folder and crash report work, so its Windows zip is only used to make older saves for #264.

## Setup

1. Visual Studio 2022 17.8+ with "Desktop development with C++" (CMake and Ninja come with it), Git for Windows, Python 3.10+, uv, and `gh` logged in. Optional: `winget install Casey.Just`, an up-to-date GPU driver with Vulkan.
2. `git fetch; git switch dev; git pull`.
3. Your copy of the game at `original\MapleThief-1.5.ipa` ([Your game file](../../play/your-ipa.md); `original/README.md` has the hashes).
4. `powershell -ExecutionPolicy Bypass -File scripts\build.ps1 doctor`: every line `ok` (or `note` for an optional tool). Then `scripts\build.ps1 bootstrap` and `scripts\build.ps1 recomp`.
5. **Which terminal:** `scripts\build.ps1` and `scripts\build-from-ipa.ps1` set up the compiler themselves, from any terminal. `scripts\test.ps1`, `scripts\release.ps1` and plain `cmake` need the **x64** developer prompt ("x64 Native Tools").
6. `MapleThief.exe` is a window program: to see what it prints, pipe it, as in `.\MapleThief.exe --print-data-dir | Out-String`.
7. For #82 a clean Windows 11 VM (or Windows Sandbox); for #264 administrator rights to copy into `C:\Program Files`.

## #113: Build, test and package on Windows

Closes [#113](https://github.com/armaatus/maplethief-port/issues/113) (its Windows criteria; the Mac and Linux halves passed on 2026-10-27, and the self-hosted runner task is moot now that hosted runners are free). In the x64 developer prompt:

1. `powershell -ExecutionPolicy Bypass -File scripts\test.ps1`: win-debug and pytest, with `work\ipa\` present.
2. Move `work\ipa` to `work\ipa.off`, run step 1 again (the real-asset tests report as skipped), move it back.
3. `scripts\test.ps1 -Release`: the win-release workflow with packaging; it makes `build\win-release\MapleThief-kit-0.0.1-windows-x64.zip`. This run also builds #131's tests.
4. The MSVC-only warning (the old demo branch is gone): `git switch -c tmp-c4459 dev`, append `static int mt_c4459_demo = 0; int mt_c4459_demo_fn() { int mt_c4459_demo = 1; return mt_c4459_demo; }` to `platform/src/file_dialog.cpp`, then `cmake --preset win-debug -DMAPLETHIEF_WARNINGS_AS_ERRORS=ON` and `cmake --build --preset win-debug`: it fails with C4459. Then `git checkout -- .`, `git switch dev`, `git branch -D tmp-c4459` and `cmake --preset win-debug -DMAPLETHIEF_WARNINGS_AS_ERRORS=OFF`.
5. Ask the agent for `gh workflow run ci.yml --ref dev` on the same commit, to compare the counts.

**Post back:** the summary of steps 1 to 3 (pass or fail with test counts), the kit's file name and the C4459 error line.

## #131: The GPU backend on D3D12 and Vulkan

Closes [#131](https://github.com/armaatus/maplethief-port/issues/131) (Windows half). Uses the `win-release` tree of #113 step 3. In the x64 developer prompt (PowerShell syntax, not the comment's `set`):

1. `ctest --test-dir build\win-release -L gpu -V > gpu-d3d12.log` (D3D12 is the default; `$env:MAPLETHIEF_GPU_DRIVER='direct3d12'` forces it).
2. `$env:MAPLETHIEF_GPU_DRIVER='vulkan'`, `ctest --test-dir build\win-release -L gpu -V > gpu-vulkan.log`, then `Remove-Item Env:MAPLETHIEF_GPU_DRIVER`.
3. The per-scene output is in `build\win-release\test-output\gpu_compare\`.

**Post back:** both logs with their per-scene numbers, and the GPU and driver version. `gfx_gpu: skipped` means no device was found.

## #141: One-command build on Windows

Closes [#141](https://github.com/armaatus/maplethief-port/issues/141) (the Mac half passed). Any terminal:

1. `powershell -ExecutionPolicy Bypass -File scripts\build.ps1 win`: it ends with `Built: dist\win\MapleThief-game-<version>-windows-x64` and its zip. Note the `Done in ... s` time.
2. Double-click `MapleThief.exe` in that folder: it starts with no import screen.
3. Step 1 again: every step says up to date and it takes seconds. Note the time.
4. If `just` is installed: `just build win`.

**Post back:** the doctor output (from setup), pass or fail per step, both `Done in` times and the `Built:` line.

## #198: Crash dialog and safe start (Windows part)

Covers the Windows part of [#198](https://github.com/armaatus/maplethief-port/issues/198); [Mac](mac.md#198-crash-dialog-and-safe-start-mac-part) has the other. In PowerShell, in the `dist\win\MapleThief-game-<version>-windows-x64` folder from #141:

1. `$env:MAPLETHIEF_DATA_DIR = "$HOME\mt-crash-test"`
2. `.\MapleThief.exe --preset original --force-crash host`: a "MapleThief crashed" box from a second process, naming a file in `crashes\`; **Open folder** shows it.
3. `.\MapleThief.exe --preset original --force-crash trap`: the same box.
4. Each crash file has the version lines, the guest report and the last log lines; `logs\` has a log with its header.
5. `.\MapleThief.exe --preset original`: the box offers to start with default settings. Choose it, play a little; `settings.ini` is unchanged.
6. `Remove-Item -Recurse $HOME\mt-crash-test; Remove-Item Env:MAPLETHIEF_DATA_DIR`

**Post back:** per step, whether the box showed and Open folder worked, the crash file's section headers, and the safe start's result.

## #142: Performance (Windows part)

Covers the Windows part of [#142](https://github.com/armaatus/maplethief-port/issues/142); see the [Mac section](mac.md#142-performance-under-rosetta-mac-part) for the measure and the criterion (at least 19.5 ticks a second, p99 under 75 ms).

1. In the #141 folder, start `.\MapleThief.exe --perf-session` at default settings.
2. Load a hero, stay on a map for 5 minutes or more, then F1 > Quit.
3. In the newest log in `data\logs\`, read the `game: ticks:` and `game: clock:` lines.

**Post back:** those two lines, and the PC's CPU and GPU.

## #264: Data folder sign-off (Windows part)

Covers the Windows part of [#264](https://github.com/armaatus/maplethief-port/issues/264); [Mac](mac.md#264-data-folder-sign-off-mac-part) and [Linux](linux.md#264-data-folder-sign-off-linux-part) have the others. Run `.\MapleThief.exe --print-data-dir | Out-String` before and after each step.

1. Build the player zip in the x64 developer prompt: `cmake --workflow --preset win-player`. It makes `build\win-player\MapleThief-0.0.1-windows-x64.zip` (no game inside; the first start imports your IPA). #192 and #82 use the same zip.
2. Older saves: `gh release download v0.1.0 -p "MapleThief-0.1.0-windows-x64.zip"`, unzip, import, save a hero, quit. That leaves saves in `%APPDATA%\MapleThief`.
3. Unzip the new zip into a fresh folder and start it: the older saves are copied into `data\` with a notice for about 20 s, and `%APPDATA%\MapleThief` is untouched.
4. Rename `%APPDATA%\MapleThief`, start in another fresh folder, import, play, save, change a setting and a binding: only `data\` and `game\` in that folder change, and `%APPDATA%\MapleThief` isn't created.
5. Copy the whole folder to a USB stick (or another PC) and start it: the hero, settings and bindings come along.
6. Delete `data\saves\`: empty slots, settings kept. Delete `data\`: a first start.
7. On the title, F1 > Saves > **Data location** > User folder: after the restart the hero is there and `location: user`. Switch back: the same.
8. Copy the folder to `C:\Program Files\MapleThief` and start it as a normal user: `reason: fallback-not-writable`, and the Saves tab's help says why.

**Post back:** one Windows comment with each step's result, the `--print-data-dir` outputs, and the Saves tab's text.

## #192: Saves in the menu (Windows part)

Covers the Windows part of [#192](https://github.com/armaatus/maplethief-port/issues/192); [Mac](mac.md#192-saves-in-the-menu-mac-part) has the rest. In the player zip's folder from #264 (or a copy at `C:\My Games\MapleThief`, a path with a space), on the title, F1 > **Saves**. The comment's `portable.txt` is gone since ADR 0127, and on Windows both fullscreen modes are the same borderless window, so one fullscreen pass is enough.

1. **Export saves...** opens in `data\exports\`. Save to the Desktop: `Exported to <name>.zip`. Again without `.zip`: it gets `.zip`. Again and Cancel: `Export cancelled`.
2. **Import saves...** opens in `data\import\` showing only `.zip` files. Pick that zip: `Imported N files from <name>.zip`.
3. Steps 1 and 2 in fullscreen (F11): the dialog shows over the game.
4. Load a character, then Import: no dialog, and `Import from the title or the main menu (a character is loaded)`.
5. **Open data folder** opens Explorer on the folder's `data\`; from `C:\My Games\MapleThief` too; and with Data location = User folder, `%APPDATA%\MapleThief`.
6. Optional, with a gamepad: the Backups list (D-pad, LT+down pages, A asks with No selected, Right then A restores, B back).

**Post back:** OK or the problem per step.

## #82: The Windows build on a clean machine

Closes [#82](https://github.com/armaatus/maplethief-port/issues/82). Uses the zip of #264 step 1.

1. On a Windows 11 machine with no Visual Studio and no VC++ redistributable (a fresh VM, or Windows Sandbox), unzip it and start `MapleThief.exe`.
2. Import your IPA at the prompt and reach the title screen.
3. Also copy #141's `dist\win` folder there and start it.

**Post back:** the Windows build number, that no Visual Studio or redistributable was installed, and pass or fail for steps 2 and 3.

## #85: Build from your IPA (Windows half)

Closes [#85](https://github.com/armaatus/maplethief-port/issues/85) (the macOS half passed). A fresh clone with no IPA in `original\`; it is the longest build, so start it while #264 and #82 run.

1. `git clone --branch dev https://github.com/armaatus/maplethief-port.git mt-fresh; cd mt-fresh`
2. Put your IPA at `C:\My Games\MapleThief-1.5.ipa`, then `powershell -ExecutionPolicy Bypass -File scripts\build-from-ipa.ps1 "C:\My Games\MapleThief-1.5.ipa" win`: it ends with `Built: dist/win/MapleThief-game-...-windows-x64`, and `original\MapleThief-1.5.ipa` now exists.
3. Double-click `MapleThief.exe`: it reaches the title screen.
4. `powershell -ExecutionPolicy Bypass -File scripts\build.ps1 win --ipa "C:\My Games\MapleThief-1.5.ipa"`: every step is up to date.

**Post back:** pass or fail for steps 2 to 4, step 2's `Done in` time, and the output if anything fails.

## #114: Release flow (already done)

[#114](https://github.com/armaatus/maplethief-port/issues/114) is done in practice: release v0.1.0 (2026-11-07) went through the release flow with all three kits and player zips, a `.sha256` for each and `SHA256SUMS`. Its `v0.0.1-rc.1` rehearsal and `scripts/release start` are superseded by the release PR ([the release checklist](../../release-checklist.md)). Nothing needs the PC; from any machine:

1. `gh release download v0.1.0 -D mt-v010`, then in that folder `shasum -a 256 -c SHA256SUMS` (on Windows, compare `Get-FileHash` with the file).
2. Unzip the Windows kit and run `bin\MapleThief.exe --version | Out-String`: `MapleThief 0.1.0`.

**Post back:** the release URL and both results; then close it.
