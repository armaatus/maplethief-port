Closes #<epic>
Closes #<child>
<!-- One `Closes` line per completed child; `Refs #N` for a needs-human child that stays open. -->

## Summary
<!-- What the epic delivers and why, in a few sentences. -->

## Epic exit criteria
<!-- Copy the epic's exit criteria. For EACH one, say how it was verified on the epic branch. -->
- [ ] … *(verified by: …)*

## Acceptance criteria per child
<!-- One subsection per child issue. Copy its acceptance criteria; for EACH one, say how it was verified (command, test name, screenshot in docs/, manual check), rechecked after merging into the epic branch. -->
### #N Title
- [ ] … *(verified by: …)*

## Checks
<!-- The quick local check on the head commit (CLAUDE.md, "Checks and CI"; ADR 0126), and CI. PRs target `dev`; only a release PR (`Release vX.Y.Z`) targets `main`. -->
- Local (Mac): `scripts/lint` …; `mac-debug` build + tests …
- Real assets (`work/ipa/` present): …
- CI on this PR (into `dev`: lint, Python, Linux; into `main`: all OSes, Release): green / link
- All OSes, if the build or platform code changed (`gh workflow run ci.yml --ref <branch>`): … / not run

## Checklist
- [ ] Targets `dev`; branch is named `epic-<epic-number>-<slug>`, with one `--no-ff` merge per child branch (`<issue-number>-<slug>`)
- [ ] No extracted game files, converted assets, generated code, Ghidra databases, built binaries or copyrighted screenshots outside `docs/` are committed
- [ ] New/changed file formats are specified in `docs/formats/` with a tested parser
- [ ] Significant design decisions are recorded as an ADR in `docs/adr/`
- [ ] Tests added/updated; the quick local check passes on the head commit, and CI is green
- [ ] Docs updated (`docs/ANALYSIS.md`, `docs/ARCHITECTURE.md`, `README.md`) if facts or design changed
- [ ] Follow-up issues created for anything out of scope: #

## Notes for review
<!-- Risks, open questions, open `needs-human` hand-offs. -->
