# 147. What v1.0 is, which platforms gate it and which hand-offs it waits for

- **Status:** Accepted (Joris, 2026-11-13, [on #308](https://github.com/armaatus/maplethief-port/issues/308))
- **Date:** 2026-11-13
- **Issue:** [#308](https://github.com/armaatus/maplethief-port/issues/308) (epic [#286](https://github.com/armaatus/maplethief-port/issues/286))
- **Builds on:** [ADR 0024](0024-release-contents-and-flow.md) (kits and the release flow), [ADR 0100](0100-public-repository-and-player-releases.md) (public repository, player builds without assets), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (a player build per platform), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (release pull requests), [ADR 0146](0146-distribution-on-itch-io.md) (itch.io)

## Context

The M11 issues disagreed about what v1.0 is. #94's old title said "private release: own-code kits only, never game builds", #106's exit criteria said "tagged privately with no public binaries", and #114 said "GitHub Release (private repo)". ADR 0100, ADR 0125, ADR 0126 and ROADMAP M11 say a public v1.0 with a player build per platform, and v0.1.0 (2026-11-07) already shipped player builds that way. ADR 0146 reworded #94 for itch.io but left the rest.

The second gap: about 38 open issues waited only on Joris's hands (the `needs-human` label, collected per device by #283 in `docs/testing/handoffs/`), and nothing said which of them must be done before the tag. Without that list v1.0 waits on all of them, the Switch console runs and the iPhone device checks included, which need hardware and accessories that are not always at hand.

The hand-off rule also changed on 2026-11-13 (CLAUDE.md, "Human hand-offs"): `needs-human` now means real hardware, Joris's identity or money, or approving legal wording. Judgement calls (design ADRs, defaults, look-and-feel checks an agent can run on its own runs) and repository settings are agent work. Some open `needs-human` issues are only that.

## Options considered

1. **Every open hand-off gates v1.0.** Nothing ships unchecked. But the Switch sign-off and the iPhone device checks have no date, and later-milestone hand-offs (Android, signing, store listings) would hold the tag for work that is not part of v1.0 at all.
2. **Only the release flow gates it (#94).** The tag comes soonest, but the whole game would be released without anyone having played it to the end or run the Windows zip on a clean machine.
3. **The three desktop systems gate it; iPhone and Switch ship as preview** (the proposal on #308). The tag waits for a full playthrough on each desktop system, the clean Windows machine, the build guide, the crash dialog and the data folder. The iPhone app and the Switch zip ship with v1.0 but are labelled preview until their device sign-offs.

## Decision

Option 3, decided by Joris on #308.

**v1.0 is public.** `v1.0.0` is a published GitHub Release with the player builds and the port kits (ADR 0024, ADR 0100, ADR 0125), cut through a release PR (ADR 0126), and the same player builds on itch.io (ADR 0146). Nothing about it is private, and it never carries the game's assets.

**Windows, macOS and Linux gate the tag.** These hand-offs must be closed before `v1.0.0` is tagged:

- #79: all chapters, mini-games and endings in a human playthrough;
- #82: the Windows zip on a clean machine;
- #92: the playthrough checklist on every desktop system;
- #93: the build guide followed on a clean machine;
- #198: the crash dialog and the safe start on macOS and Windows;
- #264: the data folder on macOS, Windows and Linux.

#94 is the tag itself, and #208 (the itch.io page) is part of it, since v1.0 is on itch.io.

**iPhone and Switch are preview.** A preview platform ships with v1.0 and plays, but has not been signed off on the device yet. Its play page, the release notes and the itch.io page say "preview", and link the issues that lift it. The label comes off with a later release once these are closed:

- iPhone: #73 (controllers, touch and the safe area), #128 (app lifecycle), #197 (audio session);
- Switch: #245 (the sign-off on a real console, handheld and docked; its checklist covers the other Switch hand-offs).

**Not gates.** #60 (golden images from a device), #118 (font calibration), #149 (saves from a device backup), #272 (144 Hz pacing) and #84 (the Steam Deck, optional by its own text) were named by Joris. Every other open hand-off is not a gate either: they check features whose default or fallback is already covered by the gates (the GPU renderer, performance, input and window details), belong to a later milestone, or are now agent work. The per-issue list is in [`docs/testing/handoffs/README.md`](../testing/handoffs/README.md#v10-gates), and `python3 -m mtport.handoffs --live` fails when an open `needs-human` issue has no mark there.

**Judgement-only hand-offs are agent work.** Following CLAUDE.md's rule of 2026-11-13, the index marks the hand-offs that are only a judgement call or a repository setting as agent work; the agent does them and names the call in its PR so Joris can veto it there. Hardware runs, accounts and keys, and legal wording stay with Joris.

## Consequences

- #94, #106 and #114 are reworded to match (public, desktop gates, preview platforms); ROADMAP M11 and KNOWN_ISSUES use "preview" in the same sense.
- v1.0 waits on six hand-offs plus the release itself, all at a desk with a Mac, a Windows PC and Linux (the container or a PC). The Switch and the iPhone no longer hold it.
- A preview platform can ship a bug that its sign-off would have found. The label and the linked issues say so, and a fix goes out in a patch release.
- The play pages, the release notes header and the itch.io page still need the preview line: #394.
- About 30 hand-offs filed on 2026-11-13 have a gate mark but no device section yet, so `--live` still reports them as missing from the hand-off list; #395 adds the sections and drops `needs-human` from the ones that are now agent work.
- A new `needs-human` issue needs a mark in the index before `--live` passes, so the gate list stays current. If a later hand-off turns out to block players on a desktop system, it becomes a gate by a new ADR that supersedes this one's list.
- This decision would be wrong if players on a desktop system hit problems that one of the non-gate hand-offs (#131, #142, #132) would have caught; then those join the gates for the next release.
