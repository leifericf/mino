# ADR 05: Changelog lines travel as proposal EDN, serialized at land time

Date: 2026-06-11

## Context

The skill system runs parallel agent fleets: multiple editors and
writers landing branches on a feature branch concurrently. Every unit
of work wants a changelog line, and `CHANGELOG.md` has exactly one
insertion point (`## Unreleased`) — if each branch edits it, every
integration is a guaranteed merge conflict. Version fields share the
problem. (The same lesson generalizes: any single-insertion-point
file is a serialization point in disguise.)

## Decision

Agents never edit `CHANGELOG.md` or version fields. Each branch
writes a proposal file (`<run-dir>/proposals/<branch-slug>.edn` with
`:branch`, `:changelog` lines, `:commits`), and the deterministic
spine script `tools/merge_proposals.clj` appends the lines under
`## Unreleased` exactly once at land time — in filename order,
deduplicated against the changelog and the run's `merged.edn`, so a
crashed run can re-merge safely. The round runner commits the merged
changelog on the feature branch.

## Consequences

- Zero changelog merge conflicts regardless of fleet width.
- Changelog updates are crash-resumable and idempotent.
- The changelog line is authored next to the change (in the proposal,
  by the agent that made it) but lands centrally and consistently.
- One more file format for editors to know — pinned by
  `tests/tooling_merge_proposals_test.clj` and stated in the editor
  agent's body.

## Alternatives

- **Let agents edit CHANGELOG.md, resolve conflicts at integrate
  time** — conflicts are guaranteed, and `integrate_fixes.clj`
  rightly refuses to resolve conflicts by guessing; every wave would
  escalate. Rejected.
- **git union merge driver for CHANGELOG.md** — resolves textual
  conflicts but produces unordered, duplicate-prone results and
  needs per-clone gitattributes setup. Rejected.
- **Generate the changelog from commit messages at release time** —
  commit subjects and user-facing changelog lines serve different
  readers; collapsing them degrades one or both (see the commit-form
  rules in the worktree model). Rejected.
