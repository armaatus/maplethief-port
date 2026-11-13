# 148. Signing: the downloads stay unsigned, with no legal name on the binaries

- **Status:** Accepted (Joris, 2026-11-13, [on #347](https://github.com/armaatus/maplethief-port/issues/347#issuecomment-6854266168))
- **Date:** 2026-11-13
- **Issue:** [#347](https://github.com/armaatus/maplethief-port/issues/347) (epic [#291](https://github.com/armaatus/maplethief-port/issues/291)); closes [#348](https://github.com/armaatus/maplethief-port/issues/348) as not planned
- **Builds on:** [ADR 0024](0024-release-contents-and-flow.md) (its Consequences call Developer ID signing a new decision), [ADR 0064](0064-macos-game-app.md) (the ad-hoc signed app, Gatekeeper), [ADR 0100](0100-public-repository-and-player-releases.md) (the risk of shipping recompiled game code), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the player builds), [ADR 0127](0127-data-folder-next-to-the-game.md) (App Translocation)

## Context

The player builds (ADR 0125) are published on GitHub and itch.io (ADR 0146). The macOS app is ad-hoc signed over the whole bundle (ADR 0064); the Windows executable is not signed at all. A player who downloads either meets a warning the first time: Gatekeeper on macOS, SmartScreen on Windows. The question is whether to pay for signing that would soften those warnings, and whether the maintainer's legal name may sit on the binaries.

Facts, rechecked on 2026-11-13 against the vendors' own pages:

- **Every route that signs puts a person's legal name on binaries that contain the game's recompiled code.** ADR 0100 accepted the risk of shipping that code; a certificate turns the anonymous GitHub account into a named, identity-checked publisher of it. The one exception is SignPath Foundation, whose certificate names the foundation (below), and which is the least likely to accept this project.
- **macOS, Apple Developer Program.** 99 USD per membership year, in local currency at enrolment ([enrol](https://developer.apple.com/programs/enroll/)). Individuals can enrol with their personal legal name, which Apple shows as the seller. A Developer ID certificate can only be made by the team's Account Holder ([Developer ID](https://developer.apple.com/developer-id/)).
  - Notarisation is an automated scan of Developer ID signed software, not App Review ([Developer ID](https://developer.apple.com/developer-id/)). It needs the hardened runtime, which the app has off today (ADR 0064).
  - Without it, a quarantined ad-hoc app is blocked the first time. Since macOS 15 the player allows it in System Settings › Privacy & Security › **Open Anyway**; macOS 13 and 14 still have Control-click › **Open** (ADR 0064). `xattr -dr com.apple.quarantine` also clears it, and lets the game keep its data next to itself (ADR 0127).
  - Homebrew Cask no longer takes apps that fail Gatekeeper: Homebrew 5.0.0 deprecated unsigned casks and disables those that fail the check from 2026-09-01 ([Homebrew 5.0.0](https://brew.sh/2025/11/12/homebrew-5.0.0), [Acceptable Casks](https://docs.brew.sh/Acceptable-Casks)). A cask needs a notarised app.
- **Windows, SmartScreen.** Reputation is earned per file hash or per certificate over time: "several weeks and hundreds of clean installs", with no fixed threshold. A signed file can still warn until then, and "EV certificates no longer bypass SmartScreen" ([SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)). With a certificate the reputation carries over to the next release; an unsigned file starts over each time.
  - **Microsoft Artifact Signing** (formerly Trusted Signing): from 9.99 USD a month, no free or trial subscriptions ([FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)). Individual developers must be in the United States or Canada, identified by a government ID whose name matches the billing account ([quickstart](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart)). The maintainer is in the Netherlands, so it is closed to this project.
  - **SignPath Foundation**: free for open-source projects, but every component must be under an OSI licence and the project may hold no proprietary code; the certificate is issued to the foundation, each release is approved by hand, and acceptance is at its discretion ([terms](https://signpath.org/terms)). The player builds carry code translated from Nexon's program, which is neither open source nor ours, so the project does not qualify.
  - **Certum Open Source Code Signing**: 69 EUR for the set (smart card and reader, delivered by courier), issued to a natural person, with "Open Source Developer" before the person's name in the certificate; trusted by Microsoft, reputation included ([Certum shop](https://shop.certum.eu/open-source-code-signing.html)). The renewal price was not checked. Signing happens on the smart card, so only on the machine that holds it.
- **Comparable recompilation ports** ship ad-hoc signed macOS builds and unsigned Windows builds (survey in #347, not rechecked).

## Options considered

1. **Stay unsigned: macOS ad-hoc signed, Windows unsigned.** Cost: nothing. No legal name on the binaries, no credentials to keep, no change to the release flow. Players see the Gatekeeper block once (Open Anyway or Control-click › Open) and the SmartScreen warning on every release (More info › Run anyway). No Homebrew cask. **Chosen.**
2. **Notarise macOS only.** Cost: 99 USD a year. A Mac player opens the download like any app, with no Privacy & Security step; a Homebrew cask becomes possible; whether a notarised app in a zip is still translocated would need testing (#348). The maintainer's legal name sits on the Mac app as its Developer ID team. Work: the hardened runtime, `notarytool` and stapling in `scripts/release`, credentials in the keychain. Windows players keep the warning.
3. **Notarise macOS and sign Windows with a Certum certificate.** Cost: 99 USD a year plus 69 EUR for the first Certum set. The SmartScreen warning stays until the certificate has reputation, which a small fan project may take months to earn, so for most players the Windows step does not change at first. The maintainer's legal name is on both. Signing needs the smart card at the Windows build, which is cut by hand on Joris's PC (ADR 0126).
4. **Windows through Artifact Signing or SignPath Foundation.** Not available: Artifact Signing takes individuals only in the United States and Canada; SignPath Foundation requires the whole project to be open source.

### The legal paragraph

A code-signing certificate is an identity claim: Apple and Certum check the maintainer's identity, and the signed binary names that person to anyone who inspects it. The binaries contain Nexon's program, translated. ADR 0100 accepted that the project publishes them and set a withdrawal plan; it did not attach a real name to them. Signing would make the maintainer the named, verified publisher of each build, and a certificate authority or Apple could revoke the certificate on a rights holder's complaint, which would also affect anything else signed with it. Staying unsigned keeps the published name the GitHub account, as it is for the source.

## Decision

The downloads stay unsigned (Joris, 2026-11-13). The macOS app stays ad-hoc signed over the whole bundle with the hardened runtime off (ADR 0064), and the Windows executable stays unsigned. No Apple Developer Program membership and no Windows certificate is bought, and no legal name sits on the binaries.

Players get past the first-run warnings with the steps in [Troubleshooting](../play/troubleshooting.md#mac-apple-could-not-verify-maplethief-is-free-of-malware): **Open Anyway** in Privacy & Security on macOS 15 and later, Control-click › **Open** on macOS 13 and 14, and **More info** › **Run anyway** for SmartScreen. [KNOWN_ISSUES](../KNOWN_ISSUES.md) lists both warnings with this ADR as their source, and [legal.md](../legal.md) says why the downloads are unsigned.

#348 (a notarised macOS download) closes as not planned. There is no Homebrew cask.

## Consequences

- Nothing changes in the release flow or the builds; the release guard and `codesign --verify --deep --strict` on the Mac app stay as they are.
- Every release meets SmartScreen afresh, since an unsigned file's reputation does not carry over. The warnings are documented, not removed.
- A quarantined Mac app is still translocated and keeps its data in the user folder until the player clears the quarantine or moves it (ADR 0127).
- How we'd know this was wrong: players give up at the warnings (issues or itch.io comments that the game "is malware" or "won't open"), or a store or package manager that matters to players requires signing. Reopening it needs a new ADR that supersedes this one and accepts a legal name on the binaries; #348 holds the macOS work as it was planned.
