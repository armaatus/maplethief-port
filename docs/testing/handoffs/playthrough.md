# Playthrough, review and release hand-offs

The hand-offs that need play time and judgement rather than one device, then the release. Most are played on the Mac's game app (`scripts/build mac`, `dist/mac/MapleThief.app`); #92 repeats the playthrough on every platform. Do them in this order: the approvals of legal wording (#326, #331, #318), then the release rehearsal and its parts (#354, #343, #317), and the release last because it needs the others signed off. The checks from closed issues sit beside the hand-off they go with: #67 and #72 after #132, and the two calls for Joris (#156, #162) after the playthrough they are made in ([Checks from closed issues](README.md#checks-from-closed-issues)). [How to report](README.md#how-to-report) says what to post.

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

## #67: Real pads: hot-plug, focus and the dead zone

From closed [#67](https://github.com/armaatus/maplethief-port/issues/67). Its play steps moved to #132 above; these are the ones #132 doesn't ask. With `MAPLETHIEF_TRACE=input` as in #132 step 1:

1. Unplug the pad while walking: the hero stops. Plug it back in: it plays again without a restart.
2. Plug in the second pad: it takes over only once the first is idle and you press something on it.
3. Cmd+Tab (Alt+Tab) away while holding a direction: the hero stops; back in the window, the pad works again after you let go and press.
4. Stick diagonals near 45 degrees don't flicker between directions; letting the stick go stops the walk at once.
5. Does the stick's dead zone (30 % to engage, 22 % to let go) feel right? Any button you would map differently?

**Post back:** the pad models and the OS, pass or fail per step, and the `input:` log lines of anything that didn't behave.

## #72: Rumble: the game's vibration option and the strength

From closed [#72](https://github.com/armaatus/maplethief-port/issues/72). #132 step 5 checks the hit and level-up rumble and the flags; this is the rest.

1. With the game's own vibration option on (its default), a hit gives the stronger 0.4 s buzz merged with the hit's thump.
2. Toggle the vibration in the game's options: the preview buzzes once (0.4 s). With it off, hits still give the short thump.
3. Is the default strength too strong or too weak, and for which effect (hit, vibration, level-up)? The exit line `game: N hits taken, M level-ups; rumble: ...` says what was sent.

**Post back:** pass or fail of steps 1 and 2, and per effect "fine", "stronger" or "weaker". The agent retunes `platform/src/rumble.h` from that.

## #93: The build guide on a clean machine

Closes [#93](https://github.com/armaatus/maplethief-port/issues/93). Follow [the build guide](../../BUILDING.md) exactly on one clean machine: macOS 13+ with only the Command Line Tools (or a fresh user account), a clean Windows 10/11 VM (it also covers [#82](windows.md#82-the-windows-build-on-a-clean-machine)), or Ubuntu 24.04 with Docker.

1. Install what the page lists, clone, put your IPA in `original/` (or pass `--ipa`).
2. `just doctor`, then `just build`.
3. Start the result from `dist/<platform>/` as the page says, and get to the first map.
4. Note everything the page left out or got wrong: a package name, the Linux `docker` group step, whether the Command Line Tools alone are enough (their python3 is 3.9), `scripts\build.ps1` on a real PC.

**Post back:** the OS, pass or fail from clone to the first map using only the page, and every tool forgotten, command changed or step guessed.

## #79: Full playthrough

Closes [#79](https://github.com/armaatus/maplethief-port/issues/79). The list is the [playthrough checklist](../playthrough-checklist.md): `PYTHONPATH=tools python3 -m mtport.compendium.kb build`, then `PYTHONPATH=tools python3 -m mtport.compendium.playthrough` writes `build/checklists/playthrough.md`, every quest, boss, job advancement, climb and scene chapter by chapter with a tick box each. The compendium's walkthrough has the routes: [chapter 1](https://armaatus.github.io/maplethief-port/walkthrough/chapter-1.html), [chapter 2](https://armaatus.github.io/maplethief-port/walkthrough/chapter-2.html), [chapter 3](https://armaatus.github.io/maplethief-port/walkthrough/chapter-3.html), [the endings](https://armaatus.github.io/maplethief-port/walkthrough/endings.html) and [the bosses](https://armaatus.github.io/maplethief-port/monsters/bosses.html). Do #223's quests and #228's chapter 1 run along the way.

1. Optional baseline first: `scripts/regress`, and the automated chapter 1 chain ([chapter-1-quests.md](../chapter-1-quests.md)).
2. Play chapters 1 to 3, every mini-game and every ending in `dist/mac/MapleThief.app`, ticking `build/checklists/playthrough.md` as you go.
3. File each bug with the bug template.

**Post back:** "checklist completed, signed off", the rows you could not tick (by their ids) and the bug issue numbers.

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

## #156: A call for Joris: the cash-shop prices

From closed [#156](https://github.com/armaatus/maplethief-port/issues/156). [ADR 0072](../../adr/0072-offline-cash-shop-and-revives.md) leaves the tuning of the prices to a playthrough (its Consequences: game feel). `cash_shop = meso` is an Assist, off in every preset: turn it on (Settings > Assists > **Cash shop**) for part of #79, or a second hero.

1. `PYTHONPATH=tools python3 -m mtport.cashprice work/ipa/Payload/MapleThief.app/English/data` prints the price curve per level. The knobs and their defaults are in [the cash shop notes](../../re/cash-shop.md) and `runtime/src/cashshop/pricing.cpp`.
2. Early on (below level 35): do the revives' 9,000 meso floor and the 100 meso minimum feel right?
3. Dying or buying more than once at one level: is x1.5 a step, at most 4 steps, too harsh or too soft?
4. The price line in the shop's and the death popup's corner: can it be read, is it in the way?

**Post back:** keep the defaults, or the knobs to change and to what. Any value can change in a later release without touching a save.

## #162: A call for Joris: main 20 never reaches done

From closed [#162](https://github.com/armaatus/maplethief-port/issues/162). Chapter 1's main 20 (Master in Danger 2) is given by Stan and stays active until the trip to Orbis, whose `clearAll` sets every chapter 1 quest back to 0: no script ever rewards it ([chapter 1 quests](../chapter-1-quests.md), main 20). The port does what the game's scripts say; the call is whether that is the original's behaviour too.

1. During #79, look at the quest list just before the trip: main 20 shows as active, not done.
2. If you played the original to Orbis on the device and remember main 20 ending some other way (a reward, a message), say where.

**Post back:** "accept: main 20 ends with the trip" (the playthrough checklist then ticks it at the trip), or what the original did instead.

## #92: The playthrough on every platform

Closes [#92](https://github.com/armaatus/maplethief-port/issues/92). The template is `build/checklists/platforms.md` from the same command as #79 ([playthrough checklist](../playthrough-checklist.md)): a sign-off row per platform, the Switch included, and per platform a column per preset and a controller-first section. The #79 playthrough, shorter, on each platform's **player build** (from the release, or built), with the import on first start, once per preset (Original, Original + controller, Enhanced). Each platform also gets a controller-first pass: plays with the defaults and no launcher, glyphs match the pad, legible at 1280 x 800, text entry with a controller, a steady 20 ticks a second (#142).

1. macOS: the `mac-player` kit, also once under Rosetta (`arch -x86_64`).
2. Windows: the `win-player` kit ([Windows](windows.md)).
3. iPhone: `scripts/build ios` ([iPhone](iphone.md)).
4. Linux and the Steam Deck: `scripts/linux-game` ([Linux](linux.md)).
5. Switch: the [Switch checklist](../switch-checklist.md), signed off on #245.

**Post back:** one signed-off comment per platform.

## #326: The SECURITY.md wording

Covers [#326](https://github.com/armaatus/maplethief-port/issues/326)'s human part: approving legal wording. **Waits on** the agent's SECURITY.md and code of conduct. Switching on private vulnerability reporting is a repository setting, so the agent applies it and lists it in its pull request.

1. Read SECURITY.md (the private route, what is in scope) and the code of conduct's rules on game files and links in the pull request the agent names on #326.
2. Approve, or say what to change.

**Post back:** approved, or the changes.

## #331: One legal paragraph

Covers [#331](https://github.com/armaatus/maplethief-port/issues/331)'s human part: approving legal wording. **Waits on** the agent's paragraph, kept in one place and reused in the README, [docs/legal.md](../../legal.md) and [Your game file](../../play/your-ipa.md). The repository's description and topics are settings, so the agent applies them and lists them in its pull request.

1. Read the paragraph in the pull request the agent names on #331.
2. Approve, or say what to change.

**Post back:** approved, or the changes.

## #318: The takedown runbook

Closes [#318](https://github.com/armaatus/maplethief-port/issues/318)'s approval. **Waits on** the agent's runbook (what comes down in which order, what stays, and the rule that no counter-notice is sent without legal advice) and its private contact route in [docs/legal.md](../../legal.md) and SECURITY.md (#326).

1. Read the runbook in the pull request the agent names on #318, and the dry run it records on the issue.
2. Approve, or say what to change.

**Post back:** approved, or the changes.

## #354: The signing key for checksums

Closes [#354](https://github.com/armaatus/maplethief-port/issues/354)'s key criterion. **Waits on** the agent's part of #354 (the ADR 0024 addendum and the signing step in `scripts/release publish`). The key is yours and never enters the repository or CI.

1. Install minisign on the Mac and generate a key pair as the addendum says, with a passphrase. Keep the secret key outside the repository, with a backup.
2. Give the agent the **public** key (it goes in the repository).
3. At the next pre-release (#317), `scripts/release publish` signs `SHA256SUMS`; then, in an empty folder, download the release and run the verify command from the play docs.

**Post back:** the verify command's output. Never post the secret key or its passphrase.

## #343: One release command on both machines

Closes [#343](https://github.com/armaatus/maplethief-port/issues/343)'s release criterion. **Waits on** the agent's part of #343 (the one resumable command and the maintainer runbook). At the next pre-release (#317 or later):

1. On the Mac, the one command the runbook names, with the tag.
2. When it stops at the step for the PC, the one command it prints, on the PC; then the Mac command again, which resumes.

**Post back:** the release's address, and where the runbook was unclear.

## #317: The `v1.0.0-rc.1` rehearsal

Closes [#317](https://github.com/armaatus/maplethief-port/issues/317)'s human part. **Waits on** #308, #313 and #316; #354 and #343 can ride on it. The same flow as [#94](#94-release-v100) with the tag `v1.0.0-rc.1`, published as a pre-release:

1. The release PR from `dev` titled `Release v1.0.0-rc.1`; merge it when every OS is green.
2. Mac: `git fetch origin --tags && scripts/release build v1.0.0-rc.1 --players-only`. PC: `powershell -ExecutionPolicy Bypass -File scripts\release.ps1 build v1.0.0-rc.1 -PlayersOnly`. Then on the Mac `scripts/release publish v1.0.0-rc.1`.
3. Per system, on a clean user account or VM, from the downloaded files: unpack, the system's warning as the play docs describe it, import, the preset picker, the first map, save, quit, start again.

**Post back:** the release's address, `shasum -a 256 -c SHA256SUMS`, and a smoke table: one row per system (Windows, macOS, Linux), pass or the step that differed from the docs.

## #94: Release v1.0.0

Closes [#94](https://github.com/armaatus/maplethief-port/issues/94). Needs #79, #92, #93 and #228 signed off. The release goes through a release PR, and the assets are the player builds, the kits and the Switch zip (as in v0.1.0); #94 was reworded to match on 2026-11-13, when ADR 0146 was accepted. [The release checklist](../../release-checklist.md) is the full procedure; in short:

1. Before: label the epic PRs since v0.1.0, read `.github/release-notes-header.md`, `gh auth status` and `scripts/bootstrap && python3 -m recomp all` on both machines.
2. Optional rehearsal: `git tag -a v0.0.0-rc.99 -m rehearsal`, `scripts/release build v0.0.0-rc.99 --dry-run`, `git tag -d v0.0.0-rc.99`.
3. The release PR: `gh pr create --base main --head dev --title "Release v1.0.0"`. Merge it with a merge commit when every OS is green; `release.yml` tags and drafts.
4. Mac: `git fetch origin --tags && scripts/release build v1.0.0 --players-only`.
5. Windows PC: `git fetch origin --tags`, then `powershell -ExecutionPolicy Bypass -File scripts\release.ps1 build v1.0.0 -PlayersOnly`.
6. Mac: `scripts/release publish v1.0.0` (add `--platforms mac,linux,windows` if there is no Switch zip).
7. itch.io (after the #208 one-time steps below): `scripts/release itch v1.0.0 --dry-run`, then `scripts/release itch v1.0.0` on the Mac, and `scripts\release.ps1 itch v1.0.0` on the PC.
8. After: in an empty folder, `shasum -a 256 -c SHA256SUMS`; each player build's `--version` prints `MapleThief 1.0.0`; a fresh-folder import per player build.

**Post back:** the release URL, the `shasum -c` output and the `--version` lines.

## #208: itch.io page

Covers [#208](https://github.com/armaatus/maplethief-port/issues/208)'s human part. The tooling and the page text are in this repository ([ADR 0146](../../adr/0146-distribution-on-itch-io.md), accepted): `scripts/release itch TAG` pushes the published player builds, the Switch zip included, and [the release checklist](../../release-checklist.md), step 5, has the full procedure.

1. One time: create the itch.io project `armaatus/maplethief` (downloadable, draft) with "$0 or donate", payouts and the tax form. Create an itch API key and set it as `BUTLER_API_KEY` in the environment on the Mac and the PC; never commit it or put it on a command line. Add a cover and banner of your own art, and gameplay screenshots from a local build (never committed).
2. Paste [docs/itch/page.md](../../itch/page.md) into the page editor and fill the fields from [docs/itch/metadata.md](../../itch/metadata.md).
3. After #94's publish, on the Mac: `scripts/release itch v1.0.0 --dry-run`, then `scripts/release itch v1.0.0` (macOS, Linux and Switch channels). On the PC: `powershell -ExecutionPolicy Bypass -File scripts\release.ps1 itch v1.0.0` (Windows channel).
4. Download each file from the page into an empty folder and compare its `shasum -a 256` with `SHA256SUMS`. Publish the page, then post the launch devlog in your own words.

**Post back:** `butler status` showing every channel at 1.0.0, the checksum comparison, and the page's address.
