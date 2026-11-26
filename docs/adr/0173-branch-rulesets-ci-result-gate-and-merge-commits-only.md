# 173. Branch rulesets on dev and main: a `CI result` gate, merge commits only, no bypass

- **Status:** Accepted (delegated by Joris, 2026-11-26)
- **Date:** 2026-11-26
- **Issue:** [#329](https://github.com/armaatus/maplethief-port/issues/329) (epic [#288](https://github.com/armaatus/maplethief-port/issues/288))
- **Builds on:** [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (dev, release pull requests, CI by target: "CI is the gate"), [ADR 0006](0006-continuous-integration.md) (CI and `scripts/ci-scope`)

## Context

ADR 0126 made CI the gate, but nothing in the repository enforced it: neither branch had protection or a ruleset. A pull request was merged red once (#246), and epic pull requests were squashed (#119) although the rule is a merge commit; squash was still allowed in the settings.

A ruleset requires checks by the name CI reports. On a pull request into `dev` that `scripts/ci-scope` finds documentation-only, the build matrix is skipped before its matrix expands, and GitHub reports it as `${{ matrix.os }} ${{ matrix.config }}` (skipped), never as `ubuntu-latest debug` (seen on run 42244414598). Requiring `ubuntu-latest debug` on `dev` would leave every docs-only pull request waiting for a check that never comes. The same holds for every job behind an `if:`: `lint (clang-tidy)`, `linux asan+ubsan`, `switch`, `android`, `ios`.

## Decision

**A gate job.** `ci.yml` gains `ci-result`, named `CI result`: it needs every other job, runs with `if: always()`, and fails when any of them failed or was cancelled. A job the scope skipped counts as passing. Its `needs:` list is every job of the workflow (`tests/test_rulesets.py` checks this, so a new job can't be left out).

**dev's ruleset** (`.github/rulesets/dev.json`): a pull request is required (no approvals: one maintainer), merge commits only, no force-push, no deletion. Required checks: `scope`, `lint + Python min/latest` and `CI result`, the three jobs that report on every pull request into `dev`. Not strict: a branch need not be up to date with `dev` before merging, since an epic branch would otherwise rerun CI after every unrelated merge.

**main's ruleset** (`.github/rulesets/main.json`): the same rules, and every job of the full profile required by name: the six `{ubuntu,macos,windows}-latest {debug,release}` builds, `linux asan+ubsan`, `ios arm64 (...)`, `switch (...)`, `android (...)`, `lint (clang-tidy)`, `lint + Python min/latest`, `scope`, `CI result`, and `release title` from `release.yml`, which fails unless the pull request comes from `dev` (rulesets can't restrict the source branch themselves). So that these names always report, the `scope` job answers "build everything" on a pull request into `main`: a release pull request never skips the matrix.

**No bypass actors.** Not even the admin: the point is that a red pull request can't be merged by habit or by an agent with the owner's token. Changing a rule means changing the JSON and running `scripts/rulesets apply`, in a pull request where it is visible.

**Squash merging is off** (the proposal #329 asked for, decided under the delegation of 2026-11-13). Every pull request into `dev` or `main` is merged with a merge commit: the epic flow needs each child visible in history, `release.yml` tags the merge commit, and one method means no pull request is squashed by mistake. The repository settings match: `allow_squash_merge=false`, `allow_rebase_merge=false`, `allow_merge_commit=true`. A one-commit fix pull request costs one extra merge commit; that is accepted.

**The rulesets live in git.** `.github/rulesets/*.json` is what `POST/PUT repos/{owner}/{repo}/rulesets` takes; `scripts/rulesets apply` creates or updates each by name, `scripts/rulesets check` diffs them with what GitHub has. `tests/test_rulesets.py` checks every required name against the job names in `.github/workflows/`.

## Consequences

- A pull request with a failing or cancelled job can't be merged into `dev` or `main`; the docs-only pull request into `dev` still merges once its three checks pass.
- A new CI job joins `ci-result`'s `needs:` (the test fails otherwise), and, if it runs on a release pull request, `main.json`'s required checks.
- A renamed job must be renamed in the rulesets in the same pull request and applied after merging; until then the old name waits as "expected".
- Emergencies go through a pull request too. If a check is broken for reasons outside the change, fix the check or, visibly, edit the ruleset.
- Joris can veto the merge-commit-only choice in the pull request that carries this ADR; turning squash back on is one line in each JSON file and one repository setting.
