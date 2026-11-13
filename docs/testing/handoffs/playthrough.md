# Playthrough, review and release hand-offs

The hand-offs that need play time and judgement rather than one device, then the release. Most are played on the Mac's game app (`scripts/build mac`, `dist/mac/MapleThief.app`); #92 repeats the playthrough on every platform. Do them in this order: the release comes last because it needs the others signed off. [How to report](README.md#how-to-report) says what to post.

The compendium site is live at <https://armaatus.github.io/maplethief-port/>; the review steps link its pages there. The LAN address in the older comments is gone. To check a local build instead: `scripts/compendium serve`, then <http://127.0.0.1:8000/maplethief-port/>.

## Setup

1. Your copy of the game at `original/MapleThief-1.5.ipa` on every machine that builds, then `scripts/bootstrap && python3 -m recomp all`.
2. On the Mac: `just doctor`, then `scripts/build mac`. Docker (OrbStack) running for the Linux and Switch builds; `gh auth login`.
3. On the Windows PC: Visual Studio 2022 with the C++ workload, Python 3.10+, Git and `gh` logged in. A separate clean Windows VM for #93.
4. An Xbox controller and a PlayStation controller.

## #132: Gamepad only and keyboard only

Closes [#132](https://github.com/armaatus/maplethief-port/issues/132). About an hour, on the Mac (Windows optional). The #67 comment's "menu navigation by pad is #69/#75" no longer applies: menus work with the pad. The #72 comment's "build the epic branch" is now `scripts/build mac` from `dev`.

1. `MAPLETHIEF_TRACE=input dist/mac/MapleThief.app/Contents/MacOS/MapleThief`. The log shows `input: gamepad N connected: <name> (<type>)`.
2. Xbox controller only: main menu to the first map and a few minutes of combat, with the mapping in [Key pad and gamepad](../../controls/keypad.md).
3. The same with only the PlayStation controller.
4. Keyboard only, with the defaults in [Keyboard](../../controls/keyboard.md).
5. Rumble: a hit gives about 0.2 s, a level-up about 0.7 s. Then start with `--rumble-strength 30`, and with `--rumble off`.
6. Remap: F1 > Controls > Bindings, rebind one action, Save, quit and start again: it still works ([Remapping](../../controls/remapping.md); it is kept in `bindings.ini` in the data folder). Plug in the other pad type: the glyphs change.

**Post back:** steps 2 to 4 confirmed, rumble OK (or a tuning issue filed), and the remap kept.

## #93: The build guide on a clean machine

Closes [#93](https://github.com/armaatus/maplethief-port/issues/93). Follow [the build guide](../../BUILDING.md) exactly on one clean machine: macOS 13+ with only the Command Line Tools (or a fresh user account), a clean Windows 10/11 VM (it also covers [#82](windows.md#82-the-windows-build-on-a-clean-machine)), or Ubuntu 24.04 with Docker.

1. Install what the page lists, clone, put your IPA in `original/` (or pass `--ipa`).
2. `just doctor`, then `just build`.
3. Start the result from `dist/<platform>/` as the page says, and get to the first map.
4. Note everything the page left out or got wrong: a package name, the Linux `docker` group step, whether the Command Line Tools alone are enough (their python3 is 3.9), `scripts\build.ps1` on a real PC.

**Post back:** the OS, pass or fail from clone to the first map using only the page, and every tool forgotten, command changed or step guessed.

## #79: Full playthrough

Closes [#79](https://github.com/armaatus/maplethief-port/issues/79). There is no chapter by chapter checklist in the repository yet (the issue's agent task); use the compendium's walkthrough as the list: [chapter 1](https://armaatus.github.io/maplethief-port/walkthrough/chapter-1.html), [chapter 2](https://armaatus.github.io/maplethief-port/walkthrough/chapter-2.html), [chapter 3](https://armaatus.github.io/maplethief-port/walkthrough/chapter-3.html), [the endings](https://armaatus.github.io/maplethief-port/walkthrough/endings.html) and [the bosses](https://armaatus.github.io/maplethief-port/monsters/bosses.html). Do #223's quests and #228's chapter 1 run along the way.

1. Optional baseline first: `scripts/regress`, and the automated chapter 1 chain ([chapter-1-quests.md](../chapter-1-quests.md)).
2. Play chapters 1 to 3, every mini-game and every ending in `dist/mac/MapleThief.app`.
3. File each bug with the bug template.

**Post back:** "checklist completed, signed off" and the bug issue numbers.

## #223: Five compendium quests in the port

Closes [#223](https://github.com/armaatus/maplethief-port/issues/223) (only its port criterion is left). During #79, play each quest from its page alone:

1. [0-main-5](https://armaatus.github.io/maplethief-port/quests/0-main-5.html), [1-sub-21](https://armaatus.github.io/maplethief-port/quests/1-sub-21.html), [1-main-10](https://armaatus.github.io/maplethief-port/quests/1-main-10.html) (chapters 0 and 1)
2. [2-sub-21](https://armaatus.github.io/maplethief-port/quests/2-sub-21.html) (chapter 2)
3. [3-sub-1](https://armaatus.github.io/maplethief-port/quests/3-sub-1.html) (chapter 3)

**Post back:** each quest completed as written, or the step number that didn't match and what happened.

## #228: Compendium accuracy review

Closes [#228](https://github.com/armaatus/maplethief-port/issues/228). Its 2026-11-06 comment is the checklist; read its links on the live site. It must be signed off before any announcement (the itch launch post, the v1.0 notes).

1. Section 1: check 20 or more pages against the game.
2. Section 2: play chapter 1 following only the chapter 1 walkthrough (with #79).
3. Section 3: read the three prose pages it names for tone.
4. Section 4: confirm or reject the "probable" and "unknown" facts.
5. Section 5: the port checks of #218 (the Yeti fight start), #220, #221, #222 and #223.

**Post back:** each item ticked or annotated, then "signed off". The agent then fixes and redeploys, with `scripts/compendium check` green.

## #92: The playthrough on every platform

Closes [#92](https://github.com/armaatus/maplethief-port/issues/92). The #79 playthrough, shorter, on each platform's **player build** (from the release, or built), with the import on first start, once per preset (Original, Original + controller, Enhanced). Each platform also gets a controller-first pass: plays with the defaults and no launcher, glyphs match the pad, legible at 1280 x 800, text entry with a controller, a steady 20 ticks a second (#142).

1. macOS: the `mac-player` kit, also once under Rosetta (`arch -x86_64`).
2. Windows: the `win-player` kit ([Windows](windows.md)).
3. iPhone: `scripts/build ios` ([iPhone](iphone.md)).
4. Linux and the Steam Deck: `scripts/linux-game` ([Linux](linux.md)).
5. Switch: the [Switch checklist](../switch-checklist.md), signed off on #245.

**Post back:** one signed-off comment per platform.

## #94: Release v1.0.0

Closes [#94](https://github.com/armaatus/maplethief-port/issues/94). Needs #79, #92, #93 and #228 signed off. The issue body predates ADR 0126 and the public repository: there is no hand-pushed tag, the release goes through a release PR, and the assets are the player builds and the kits (as in v0.1.0). [The release checklist](../../release-checklist.md) is the full procedure; in short:

1. Before: label the epic PRs since v0.1.0, read `.github/release-notes-header.md`, `gh auth status` and `scripts/bootstrap && python3 -m recomp all` on both machines.
2. Optional rehearsal: `git tag -a v0.0.0-rc.99 -m rehearsal`, `scripts/release build v0.0.0-rc.99 --dry-run`, `git tag -d v0.0.0-rc.99`.
3. The release PR: `gh pr create --base main --head dev --title "Release v1.0.0"`. Merge it with a merge commit when every OS is green; `release.yml` tags and drafts.
4. Mac: `git fetch origin --tags && scripts/release build v1.0.0 --players-only`.
5. Windows PC: `git fetch origin --tags`, then `powershell -ExecutionPolicy Bypass -File scripts\release.ps1 build v1.0.0 -PlayersOnly`.
6. Mac: `scripts/release publish v1.0.0` (add `--platforms mac,linux,windows` if there is no Switch zip).
7. After: in an empty folder, `shasum -a 256 -c SHA256SUMS`; each player build's `--version` prints `MapleThief 1.0.0`; a fresh-folder import per player build.

**Post back:** the release URL, the `shasum -c` output and the `--version` lines.

## #208: itch.io page

Covers [#208](https://github.com/armaatus/maplethief-port/issues/208)'s human part. The tooling (`scripts/release itch`, the page text) lands with the #208 child of epic #106; until it is merged, only the one-time steps can be done, and this section gets its exact commands then.

1. One time: create the itch.io project (downloadable, draft) with "$0 or donate", payouts and the tax form. Create an itch API key and set it as `BUTLER_API_KEY` on the Mac and the PC; never commit or paste it. Supply the donation link and a cover and banner of your own art.
2. After #94's publish: push the builds with butler from both machines, paste the page text into the itch editor and publish, then post the launch devlog in your own words.

**Post back:** `butler status` showing every channel at 1.0.0, that each download's checksum matches `SHA256SUMS`, and the page's address.
