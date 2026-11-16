# 137. The port's only network use: an opt-in update notice

- **Status:** Accepted
- **Date:** 2026-11-10
- **Issue:** #199 (epic #275)
- **Builds on:** [ADR 0034](0034-offline-services.md) (the game runs offline), [ADR 0024](0024-release-contents-and-flow.md) (versioned releases on GitHub), [ADR 0070](0070-settings-registry-and-presets.md) (the settings registry, the presets and the first-launch picker)

## Context

The game is offline: every online service of the 2010 game is emulated or fails fast (ADR 0034), and no part of the port has contacted a network. Since #182 most players run a downloaded build and have no way to learn that a newer one exists unless they check the releases page themselves.

Telling them needs one request to a server. That is the first network traffic the port would make, so it needs a decision on when it may happen and what it reveals, not only code. The constraints:

- The port must not go online without the player's consent.
- The Original preset is the iPhone game exactly as it was, and scripted runs must stay deterministic.
- SDL has no HTTP. A new library would have to pass ADR 0003, and CI must not go online.
- A response from the network is untrusted data.
- The app must keep building for iOS, where sideloaded builds are installed by hand.

## Options considered

1. **No check; the README says to look at the releases page.** No network at all, but most players never learn of a fix.
2. **A check that is on by default, with a way to turn it off.** Reaches everyone, but breaks the rule that the port does not go online without consent.
3. **An opt-in check, asked once at the first launch.** Reaches the players who want it and nobody else. Chosen.

For the HTTP itself:

1. **Bundle libcurl or an HTTP library** on every platform. One code path, but a new dependency with its own TLS stack, for one GET a day.
2. **Link the system's libcurl on Linux.** Simple code, but the player kits would then need it installed to start at all.
3. **Use each system's own HTTP, behind one interface:** NSURLSession on macOS and iOS, WinHTTP on Windows, and on Linux libcurl only if the system has it, loaded at run time. No dependency, the system's TLS and proxy settings, and a seam where tests replace the network. Chosen.

## Decision

**The setting.** `update_check = off | on` (`settings.ini`, `--update-check`, Other). It is off by default, is in no preset, and no preset turns it on. The first-launch picker asks once ("Check for new versions of the port? It contacts github.com once a day."), with "No" selected first so that nothing goes online by accident. The picker does not ask after the Original preset. A player with an existing settings file can turn it on in the settings menu.

**When it runs.** Only when the setting is on, the preset is not Original, and the run is interactive (`is_interactive_run`, which excludes every scripted and headless run). Anything else does nothing: no state file is read, no fetcher is made, no thread starts. `--safe-start` uses default settings, so it is off there too.

**What it does.** At most once a day, on its own thread, one GET of GitHub's "latest release" API for this repository. The time of the check is written to `update_check.txt` in the data folder before the request, so a failed check counts as that day's. The file also keeps the latest tag seen, so the notice survives a restart within the day. It downloads nothing else; it never installs or changes anything. Failures are silent apart from one log line.

**Trust.** The response is untrusted data. Only `tag_name`, `draft` and `prerelease` are read, by a small bounded reader; the tag must be valid SemVer, and drafts and pre-releases are skipped. The release page's address is built from the validated tag and the repository's own address. It is never taken from the response.

**Comparing versions.** SemVer 2.0.0 precedence against `MT_VERSION`. A build whose version is not SemVer makes no check.

**The notice.** One line in the title screen's corner, in the engine's title state only, and an action row in the settings menu (Other). Both open the release page (`SDL_OpenURL`) only when the player chooses them. Nothing shows during play: the settings menu row is there only when the player opens the menu themselves.

**Privacy.** Each check tells GitHub this device's IP address, the time, and a `User-Agent` of `MapleThief/<version>`, as any visit to the releases page would. The port sends no identifier, account, device name, save data, or settings, and keeps no cookies. GitHub's own [privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement) covers what it does with a request. With the setting off, the port makes no network request at all.

**HTTP.** One interface, `UpdateFetcher` (`platform/src/update_check.h`): `update_fetch_apple.mm` (NSURLSession, only API that both macOS and iOS have), `update_fetch.cpp` (WinHTTP; libcurl loaded through `SDL_LoadObject`, so it is neither a build nor a link dependency). A platform without one has no check. No library is added, so ADR 0003 and `THIRD_PARTY.md` are unchanged. Tests replace the fetcher and count its calls; CI never goes online.

## Consequences

- The port stays offline for everyone who does not opt in, and a test proves that a run with the setting off makes no call.
- The Linux check needs the system's libcurl. Without it the player sees nothing and the log says so.
- GitHub's unauthenticated API allows 60 requests an hour per address, far above one a day.
- A later release channel, or a change of repository, changes `kUpdateApiUrl` and `kUpdateReleasePage` only.
- We would know this was wrong if players ask for an in-app update or if the notice misses a release. Automatic updates would need a new ADR: this one decides that the port never downloads or installs anything.

## Addendum: the response cap while streaming, and HTTPS-only redirects on Apple (#413, 2026-11-13)

ADR 0154 found that the Apple fetcher kept the whole body before checking `kUpdateMaxResponseBytes` (4 MiB) and followed a redirect to any address. `update_fetch_apple.mm` now uses an `NSURLSession` data delegate. Each piece of the body goes through `UpdateBody` (`update_check.h`), which refuses the piece that would pass 4 MiB, and the task is cancelled then, or as soon as the response states a longer `Content-Length`. A redirect is followed only when `update_redirect_allowed` takes the new address (`https://` with a host); otherwise the fetch fails with that reason. libcurl's writer keeps its body through the same `UpdateBody`, and its redirects were already HTTPS only (#324). WinHTTP already stopped reading at the cap, and its default redirect policy never leaves HTTPS for HTTP. Tests (`update_check_test.cpp`): `update check: the response cap, 4 MiB, holds while the body streams (#413)`, `update check: a redirect is followed to an HTTPS address only (#413)`, and on macOS, against a loopback HTTP server, `update check: the Apple fetcher caps the body while it streams and follows HTTPS redirects only (#413)`.
