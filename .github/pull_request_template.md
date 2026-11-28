Closes #
<!-- The issue this fixes. No issue? Say what the pull request is about in "What" below. -->

## What
<!-- What changes, in a sentence or two. -->

## Why
<!-- The bug or the goal: what was wrong or missing. -->

## How it was tested
<!-- Commands you ran and their result, test names added or changed. "Looks right" isn't evidence; a test, a command or a golden image is. For an issue's acceptance criteria, list each one with how it was checked. -->
- `scripts/lint`: …
- `scripts/test` (or `scripts\test.ps1` on Windows): …
- Real-asset tests that ran (they need `work/ipa/`; CI never runs them): … / none

## Checklist
- [ ] Targets `dev`
- [ ] Nothing from the game is committed: no IPA, extracted or converted files, generated code, built binaries or screenshots of game art (`git status` checked)
- [ ] Tests added or updated for the change, and docs updated if behaviour or facts changed

<!--
Epic pull requests (the maintainer's agent workflow, CLAUDE.md → "The loop"): open the
section below and fill it in. Everyone else leaves it as it is.
-->
<details>
<summary>Epic pull request (optional)</summary>

<!-- Add one `Closes #<child>` line per completed child next to `Closes #<epic>` at the top; `Refs #N` for a needs-human child that stays open. -->

### Epic exit criteria
<!-- Copy the epic's exit criteria. For EACH one, say how it was verified on the epic branch. -->
- [ ] … *(verified by: …)*

### Acceptance criteria per child
<!-- One subsection per child issue. Copy its acceptance criteria; for EACH one, say how it was verified (command, test name, screenshot in docs/, manual check), rechecked after merging into the epic branch. -->
#### #N Title
- [ ] … *(verified by: …)*

### Checks
<!-- The quick local check on the head commit (CLAUDE.md, "Checks and CI"; ADR 0126), and CI. -->
- Local (Mac): `scripts/lint` …; `mac-debug` build + tests …
- Real assets (`work/ipa/` present): …
- CI on this PR (into `dev`: lint, Python, Linux; into `main`: all OSes, Release): green / link
- All OSes, if the build or platform code changed (`gh workflow run ci.yml --ref <branch>`): … / not run

### Epic checklist
- [ ] Branch is named `epic-<epic-number>-<slug>`, with one `--no-ff` merge per child branch (`<issue-number>-<slug>`)
- [ ] New/changed file formats are specified in `docs/formats/` with a tested parser
- [ ] Significant design decisions are recorded as an ADR in `docs/adr/`
- [ ] Docs updated (`docs/ANALYSIS.md`, `docs/ARCHITECTURE.md`, `README.md`) if facts or design changed
- [ ] Follow-up issues created for anything out of scope: #

### Notes for review
<!-- Risks, open questions, open `needs-human` hand-offs. -->

</details>
