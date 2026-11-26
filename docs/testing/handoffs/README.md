# Hand-off checklists

Everything that waits on Joris's hands (the `needs-human` issues), one page per device, so a session at one device clears every hand-off it can ([#283](https://github.com/armaatus/maplethief-port/issues/283)). Each page has its setup once at the top, then one section per issue in an order that avoids rebuilds; each section has numbered steps, what to post back, and the issue it closes. The steps were checked against `dev` on 2026-11-13; the issue comments they come from are older and sometimes name merged branches or old paths, and where they disagree this page wins.

| Device | Page | Issues | Checks from closed issues |
|---|---|---|---|
| Windows PC | [windows.md](windows.md) | #113, #131, #141, #198, #142, #264, #192, #82, #85, #114, #314, #315, #368, #316, #352 | #145 |
| Mac | [mac.md](mac.md) | #179, #196, #192, #198, #124, #142, #272, #264, #263, #411, #342 | #59, #64, #145, #144, #89, #88, #170, #157, #87, #151, #153, #81, optional #174, #175, #204 |
| iPhone (and an old iPod or iPhone with the original game) | [iphone.md](iphone.md) | #194, #197, #128, #73, #338, #149, #60, #118 | #83, #144, #89, #159, #168, #76 |
| Linux and Steam Deck | [linux.md](linux.md) | #264, #84, #185, #142, #353 | #146, #145 |
| Nintendo Switch | [switch.md](switch.md), with the [Switch sign-off checklist](../switch-checklist.md) | #235, #236, #243, #240, #239, #238, #241, #242, #244, #245, #279, #417, #345, #351, #237 | |
| Android phone | [android.md](android.md) | #358, #360, #361 | |
| Playthrough, review and release | [playthrough.md](playthrough.md) | #132, #93, #79, #223, #228, #92, #326, #331, #318, #354, #343, #317, #94, #208 | #67, #72, calls for Joris on #156 and #162 |

