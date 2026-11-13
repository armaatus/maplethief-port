# Hand-off checklists

Everything that waits on Joris's hands (the `needs-human` issues), one page per device, so a session at one device clears every hand-off it can ([#283](https://github.com/armaatus/maplethief-port/issues/283)). Each page has its setup once at the top, then one section per issue in an order that avoids rebuilds; each section has numbered steps, what to post back, and the issue it closes. The steps were checked against `dev` on 2026-11-13; the issue comments they come from are older and sometimes name merged branches or old paths, and where they disagree this page wins.

| Device | Page | Issues |
|---|---|---|
| Windows PC | [windows.md](windows.md) | #113, #131, #141, #198, #142, #264, #192, #82, #85, #114 |
| Mac | [mac.md](mac.md) | #179, #196, #192, #198, #124, #142, #272, #264, #263 |
| iPhone (and an old iPod or iPhone with the original game) | [iphone.md](iphone.md) | #194, #197, #128, #73, #149, #60, #118 |
| Linux and Steam Deck | [linux.md](linux.md) | #264, #84, #185, #142 |
| Nintendo Switch | [switch.md](switch.md), with the [Switch sign-off checklist](../switch-checklist.md) | #235, #236, #243, #240, #239, #238, #241, #242, #244, #245, #279, #237 |
| Playthrough, review and release | [playthrough.md](playthrough.md) | #132, #93, #79, #223, #228, #92, #94, #208 |

Already done or obsolete, so their sections say how to close them: #114 (the v0.1.0 release went through the flow), #234 (ADR 0138 accepted and closed). Waiting on something else first: #279 (on the Switch's F1 row), #208 (on its child in epic #106), #94 (on the playthrough and review).

## How to report

1. Work through a page top to bottom; skip a section whose device or accessory you don't have, and say so.
2. Post the section's **Post back** items as a comment on the issue the section names, starting with the device, its OS version and the commit you built (`git rev-parse --short HEAD`). A section shared by several devices (#142, #192, #198, #264) gets one comment per device.
3. Paste text output; attach long logs as files. Never attach or commit anything of the game: no screenshots or photos of its picture (describe it in words, or show only the port's own menus where a step asks), no saves, no IPA, no extracted files ([ADR 0100](../../adr/0100-public-repository-and-player-releases.md)). Strip your own paths from logs if you like; the player log already writes them as `<data>` and `<game folder>`.
4. A failure is a result too: post what happened and the log, and the agent fixes it in a follow-up. Don't close an issue yourself unless its section says to; the agent closes it from your comment.

## The list

The committed list of hand-off issues and the pages that cover them. `tests/test_handoff_checklists.py` checks that each one has a section on exactly these pages (a level-2 heading naming it) and no other, that every repo path, script, preset and `just` recipe the pages name exists, and that the pages use no long dashes. `PYTHONPATH=tools python3 -m mtport.handoffs --live` also compares the list with GitHub's open `needs-human` issues (it needs `gh`; the test does the same with `MAPLETHIEF_LIVE_GH=1`). Add an issue here and a section on its page when a new hand-off appears. #237 isn't labelled `needs-human` but waits on a decision of Joris's.

<!-- handoffs:begin -->
- #60: iphone (golden images from the device)
- #73: iphone (controllers, touch and the safe area)
- #79: playthrough (full playthrough)
- #82: windows (the Windows build on a clean machine)
- #84: linux (the Steam Deck build)
- #85: windows (build from your IPA, Windows half)
- #92: playthrough (the playthrough on every platform)
- #93: playthrough (the build guide on a clean machine)
- #94: playthrough (release v1.0.0)
- #113: windows (build, test and package on Windows)
- #114: windows (release flow, already done)
- #118: iphone (font calibration references)
- #124: mac (audio listening check)
- #128: iphone (app lifecycle)
- #131: windows (the GPU backend on D3D12 and Vulkan)
- #132: playthrough (gamepad only and keyboard only)
- #141: windows (one-command build on Windows)
- #142: mac, windows, linux (performance on every platform)
- #149: iphone (import saves from a device backup)
- #179: mac (fullscreen first start, modes and Quit)
- #185: linux (gamepad pointer on the Deck's trackpad)
- #192: windows, mac (saves in the menu)
- #194: iphone (key pad haptics)
- #196: mac (window size hotkeys and the fullscreen display)
- #197: iphone (audio session)
- #198: windows, mac (crash dialog and safe start)
- #208: playthrough (itch.io page)
- #223: playthrough (five compendium quests in the port)
- #228: playthrough (compendium accuracy review)
- #235: switch (the hello NRO on the console)
- #236: switch (the runtime's test NRO)
- #237: switch (the emulator decision)
- #238: switch (the game to the first map)
- #239: switch (controllers and touch)
- #240: switch (lifecycle, audio and memory)
- #241: switch (the settings menu)
- #242: switch (the GPU renderer)
- #243: switch (the player build and the import)
- #244: switch (performance)
- #245: switch (the sign-off)
- #263: mac (the macOS kit under App Translocation)
- #264: windows, mac, linux (data folder sign-off)
- #272: mac (1 ms grid pacing on a 144 Hz panel)
- #279: switch (profile and tune)
<!-- handoffs:end -->
