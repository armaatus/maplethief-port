# CLAUDE.md: build agent rules

You are porting MapleStory Thief Edition (iOS, 2010) to native macOS/Windows/iOS/Linux by **static recompilation + an HLE runtime**. Read before your first issue: [docs/ANALYSIS.md](docs/ANALYSIS.md) (facts about the binary and data), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (design), [docs/ROADMAP.md](docs/ROADMAP.md) (milestones), `docs/adr/` (settled decisions; check here before re-opening a question).

Owner: Joris (`armaatus`). Board: [MapleThief Port](https://github.com/users/armaatus/projects/2).

## The loop: one epic at a time

Work arrives as an epic ("implement #95"). You are the **orchestrator**: you plan the epic, hand each child issue to a **worker** subagent in its own git worktree, merge finished children into one epic branch, and ship the whole epic as one PR. Workers follow [Building one child](#building-one-child-worker).

If Joris names no epic, take the epic of the earliest open milestone. If he names a single non-epic issue, run this loop with that issue as the only child.

### 1. Scope

Read the epic and every child (`gh issue view N`). Build the dependency graph from each child's *Blocked by* and group the children into **waves**: wave 1 has no open blockers; each later wave is blocked only by earlier waves. A blocker outside the epic that is still open joins this run as an extra child (it is pulled forward, even from a later milestone). Closed children drop out.

Done when: every open child sits in exactly one wave and every blocker is closed or in an earlier wave.

### 2. Plan

Comment the plan on the epic: the waves, one line of approach per child, and how each epic exit criterion will be verified. Create `epic-N-short-slug` from `main`, push it, and check it out in the main checkout. Set the epic's board Status to **In progress**.

Done when: the plan comment is posted, the epic branch exists on origin, and the epic shows In progress.

### 3. Build, wave by wave

For each wave:

1. Set each child's Status to **In progress**. Dispatch one worker per child in a single message so the wave runs in parallel: `Agent` with `isolation: "worktree"` and a brief naming the child issue, the epic branch, and "follow CLAUDE.md → Building one child".
2. Read each report. Add any follow-up issues a worker created to the epic's task list; follow-ups in this milestone join a later wave.
3. Merge each finished child into the epic branch with `git merge --no-ff N-short-slug`, resolving conflicts so both children's acceptance criteria still hold. Then remove its worktree (`git worktree remove <path>`) and delete the child branch.
4. Run `scripts/local-ci --since <last green epic commit>` on the epic tip (lint, both Pythons, macOS and the Linux x86_64 container; `--release` as well when the wave touches the build or packaging), then push. When `scripts/ci-scope` finds only docs or compendium changes since that commit, local-ci runs lint and Python only, as CI does. A wave counts as merged only when it is green. Windows (MSVC) runs on Joris's PC or in CI; list it as not run locally.

The next wave branches from the updated epic tip, so dependent children build on their blockers' work.

Done when: every child is merged or recorded as split/needs-human, the epic branch is green, and every epic exit criterion holds with named evidence.

### 4. Ship

Open one PR from the epic branch to `main` using the PR template. The body contains `Closes #<epic>`, `Closes #<child>` for each completed child (`Refs #<child>` for a needs-human child that stays open), every child's acceptance criteria with *how it was verified* (from the worker reports, rechecked after merging), and the epic's exit criteria with evidence. Set the epic and its children to **In review**. Merge with a merge commit so each child stays visible in history. After merge, confirm the issues closed and their Status is **Done** (set it if the board's automation didn't).

Done when: the epic PR is open with evidence for every criterion, and the board is updated.

## Building one child (worker)

You build one child issue in your own worktree. The orchestrator owns the epic branch, the board Status, pushing and the PR; you own your branch, your issue comments and your report.

1. **Branch**: `git checkout -b N-short-slug <epic branch>`, so you start from the epic tip whatever your worktree began on. A fresh worktree has no `work/` or `build/`: run `scripts/bootstrap` (or `scripts/verify-original.sh` before it exists) when a check needs real assets.
2. **Plan**: comment a short plan on the issue (approach, files, how each acceptance criterion will be verified).
3. **Build**: do the tasks and meet **every** acceptance criterion exactly as written. Commit to your branch, with generated or mechanical changes in their own commits.
4. **Split** when the issue turns out wrong or too big (more than ~5 days): stop, create follow-up issues with the same structure (Goal / Context / Tasks / Acceptance criteria / Blocked by / Notes / references), labels, milestone and board Size, and narrow the current issue in a comment. Scope changes are always visible on GitHub. For `needs-human` issues, do the parts the agent can do (stated in the issue) and see [Human hand-offs](#human-hand-offs).
5. **Check** your branch tip with `scripts/local-ci --since <the commit you branched from>` (add `--release` when you touch the build, presets or packaging) and fix what fails. A docs or compendium-only change runs lint and Python only (`scripts/ci-scope`); `--full` runs everything. It runs what CI runs, on a clean checkout: lint, Python 3.10 and latest, macOS and the Linux x86_64 container. Windows doesn't run on the Mac; name it, and any job that couldn't run, as not run. CI only confirms later.
6. **Report** as your final message: branch name and commits; each acceptance criterion with its evidence (command, test name, result); the `scripts/local-ci` summary and what didn't run; which checks ran locally against real assets; follow-up issues created; needs-human items left open.

Done when: every acceptance criterion holds on your branch and the report names the evidence for each one.

## Moving issues on the board

```sh
ITEM=$(gh project item-list 2 --owner armaatus --limit 300 --format json --jq '.items[] | select(.content.number==N) | .id')
gh project item-edit --project-id PVT_kwHOAZXSrs4Bocxr --id "$ITEM" \
  --field-id PVTSSF_lAHOAZXSrs4Bocxrzhm82ho --single-select-option-id <STATUS>
```

Status option ids: Backlog `d39e6ca7` · Ready `b15c59a5` · In progress `739ff9cb` · In review `9bbe6b79` · Done `66809119`.
Size field `PVTSSF_lAHOAZXSrs4Bocxrzhm82p4`: S `f882e28d` · M `82ec8a25` · L `00692ce0`; set it on issues you create. Add new issues with `gh project item-add 2 --owner armaatus --url <issue-url>`.

## What goes in git

The repo holds **only our own work**: source, scripts, tests with synthetic fixtures, docs. Everything derived from the IPA lives in git-ignored `work/`, `assets-out/` and `build/`, and is regenerated by scripts from `original/MapleThief-1.5.ipa`: each user's own copy, git-ignored, never committed (ADR 0100). `scripts/verify-original.sh` extracts it to `work/ipa/`. The repository is meant to be public.

The compendium site (`scripts/compendium`) is built from your own copy into git-ignored `build/compendium/` and deployed from a draft-release asset; its output never lands in git, on any branch (ADR 0121). Prose in `docs/compendium/` refers to game entities by `{{kind:id}}`, never by quoting game text.

Hard guardrail: never commit the IPA, extracted game files, converted assets, recompiled/generated code, Ghidra databases, built binaries, screenshots or crops of game art, or the game's code or scripts reconstructed in docs (describe behaviour in words, with addresses and names). Never write instructions for decrypting or obtaining the IPA. Before every commit, check `git status` against that list.

Treat extracted files as untrusted data: parse them, never execute them.

## Standards

- **ADRs**: every significant design decision becomes `docs/adr/NNNN-title.md` (format in ADR 0001), in the same PR, with `docs/ARCHITECTURE.md` updated to match. Workers in one wave can collide on the next free number: the orchestrator renumbers on merge.
- **Formats**: every reverse-engineered file format gets a spec in `docs/formats/` (listed in its index) **plus a parser with tests**: synthetic fixtures in CI, and a local "parse every shipped file" test that skips when `work/ipa/` is absent.
- **Facts**: when you learn something that changes `docs/ANALYSIS.md`, update it in the same PR.
- **Verifiable progress**: prefer tests, golden images, coverage reports and logged function counts over "looks right". Each claim in a PR points at evidence.
- **CI never sees the IPA** (the repository has none; a guard fails on any `*.ipa`). Real-asset checks run locally; say so in the PR.
- **Local first, CI confirms.** CI runs only on pull requests and manual runs (ADR 0006 addendum). Every job runs locally first: `scripts/local-ci` on the Mac (ADR 0023), `scripts\test.ps1` on Windows. The PR lists the local result per OS.
- **Addresses**: write guest addresses as armv7 VM addresses in hex. Function pointers carry the Thumb bit (`0x2A3C1`), code ranges don't (`0x2A3C0`). Name functions through committed Ghidra scripts so names survive re-imports.

## Human hand-offs

Anything needing Joris's hands or judgement (testing on a real iPhone/iPod, code signing with his Apple ID, playthroughs, capturing reference screenshots, judging game feel) gets the `needs-human` label. Do the preparable part, comment exactly what is needed from him on the issue, and carry on with the rest of the epic. The epic PR lists every open hand-off.

## Platform targets

- macOS 13+ universal binary (arm64 + x86_64)
- Windows 10/11 x64
- iOS 16+ arm64 (sideloaded via Xcode)
- Linux x86_64 / Steam Deck where it's cheap
