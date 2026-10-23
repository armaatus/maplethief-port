# 1. Record architecture decisions

- **Status:** Accepted
- **Date:** 2026-10-23
- **Deciders:** Joris (owner), build agent

## Context

This port is built over a long time, mostly by an AI agent working one issue at a time. Decisions such as recompiler design, memory model, the Objective-C strategy and the rendering backend have long-lasting consequences. A future session, human or agent, needs to know *why* something is the way it is without re-deriving it from code or chat history.

## Decision

Record every significant design decision as an **Architecture Decision Record** in `docs/adr/`, following Michael Nygard's format.

- File name: `NNNN-short-kebab-title.md`, with `NNNN` zero-padded and strictly increasing. Never reuse a number.
- An ADR is written in the same PR as the issue that makes the decision (issues labelled `type:adr` exist for the big ones).
- Status flow: `Proposed` → `Accepted` → optionally `Superseded by NNNN` or `Deprecated`. Accepted ADRs are never edited in substance. To change a decision, write a new ADR that supersedes the old one and update the old one's status line only.
- When an ADR changes the design described in `docs/ARCHITECTURE.md`, update that document in the same PR.

### Template

```markdown
# NNNN. Title

- **Status:** Proposed | Accepted | Superseded by NNNN | Deprecated
- **Date:** YYYY-MM-DD
- **Issue:** #N

## Context
What forces are at play: facts (link docs/ANALYSIS.md, docs/re/, measurements), constraints, requirements.

## Options considered
1. **Option A**: pros / cons / evidence
2. **Option B**: pros / cons / evidence

## Decision
What we will do, stated in full sentences.

## Consequences
What becomes easier or harder; follow-up issues created (#…); how we'd know the decision was wrong.
```

## Consequences

- Design rationale survives across sessions and contributors.
- There's a small overhead per decision, which is accepted.
- `docs/adr/` becomes the first place to look before re-opening a settled question.
