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

## Local checks
<!-- Paste the per-job lines of the `scripts/local-ci` summary for the PR's head commit, and say what didn't run. -->
- macOS (`scripts/local-ci`, jobs lint / py-min / mac): …
- Linux x86_64 container (`scripts/local-ci`, job linux): …
- Windows (`scripts\test.ps1` on the PC; `-Release` if the build changed): … / not run
- Release workflows + package (`scripts/local-ci --release`), if the build changed: …
- Real assets (`work/ipa/` present): …

## Checklist
- [ ] Branch is named `epic-<epic-number>-<slug>`, with one `--no-ff` merge per child branch (`<issue-number>-<slug>`)
- [ ] No extracted game files, converted assets, generated code, Ghidra databases, built binaries or copyrighted screenshots outside `docs/` are committed
- [ ] New/changed file formats are specified in `docs/formats/` with a tested parser
- [ ] Significant design decisions are recorded as an ADR in `docs/adr/`
- [ ] Tests added/updated; `scripts/local-ci` passes on the head commit (and CI where it ran)
- [ ] Docs updated (`docs/ANALYSIS.md`, `docs/ARCHITECTURE.md`, `README.md`) if facts or design changed
- [ ] Follow-up issues created for anything out of scope: #

## Notes for review
<!-- Risks, open questions, open `needs-human` hand-offs. -->