Which of these hold up the v1.0 tag is in [v1.0 gates](#v10-gates) below ([ADR 0147](../../adr/0147-what-v1-is-and-what-gates-it.md)). Already done or obsolete, so their sections say how to close them: #114 (the v0.1.0 release went through the flow), #234 (ADR 0138 accepted and closed). Waiting on something else first: #279 (on the Switch's F1 row), #208 (on its child in epic #106), #94 (on the playthrough and review), and most hand-offs filed on 2026-11-13 (#314 to #373) and #417, on the agent's part of the same issue; their sections say which.

The Android hand-offs (#358, #360, #361) have their own page now rather than waiting for the Android epic (#292) to finish: the epic has started (#356 merged the toolchain), the three issues are open hand-offs already, and a page lets `--live` hold them like the others ([#395](https://github.com/armaatus/maplethief-port/issues/395)). Their sections wait on the emulator half of each issue.

Not on a page, since 2026-11-26: #329, #330, #367 and #373 lost `needs-human`, as they are agent work only (a repository setting, a look the agent checks, a judgement call); see [v1.0 gates](#v10-gates). #326 and #331 keep it only for approving their legal wording; their settings are agent work.

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
- #314: windows (the update notice on Windows)
- #315: windows (the Windows parity run)
- #316: windows (release verify on the PC)
- #317: playthrough (the v1.0.0-rc.1 rehearsal)
- #318: playthrough (the takedown runbook)
- #326: playthrough (the SECURITY.md wording)
- #331: playthrough (one legal paragraph)
- #338: iphone (the crash notice at the next start)
- #342: mac (the self-hosted runner for the nightly run)
- #343: playthrough (one release command on both machines)
- #345: switch (upstream patches for the SDL fork)
- #351: switch (the Homebrew App Store submission)
- #352: windows (Scoop and winget on a clean PC)
- #353: linux (the Deck layout template in Game Mode)
- #354: playthrough (the signing key for checksums)
- #358: android (the first map on a real phone)
- #360: android (a controller and the vibrator)
- #361: android (the signed release APK)
- #368: windows (fast-forward on D3D and GL)
- #411: mac (where presents land against a 120 Hz or VRR display's vblanks)
- #417: switch (a recovery point's stall on the console)
<!-- handoffs:end -->

## Checks from closed issues

Some issues closed with a hands-on check still asked of Joris: a look, a feel or a device ([#312](https://github.com/armaatus/maplethief-port/issues/312) gathered them from the hand-off lists of PRs #123, #126, #135, #178 and #229). They have a section on their device page like the open hand-offs, with steps brought up to date (the data folder beside the app, today's flags and menu names). Post back on the closed issue all the same; it stays closed, and a problem becomes a new issue. None of them gates v1.0, so they have no gate mark.

The list below names each one's page, or `dropped` with the reason. `tests/test_handoff_checklists.py` checks it like the hand-off list: a section on exactly these pages, none for a dropped one, and no issue on both lists. `--live` notes one that is an open `needs-human` issue again.

<!-- closed:begin -->
- #59: mac (integer scaling on a Retina display)
- #64: mac (mute when the window loses focus)
- #67: playthrough (real pads: hot-plug, focus and the dead zone)
- #72: playthrough (rumble: the game's vibration option and the strength)
- #76: iphone (physics and feel against the original on the device)
- #81: mac (the app on a real Intel Mac)
- #83: iphone (icon, launch screen, orientations, the Files app and the suspend autosave)
- #87: mac (smooth frames on 60, 120 and 144 Hz)
- #88: mac (how the filters and the HD text look)
- #89: mac, iphone (the settings menu with each input)
- #144: mac, iphone (the first-start preset picker)
- #145: mac, windows, linux (window behaviour)
- #146: linux (button prompts and focus on a TV or the Deck)
- #151: mac (game speeds and fast-forward at 60, 120 and 144 Hz)
- #153: mac (texture packs)
- #156: playthrough (a call for Joris: the cash-shop prices)
- #157: mac (blur and image borders)
- #159: iphone (the key pad editor with fingers)
- #162: playthrough (a call for Joris: main 20 never reaches done)
- #168: iphone (the key pad at the 16:9 edges)
- #170: mac (filtered widescreen wings)
- #174: mac (optional: how the scene skip feels)
- #175: mac (optional: how the auto-loot reach feels)
- #204: mac (optional: quick-save and quick-load in real time)
<!-- closed:end -->

Not carried over: the D3D12 and Vulkan halves of #87, #88, #157 and #170. Their GPU tests carry the `gpu` label, so [#131's run](windows.md#131-the-gpu-backend-on-d3d12-and-vulkan) covers them, and the look is the same picture as on the Mac. Windows MSVC for everything is [#113](windows.md#113-build-test-and-package-on-windows).

## v1.0 gates

What each open `needs-human` issue means for the `v1.0.0` tag ([ADR 0147](../../adr/0147-what-v1-is-and-what-gates-it.md), decided by Joris on [#308](https://github.com/armaatus/maplethief-port/issues/308)). Windows, macOS and Linux gate the tag; the iPhone app and the Switch build ship with it as **preview**: playable, but not signed off on the device yet, and labelled so on their play pages, in the release notes and on itch.io until their sign-offs close.

- **gate**: must be closed before `v1.0.0` is tagged.
- **preview**: lifts the preview label of its platform; it doesn't hold the tag.
- **not a gate**: doesn't hold the tag.
- **agent work**: since 2026-11-13 a hand-off that is only a judgement call (a design ADR, a default, a look the agent can check on its own runs) or a repository setting is the agent's to do (CLAUDE.md, "Human hand-offs"). The agent decides it, records it as "Accepted (delegated by Joris)" where it is an ADR, and names the call in its PR so Joris can veto it there. Hardware, accounts, keys, money and legal wording stay with Joris.

The list covers every open `needs-human` issue, including those of later milestones that have no device section yet. `PYTHONPATH=tools python3 -m mtport.handoffs --live` fails when an open one has no line here; add it when a new hand-off appears.

<!-- gates:begin -->
- #60: not a gate (golden images from a device; Joris)
- #73: preview (iPhone: controllers, touch and the safe area)
- #79: gate (full playthrough)
- #82: gate (the Windows zip on a clean machine)
- #84: not a gate (the Steam Deck, optional; Joris)
- #85: not a gate (build from your IPA on Windows; the clean-machine build is #93)
- #92: gate (the playthrough on every desktop system)
- #93: gate (the build guide on a clean machine)
- #94: gate (the tag itself)
- #113: not a gate (build, test and package on Windows; the zip is checked by #82)
- #114: not a gate (release flow, done with v0.1.0)
- #118: not a gate (font calibration; Joris)
- #124: not a gate (audio by ear; #79 plays every sound)
- #128: preview (iPhone: app lifecycle)
- #131: not a gate (the GPU renderer on D3D12 and Vulkan; the default is software)
- #132: not a gate (gamepad only and keyboard only)
- #141: not a gate (one-command build on Windows; the clean-machine build is #93)
- #142: not a gate (performance; #92 plays at default settings)
- #149: not a gate (saves from a device backup; Joris)
- #179: not a gate (fullscreen first start on macOS 27)
- #185: not a gate (gamepad pointer on real touchpads)
- #192: not a gate (the Saves tab's dialogs)
- #194: not a gate (iPhone key pad haptics)
- #196: not a gate (window size keys and a second display)
- #197: preview (iPhone: audio session)
- #198: gate (crash dialog and safe start)
- #208: gate (the itch.io page; v1.0 is on itch.io, part of #94)
- #223: not a gate (compendium quests in the port)
- #228: not a gate (the compendium review gates announcing the site, not the tag; its tone read is agent work)
- #235: not a gate (Switch hello NRO; feeds #245)
- #236: not a gate (Switch runtime test NRO; feeds #245)
- #237: not a gate, agent work (the emulator choice is a judgement call)
- #238: not a gate (Switch first map; feeds #245)
- #239: not a gate (Switch input; feeds #245)
- #240: not a gate (Switch lifecycle and audio; feeds #245)
- #241: not a gate (Switch settings menu; feeds #245)
- #242: not a gate (Switch GPU renderer; feeds #245)
- #243: not a gate (Switch player build and import; feeds #245)
- #244: not a gate (Switch performance; feeds #245)
- #245: preview (Switch: the sign-off)
- #263: not a gate (the macOS kit under App Translocation; the data folder is #264)
- #264: gate (data folder on all three systems)
- #272: not a gate (144 Hz pacing; Joris)
- #279: not a gate (Switch profile and tune)
- #308: not a gate (this decision, ADR 0147)
- #411: not a gate (vblank measurement on a 120 Hz or VRR display; Joris)
- #417: not a gate (a recovery point's stall on a Switch console; the Switch is preview)
- #314: not a gate (update notice on a real system)
- #315: not a gate (Windows parity run)
- #316: not a gate (release verify on the PC)
- #317: not a gate (the release candidate rehearsal; #94 runs the same flow)
- #318: not a gate (Joris approves the takedown runbook: legal wording)
- #326: not a gate (Joris approves the SECURITY.md wording; private vulnerability reporting is a repository setting, agent work)
- #329: not a gate, agent work (branch rulesets are a repository setting)
- #330: not a gate, agent work (Discussions is on; the API cannot pin a discussion, so Joris pins the FAQ, discussion #418, with one click)
- #331: not a gate (Joris approves the one legal paragraph; description and topics are repository settings, agent work)
- #334: not a gate, agent work (an ADR on traps)
- #338: not a gate (iOS crash notice on a device)
- #342: not a gate (the self-hosted runner on the Mac)
- #343: not a gate (one release command, cut on both machines)
- #345: not a gate (Switch upstream patches)
- #347: not a gate (signing, decided by Joris)
- #348: not a gate (notarisation needs the developer account)
- #351: not a gate (Homebrew App Store submission)
- #352: not a gate (Scoop and winget on a clean PC)
- #353: not a gate (the Deck's layout template)
- #354: not a gate (signed checksums need the key)
- #355: not a gate (Android, decided by Joris)
- #356: not a gate (Android on a phone)
- #357: not a gate (Android on a phone)
- #358: not a gate (Android on a phone)
- #359: not a gate (Android on a phone)
- #360: not a gate (Android on a phone)
- #361: not a gate (Android on a phone)
- #367: not a gate, agent work (smooth frames as a default is a look the agent checks on its own runs)
- #368: not a gate (session lines from Windows)
- #369: not a gate (packs, decided by Joris)
- #373: not a gate, agent work (the Dutch review is a judgement call)
- #379: not a gate (content fixes, decided by Joris)
<!-- gates:end -->
